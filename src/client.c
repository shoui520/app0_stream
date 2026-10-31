/*
 * The network client: worker threads, each owning one TCP connection to the
 * server, and a queue of jobs they take requests from.
 *
 * VFS operations never touch the network themselves.  iofilemgr runs them on
 * borrowed 8 KiB stacks (SceThreadmgrForDriver_E54FD746(0x2000, ...) in the
 * psp2 sources' iofilemgr.c), PFS adds its own frames on top, and the SceNetPs
 * stack under a socket call is deep.  So a caller fills a job slot, queues it
 * and sleeps; a worker with a stack of its own does the I/O.
 *
 * Failures are retried: the connection is dropped, made again and the request
 * resent until timeout_s (config.txt) has passed, counted from the last
 * resume, so that a console that sleeps mid-request is not charged for the
 * sleep.  SceNetPs bounds a blocking connect() with SO_SNDTIMEO
 * (sceNetPsConnectSocket waits on so_snd.sb_timeo) and, when the network is
 * down, has the Wi-Fi reconnect on its own (sceNetPsRequestAutoConnect).
 *
 * Kernel buffers can be received into only once netps_fix.c has patched
 * SceNetPs (main.c does that first).
 */

#include "a0s.h"

#define MAX_WORKERS         4
#define NJOBS               16
#define SCRATCH             4096
#define CONNECT_TIMEOUT_US  (5 * 1000 * 1000)
#define IO_TIMEOUT_US       (15 * 1000 * 1000)
#define MAX_RETRY_DELAY_US  (2 * 1000 * 1000)

typedef struct A0sJob {
	struct A0sJob *next;

	/* the request */
	int       op;
	SceUInt16 pathLen;
	char      path[A0S_MAX_PATH + 1];
	SceUInt32 arg0;
	SceUInt64 arg1;
	SceUInt32 timeoutSec;   /* 0: the configured one */

	/* READ: the blocks the data goes to */
	A0sBlock *blks[A0S_MAX_SPAN];
	SceUInt32 nblks;
	int       async;

	/* STAT, READDIR: where the payload goes */
	SceUInt8 *out;
	SceUInt32 outCap;

	/* the answer */
	int       status;
	SceUInt32 outLen;
	A0sEvent  done;
} A0sJob;

typedef struct A0sWorker {
	int       index;
	int       sock;
	SceUInt32 epoch;    /* g_epoch when sock was connected */
	SceUInt8 *scratch;  /* for payloads nobody wants */
} A0sWorker;

static A0sJob  g_jobs[NJOBS];
static A0sJob *g_free,
              *g_pending,
              *g_pendingTail;

static A0sMutex g_qlock = -1;
static A0sSema  g_qsema = -1;    /* counts pending jobs */

static A0sWorker g_workers[MAX_WORKERS];
static int       g_started;

static volatile SceUInt32 g_serverIp,
                          g_timeoutSec,
                          g_epoch,        /* bumped on resume and server change */
                          g_seq;
static volatile SceUInt16 g_serverPort;

A0sStats g_a0sStats;

/* ---- little-endian packing ----------------------------------------------- */

static void put16(SceUInt8 *p, SceUInt32 v)
{
	p[0] = (SceUInt8)v;
	p[1] = (SceUInt8)(v >> 8);
}

static void put32(SceUInt8 *p, SceUInt32 v)
{
	put16(p,     v);
	put16(p + 2, v >> 16);
}

static void put64(SceUInt8 *p, SceUInt64 v)
{
	put32(p,     (SceUInt32)v);
	put32(p + 4, (SceUInt32)(v >> 32));
}

static SceUInt32 get16(const SceUInt8 *p)
{
	return (SceUInt32)p[0] | (SceUInt32)p[1] << 8;
}

static SceUInt32 get32(const SceUInt8 *p)
{
	return get16(p) | get16(p + 2) << 16;
}

static SceUInt64 get64(const SceUInt8 *p)
{
	return (SceUInt64)get32(p) | (SceUInt64)get32(p + 4) << 32;
}

int a0sParseStat(const SceUInt8 *p, SceUInt32 len, A0sStat *st)
{
	if (len < A0S_STAT_SIZE)
		return -1;

	SceUInt32 n = get16(p + 40);
	if (n > A0S_MAX_NAME || len < A0S_STAT_SIZE + n)
		return -1;

	st->type        = get32(p);
	st->size        = get64(p + 8);
	st->id          = get64(p + 16);
	st->year        = (SceUInt16)get16(p + 24);
	st->month       = (SceUInt16)get16(p + 26);
	st->day         = (SceUInt16)get16(p + 28);
	st->hour        = (SceUInt16)get16(p + 30);
	st->minute      = (SceUInt16)get16(p + 32);
	st->second      = (SceUInt16)get16(p + 34);
	st->microsecond = get32(p + 36);
	st->nameLen     = (SceUInt16)n;

	__builtin_memcpy(st->name, p + A0S_STAT_SIZE, n);
	st->name[n] = '\0';

	return (int)(A0S_STAT_SIZE + n);
}

/* ---- connection ---------------------------------------------------------- */

static int sendAll(int s, const void *buf, SceUInt32 len)
{
	const SceUInt8 *p = buf;

	while (len) {
		int n = a0sNetSend(s, p, len);
		if (n <= 0)
			return n < 0 ? n : -1;

		p   += n;
		len -= (SceUInt32)n;
	}

	return 0;
}

/* len bytes from s into dst; NULL discards them */
static int recvAll(A0sWorker *w, int s, void *dst, SceUInt32 len)
{
	SceUInt8 *p = dst;

	while (len) {
		SceUInt32 want = p || len < SCRATCH ? len : SCRATCH;

		/* 0: the server closed the connection */
		int n = a0sNetRecv(s, p ? p : w->scratch, want);
		if (n <= 0)
			return n < 0 ? n : -1;

		if (p)
			p += n;

		len -= (SceUInt32)n;
	}

	return 0;
}

static int sendRequest(
	int s,
	SceUInt32 seq,
	int op,
	const char *path,
	SceUInt32 pathLen,
	SceUInt32 arg0,
	SceUInt64 arg1)
{
	SceUInt8 hdr[A0S_REQ_SIZE + A0S_MAX_PATH];

	put32(hdr,      A0S_REQ_MAGIC);
	put32(hdr + 4,  seq);
	put16(hdr + 8,  (SceUInt32)op);
	put16(hdr + 10, pathLen);
	put32(hdr + 12, arg0);
	put64(hdr + 16, arg1);

	__builtin_memcpy(hdr + A0S_REQ_SIZE, path, pathLen);

	return sendAll(s, hdr, A0S_REQ_SIZE + pathLen);
}

static int recvHeader(A0sWorker *w, int s, SceUInt32 seq, int *status, SceUInt32 *len)
{
	SceUInt8 hdr[A0S_RSP_SIZE];

	int r = recvAll(w, s, hdr, sizeof(hdr));
	if (r < 0)
		return r;

	if (get32(hdr) != A0S_RSP_MAGIC || get32(hdr + 4) != seq) {
		A0S_LOG("bad response header (magic %08X seq %u, want %u)\n",
			(unsigned)get32(hdr), (unsigned)get32(hdr + 4), (unsigned)seq);
		return -1;
	}

	*status = (int)get32(hdr + 8);
	*len    = get32(hdr + 12);

	return 0;
}

static int drain(A0sWorker *w, SceUInt32 len)
{
	return recvAll(w, w->sock, NULL, len);
}

static void disconnect(A0sWorker *w)
{
	if (w->sock < 0)
		return;

	a0sNetClose(w->sock);
	w->sock = -1;
}

static int connectServer(A0sWorker *w)
{
	SceUInt32 ip    = g_serverIp,
	          epoch = g_epoch;
	SceUInt16 port  = g_serverPort;

	if (ip == 0)
		return A0S_EINVAL;

	int s = a0sNetConnect(ip, port, CONNECT_TIMEOUT_US, IO_TIMEOUT_US);
	if (s < 0)
		return s;

	/* HELLO: both sides speak the same protocol */

	SceUInt32 seq = ++g_seq, len = 0;
	int status = 0;

	int r = sendRequest(s, seq, A0S_OP_HELLO, "", 0, A0S_VERSION, 0);
	if (r >= 0)
		r = recvHeader(w, s, seq, &status, &len);

	if (r >= 0 && len != 0)
		r = -1;

	if (r >= 0 && status != A0S_VERSION) {
		A0S_LOG("server speaks protocol %d, plugin %d\n", status, A0S_VERSION);
		r = -1;
	}

	if (r < 0) {
		a0sNetClose(s);
		return r;
	}

	w->sock  = s;
	w->epoch = epoch;

	A0S_LOG("worker %d connected to %u.%u.%u.%u:%u\n",
		w->index,
		(unsigned)(ip >> 24),
		(unsigned)(ip >> 16 & 255),
		(unsigned)(ip >> 8 & 255),
		(unsigned)(ip & 255),
		(unsigned)port);

	return 0;
}

/* READ's payload, straight into the job's cache blocks */
static int recvRead(A0sWorker *w, A0sJob *j, SceUInt32 len)
{
	SceUInt32 bs  = a0sCacheGetBlockSize(),
	          got = 0;

	for (SceUInt32 i = 0; got < len && i < j->nblks; ++i) {
		SceUInt32 n = len - got < bs ? len - got : bs;

		int r = recvAll(w, w->sock, a0sBlockGetData(j->blks[i]), n);
		if (r < 0)
			return r;

		got += n;
	}

	j->outLen = got;
	g_a0sStats.bytes += got;

	return 0;
}

/* one go at a job: 0 when the server answered (j->status holds its status),
 * < 0 when the connection failed and the job is to be retried */
static int attempt(A0sWorker *w, A0sJob *j)
{
	SceUInt32 seq = ++g_seq, len;
	int status;

	int r = sendRequest(w->sock, seq, j->op, j->path, j->pathLen, j->arg0, j->arg1);
	if (r < 0)
		return r;

	r = recvHeader(w, w->sock, seq, &status, &len);
	if (r < 0)
		return r;

	++g_a0sStats.requests;

	j->status = status;
	j->outLen = 0;

	/* an error's payload is not worth reading, and more than was asked
	 * for is a broken server */
	SceUInt32 cap = j->op == A0S_OP_READ ? j->arg0 : j->outCap;

	if (status < 0 || len > cap) {
		if (status >= 0)
			j->status = A0S_EIO;
		return drain(w, len);
	}

	if (j->op == A0S_OP_READ)
		return recvRead(w, j, len);

	if (len) {
		r = recvAll(w, w->sock, j->out, len);
		if (r < 0)
			return r;
	}

	j->outLen = len;
	return 0;
}

static void runJob(A0sWorker *w, A0sJob *j)
{
	SceUInt32 timeoutSec = j->timeoutSec ? j->timeoutSec : g_timeoutSec,
	          epoch      = g_epoch,
	          delay      = 250 * 1000;

	SceUInt64 deadline = a0sGetSystemTime() + (SceUInt64)timeoutSec * 1000000;

	for (int failures = 0; ; ++failures) {
		/* resumed, or a new server */
		if (w->sock >= 0 && w->epoch != g_epoch)
			disconnect(w);

		int r = w->sock >= 0 ? 0 : connectServer(w);

		if (r >= 0) {
			r = attempt(w, j);
			if (r >= 0)
				return;

			disconnect(w);
		}

		if (failures == 0)
			A0S_LOG("worker %d: request failed (%08X), retrying\n", w->index, (unsigned)r);

		++g_a0sStats.reconnects;

		/* the clock restarts on resume */
		if (epoch != g_epoch) {
			epoch    = g_epoch;
			deadline = a0sGetSystemTime() + (SceUInt64)timeoutSec * 1000000;
		}

		if (a0sGetSystemTime() >= deadline) {
			A0S_LOG("worker %d: giving up after %u s\n", w->index, (unsigned)timeoutSec);
			++g_a0sStats.errors;

			j->status = A0S_EIO;
			j->outLen = 0;
			return;
		}

		a0sDelayThread(delay);

		if (delay < MAX_RETRY_DELAY_US)
			delay *= 2;
	}
}

/* ---- job queue ----------------------------------------------------------- */

static A0sJob *jobTryGet(void)
{
	a0sLockMutex(g_qlock);

	A0sJob *j = g_free;
	if (j)
		g_free = j->next;

	a0sUnlockMutex(g_qlock);

	return j;
}

static A0sJob *jobGet(void)
{
	A0sJob *j;

	/* every slot busy: rare */
	while (!(j = jobTryGet()))
		a0sDelayThread(1000);

	return j;
}

static void jobPut(A0sJob *j)
{
	a0sLockMutex(g_qlock);
	j->next = g_free;
	g_free  = j;
	a0sUnlockMutex(g_qlock);
}

static void jobSubmit(A0sJob *j)
{
	j->next = NULL;

	a0sLockMutex(g_qlock);

	if (g_pendingTail)
		g_pendingTail->next = j;
	else
		g_pending = j;

	g_pendingTail = j;

	a0sUnlockMutex(g_qlock);

	a0sSignalSema(g_qsema);
}

static A0sJob *jobTake(void)
{
	a0sLockMutex(g_qlock);

	A0sJob *j = g_pending;
	if (j) {
		g_pending = j->next;
		if (!g_pending)
			g_pendingTail = NULL;
	}

	a0sUnlockMutex(g_qlock);

	return j;
}

static int workerMain(void *arg)
{
	A0sWorker *w = arg;

	for (;;) {
		a0sWaitSema(g_qsema);

		A0sJob *j = jobTake();
		if (!j)
			continue;

		runJob(w, j);

		if (j->op == A0S_OP_READ)
			a0sCacheComplete(j->blks, j->nblks, j->outLen, j->status < 0 ? j->status : 0);

		if (j->async)
			jobPut(j);
		else
			a0sSetEvent(j->done);
	}

	return 0;
}

/* fills in a job; out may be NULL */
static int jobInit(
	A0sJob *j,
	int op,
	const char *path,
	SceUInt32 arg0,
	SceUInt64 arg1,
	SceUInt8 *out,
	SceUInt32 outCap)
{
	SceUInt32 n = 0;

	for (; path[n] != '\0'; ++n) {
		if (n == A0S_MAX_PATH)
			return A0S_EINVAL;
		j->path[n] = path[n];
	}

	j->path[n]    = '\0';
	j->pathLen    = (SceUInt16)n;
	j->op         = op;
	j->arg0       = arg0;
	j->arg1       = arg1;
	j->timeoutSec = 0;
	j->nblks      = 0;
	j->async      = 0;
	j->out        = out;
	j->outCap     = outCap;

	return 0;
}

/* submits a synchronous job and waits for it; returns its status */
static int jobRun(A0sJob *j)
{
	jobSubmit(j);
	a0sWaitEvent(j->done);

	return j->status;
}

/* ---- public -------------------------------------------------------------- */

int a0sClientInit(const A0sConfig *c)
{
	if (g_started)
		return 0;

	int r = a0sCreateMutex(&g_qlock, "A0sQueue");
	if (r < 0)
		return r;

	r = a0sCreateSema(&g_qsema, "A0sPending", 0, NJOBS);
	if (r < 0)
		return r;

	for (int i = 0; i < NJOBS; ++i) {
		r = a0sCreateEvent(&g_jobs[i].done, "A0sJobDone");
		if (r < 0)
			return r;

		g_jobs[i].next = g_free;
		g_free = &g_jobs[i];
	}

	a0sClientSetServer(c->serverIp, c->port, c->timeoutSec);

	int n = c->connections;
	if (n < 1)           n = 1;
	if (n > MAX_WORKERS) n = MAX_WORKERS;

	for (int i = 0; i < n; ++i) {
		A0sWorker *w = &g_workers[i];

		w->index   = i;
		w->sock    = -1;
		w->scratch = a0sAlloc(SCRATCH);

		if (!w->scratch)
			return -1;

		r = a0sStartThread("A0sWorker", workerMain, w, 0x4000);
		if (r < 0)
			return r;
	}

	g_started = 1;

	A0S_LOG("client: %d connection(s)\n", n);

	return 0;
}

int a0sClientIsStarted(void)
{
	return g_started;
}

void a0sClientSetServer(SceUInt32 ip, SceUInt16 port, SceUInt32 timeoutSec)
{
	g_timeoutSec = timeoutSec;

	if (ip == g_serverIp && port == g_serverPort)
		return;

	g_serverIp   = ip;
	g_serverPort = port;
	++g_epoch;
}

void a0sClientResume(void)
{
	++g_epoch;
}

int a0sStatPath(const char *path, A0sStat *st)
{
	return a0sStatPathTimeout(path, st, 0);
}

int a0sStatPathTimeout(const char *path, A0sStat *st, SceUInt32 timeoutSec)
{
	if (!g_started)
		return A0S_EIO;

	SceUInt8 buf[A0S_STAT_SIZE + A0S_MAX_NAME];

	A0sJob *j = jobGet();

	int r = jobInit(j, A0S_OP_STAT, path, 0, 0, buf, sizeof(buf));
	if (r >= 0) {
		j->timeoutSec = timeoutSec;
		r = jobRun(j);
	}

	if (r >= 0 && a0sParseStat(buf, j->outLen, st) < 0)
		r = A0S_EIO;

	jobPut(j);

	return r < 0 ? r : 0;
}

int a0sSendLog(const char *text)
{
	if (!g_started)
		return A0S_EIO;

	char line[A0S_MAX_PATH + 1];
	SceUInt32 n = 0;

	for (; text[n] != '\0' && n < A0S_MAX_PATH; ++n)
		line[n] = text[n];

	line[n] = '\0';

	A0sJob *j = jobGet();

	int r = jobInit(j, A0S_OP_LOG, line, 0, 0, NULL, 0);
	if (r >= 0)
		r = jobRun(j);

	jobPut(j);

	return r;
}

int a0sReadDirPath(const char *path, SceUInt32 index, A0sStat *out, SceUInt32 max)
{
	if (!g_started)
		return A0S_EIO;

	if (max > A0S_MAX_DIRENTS)
		max = A0S_MAX_DIRENTS;

	SceUInt32 cap = max * (A0S_STAT_SIZE + A0S_MAX_NAME);

	SceUInt8 *buf = a0sAlloc(cap);
	if (!buf)
		return A0S_EIO;

	A0sJob *j = jobGet();

	int r = jobInit(j, A0S_OP_READDIR, path, index, max, buf, cap);
	if (r >= 0)
		r = jobRun(j);

	if (r > (int)max)
		r = A0S_EIO;

	SceUInt32 off = 0;

	for (int i = 0; i < r; ++i) {
		int n = a0sParseStat(buf + off, j->outLen - off, &out[i]);
		if (n < 0) {
			r = A0S_EIO;
			break;
		}

		off += (SceUInt32)n;
	}

	jobPut(j);
	a0sFree(buf);

	return r;
}

int a0sFetchBlocks(
	const char *path,
	SceUInt64 offset,
	SceUInt32 len,
	A0sBlock **blks,
	SceUInt32 n,
	int async)
{
	/* until the job is queued, every failure completes the blocks here */

	int bad = !g_started
	       || n == 0
	       || n > A0S_MAX_SPAN
	       || len > A0S_MAX_READ
	       || len > n * a0sCacheGetBlockSize();

	/* no free slot for a prefetch: skip it */
	A0sJob *j = bad ? NULL : async ? jobTryGet() : jobGet();

	if (!j) {
		a0sCacheComplete(blks, n, 0, A0S_EIO);
		return A0S_EIO;
	}

	int r = jobInit(j, A0S_OP_READ, path, len, offset, NULL, 0);
	if (r < 0) {
		jobPut(j);
		a0sCacheComplete(blks, n, 0, r);
		return r;
	}

	for (SceUInt32 i = 0; i < n; ++i)
		j->blks[i] = blks[i];

	j->nblks = n;
	j->async = async;

	if (async) {
		jobSubmit(j);
		return 0;
	}

	r = jobRun(j);
	if (r >= 0)
		r = (int)j->outLen;

	jobPut(j);

	return r;
}
