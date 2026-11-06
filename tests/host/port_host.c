/*
 * The port layer on a POSIX host, for testing the plugin's portable code
 * (client, cache, config, names) against app0stream_server.py.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "a0s.h"

enum { MAX_OBJECTS = 64 };

typedef struct {
	pthread_mutex_t mutex;
	pthread_cond_t  cond;
	int count,
	    max;
} HostObject;

static HostObject g_objects[MAX_OBJECTS];
static int        g_nobjects;

static pthread_mutex_t g_objectsLock = PTHREAD_MUTEX_INITIALIZER;

static int objectNew(int init, int max)
{
	pthread_mutex_lock(&g_objectsLock);

	int id = g_nobjects < MAX_OBJECTS ? g_nobjects++ : -1;

	pthread_mutex_unlock(&g_objectsLock);

	if (id < 0)
		return -1;

	HostObject *o = &g_objects[id];

	pthread_mutex_init(&o->mutex, NULL);
	pthread_cond_init(&o->cond, NULL);
	o->count = init;
	o->max   = max;

	return id;
}

/* ---- synchronisation ----------------------------------------------------- */

/* a mutex is a binary semaphore: the plugin never locks recursively */

int a0sCreateMutex(A0sMutex *l, const char *name)
{
	(void)name;

	int id = objectNew(1, 1);
	if (id < 0)
		return -1;

	*l = id;
	return 0;
}

static void down(int id)
{
	HostObject *o = &g_objects[id];

	pthread_mutex_lock(&o->mutex);
	while (o->count == 0)
		pthread_cond_wait(&o->cond, &o->mutex);
	--o->count;
	pthread_mutex_unlock(&o->mutex);
}

static void up(int id)
{
	HostObject *o = &g_objects[id];

	pthread_mutex_lock(&o->mutex);
	if (o->count < o->max)
		++o->count;
	pthread_cond_signal(&o->cond);
	pthread_mutex_unlock(&o->mutex);
}

void a0sLockMutex(A0sMutex l)   { down(l); }
void a0sUnlockMutex(A0sMutex l) { up(l); }

int a0sCreateSema(A0sSema *s, const char *name, int init, int max)
{
	(void)name;

	int id = objectNew(init, max);
	if (id < 0)
		return -1;

	*s = id;
	return 0;
}

void a0sWaitSema(A0sSema s)   { down(s); }
void a0sSignalSema(A0sSema s) { up(s); }

int a0sCreateEvent(A0sEvent *e, const char *name)
{
	(void)name;

	int id = objectNew(0, 1);
	if (id < 0)
		return -1;

	*e = id;
	return 0;
}

void a0sWaitEvent(A0sEvent e) { down(e); }
void a0sSetEvent(A0sEvent e)  { up(e); }

/* ---- time and threads ---------------------------------------------------- */

void a0sDelayThread(SceUInt32 us)
{
	usleep(us);
}

SceUInt64 a0sGetSystemTime(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (SceUInt64)ts.tv_sec * 1000000 + (SceUInt64)ts.tv_nsec / 1000;
}

int a0sGetThreadId(void)
{
	return (int)(uintptr_t)pthread_self();
}

void a0sDebugPrint(const char *text)
{
	(void)text;
}

typedef struct {
	int (*fn)(void *);
	void *arg;
} A0sThreadStart;

static void *threadEntry(void *p)
{
	A0sThreadStart t = *(A0sThreadStart *)p;
	free(p);

	t.fn(t.arg);

	return NULL;
}

int a0sStartThread(const char *name, int (*fn)(void *), void *arg, SceUInt32 stack)
{
	(void)name;
	(void)stack;

	A0sThreadStart *t = malloc(sizeof(*t));
	if (!t)
		return -1;

	t->fn  = fn;
	t->arg = arg;

	pthread_t th;
	if (pthread_create(&th, NULL, threadEntry, t) != 0) {
		free(t);
		return -1;
	}

	pthread_detach(th);

	return 0;
}

/* ---- memory -------------------------------------------------------------- */

void *a0sAlloc(SceUInt32 size)
{
	return calloc(1, size);
}

void a0sFree(void *p)
{
	free(p);
}

void *a0sAllocCache(SceUInt32 size)
{
	return aligned_alloc(4096, (size + 4095) & ~4095u);
}

/* ---- network ------------------------------------------------------------- */

static void setTimeout(int s, int name, SceUInt32 us)
{
	struct timeval tv = {
		.tv_sec  = us / 1000000,
		.tv_usec = us % 1000000,
	};

	setsockopt(s, SOL_SOCKET, name, &tv, sizeof(tv));
}

int a0sNetConnect(
	SceUInt32 ip,
	SceUInt16 port,
	SceUInt32 connectTimeoutUs,
	SceUInt32 ioTimeoutUs)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return -errno;

	int one = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	setTimeout(s, SO_SNDTIMEO, connectTimeoutUs);
	setTimeout(s, SO_RCVTIMEO, ioTimeoutUs);

	struct sockaddr_in addr = {
		.sin_family      = AF_INET,
		.sin_port        = htons(port),
		.sin_addr.s_addr = htonl(ip),
	};

	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int e = errno;
		close(s);
		return -e;
	}

	setTimeout(s, SO_SNDTIMEO, ioTimeoutUs);

	return s;
}

int a0sNetSend(int s, const void *buf, SceUInt32 len)
{
	ssize_t n = send(s, buf, len, MSG_NOSIGNAL);

	return n < 0 ? -errno : (int)n;
}

int a0sNetRecv(int s, void *buf, SceUInt32 len)
{
	ssize_t n = recv(s, buf, len, 0);

	return n < 0 ? -errno : (int)n;
}

void a0sNetClose(int s)
{
	close(s);
}

/* ---- files --------------------------------------------------------------- */

int a0sReadFile(const char *path, char *buf, SceUInt32 cap)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;

	size_t n = fread(buf, 1, cap, f);
	fclose(f);

	return (int)n;
}

int a0sFileExists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

int a0sAppendFile(const char *path, const char *buf, SceUInt32 len)
{
	FILE *f = fopen(path, "ab");
	if (!f)
		return -1;

	size_t n = fwrite(buf, 1, len, f);
	fclose(f);

	return (int)n;
}
