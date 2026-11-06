/*
 * app0stream: stream PS Vita games from a PC over the network.
 *
 * Shared declarations.  Everything here is portable C: the Vita build
 * implements the port layer with SceKernel and SceNetPs calls (port_vita.c),
 * the host test build with POSIX (tests/host/port_host.c).
 */

#ifndef A0S_H
#define A0S_H

#include <stddef.h>
#include <stdint.h>

#ifdef __vita__
#include <psp2common/types.h>
#else
typedef uint8_t  SceUInt8;
typedef uint16_t SceUInt16;
typedef uint32_t SceUInt32;
typedef uint64_t SceUInt64;
typedef int32_t  SceInt32;
#endif

#include "proto.h"

/* ---- port layer ---------------------------------------------------------- */

typedef int A0sMutex;       /* a mutex */
typedef int A0sSema;        /* a counting semaphore */
typedef int A0sEvent;       /* a binary event, cleared by its waiter */

int  a0sCreateMutex(A0sMutex *l, const char *name);
void a0sLockMutex(A0sMutex l);
void a0sUnlockMutex(A0sMutex l);

int  a0sCreateSema(A0sSema *s, const char *name, int init, int max);
void a0sWaitSema(A0sSema s);
void a0sSignalSema(A0sSema s);

int  a0sCreateEvent(A0sEvent *e, const char *name);
void a0sWaitEvent(A0sEvent e);
void a0sSetEvent(A0sEvent e);

void      a0sDelayThread(SceUInt32 us);
SceUInt64 a0sGetSystemTime(void);
int       a0sGetThreadId(void);

/* starts a thread running fn(arg); < 0 on failure */
int a0sStartThread(const char *name, int (*fn)(void *), void *arg, SceUInt32 stack);

void *a0sAlloc(SceUInt32 size);       /* small and zeroed */
void  a0sFree(void *p);
void *a0sAllocCache(SceUInt32 size);  /* large, page granular, never freed */

/* TCP, addresses in host byte order; every call returns < 0 on failure */
int a0sNetConnect(
	SceUInt32 ip,
	SceUInt16 port,
	SceUInt32 connectTimeoutUs,
	SceUInt32 ioTimeoutUs);

int  a0sNetSend(int s, const void *buf, SceUInt32 len);  /* bytes sent */
int  a0sNetRecv(int s, void *buf, SceUInt32 len);        /* bytes, 0 = closed */
void a0sNetClose(int s);

/* prints text to the console's TTY (serve's tty.data); a line at a time,
 * from a thread with a stack of its own */
void a0sDebugPrint(const char *text);

/* files on the console's own storage */
int a0sReadFile(const char *path, char *buf, SceUInt32 cap);    /* bytes read */
int a0sFileExists(const char *path);
int a0sAppendFile(const char *path, const char *buf, SceUInt32 len);

/* ---- logging (log.c) ----------------------------------------------------- */

void a0sLogInit(void);
void a0sLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* copies out what was logged since the last call; returns its length */
SceUInt32 a0sLogTake(char *out, SceUInt32 cap);

#define A0S_LOG(...) a0sLog(__VA_ARGS__)

/* sends a line to the server's log before returning, so the last one
 * written before a crash is on the PC.  Never call it from a worker */
void a0sTrace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* every file system request, when config.txt says trace = 1 */
extern volatile int g_a0sTrace;

#define A0S_TRACE(...)                          \
	do {                                    \
		if (g_a0sTrace)                 \
			a0sTrace(__VA_ARGS__);  \
	} while (0)

/* ---- configuration (config.c) -------------------------------------------- */

#define A0S_CONFIG_PATH  "ux0:data/app0stream/config.txt"
#define A0S_LOG_PATH     "ux0:data/app0stream/log.txt"

/* in a stub's top folder (ux0:app/TITLEID/ or ux0:patch/TITLEID/), it marks
 * the game as streamed; app0stream_server.py writes it */
#define A0S_MARKER       "app0stream.txt"
#define A0S_MAX_TITLES   128

typedef struct A0sConfig {
	SceUInt32 serverIp;     /* host byte order, 0 = not set */
	SceUInt16 port;
	SceUInt8  connections;  /* worker threads, one TCP connection each */
	SceUInt8  logToFile;
	SceUInt8  trace;
	SceUInt32 cacheKb;
	SceUInt32 readaheadKb;
	SceUInt32 timeoutSec;   /* how long a request is retried before EIO */

	SceUInt16 titleCount,
	          patchCount;
	char      titles[A0S_MAX_TITLES][10],
	          patches[A0S_MAX_TITLES][10];
} A0sConfig;

void a0sConfigSetDefaults(A0sConfig *c);

/* parses text into c, starting from the defaults; returns the number of
 * lines it could not understand */
int a0sConfigParse(A0sConfig *c, const char *text, SceUInt32 len);
int a0sConfigHasTitle(const A0sConfig *c, int patch, const char *titleid);
int a0sParseIpv4(const char *s, SceUInt32 len, SceUInt32 *ip);

/* ---- names (upcase.c) ---------------------------------------------------- */

/*
 * The path element at the start of path, decoded the way exFAT decodes it
 * for sceVfsOpDecodePathElem: upper-cased (ASCII, Latin-1, Latin Extended-A,
 * Greek, Cyrillic, fullwidth Latin).  Writes a terminated string to buf;
 * *elemLen gets the element's length in path, the return value the decoded
 * length, which may exceed cap - 1 (buf then holds a prefix).
 */
SceUInt32 a0sDecodePathElem(
	const char *path,
	char *buf,
	SceUInt32 cap,
	SceUInt32 *elemLen);

/* ---- client (client.c) --------------------------------------------------- */

typedef struct A0sStat {
	SceUInt32 type;           /* A0S_TYPE_* */
	SceUInt64 size;
	SceUInt64 id;

	SceUInt16 year, month, day,
	          hour, minute, second;
	SceUInt32 microsecond;

	SceUInt16 nameLen;
	char      name[A0S_MAX_NAME + 1];
} A0sStat;

typedef struct A0sBlock A0sBlock;

int a0sClientInit(const A0sConfig *c);
int a0sClientIsStarted(void);

/* takes a newly read configuration's server (from the next connection on) */
void a0sClientSetServer(SceUInt32 ip, SceUInt16 port, SceUInt32 timeoutSec);

/* drops every connection; later requests reconnect (after a resume) */
void a0sClientResume(void);

int a0sStatPath(const char *path, A0sStat *st);

/* hands a line to the server's log and waits for it to be taken */
int a0sSendLog(const char *text);

/* the same, retried for timeoutSec instead of the configured timeout */
int a0sStatPathTimeout(const char *path, A0sStat *st, SceUInt32 timeoutSec);

/* fills up to max records starting at index; returns the count or < 0 */
int a0sReadDirPath(const char *path, SceUInt32 index, A0sStat *out, SceUInt32 max);

/* parses one record; returns its total length or < 0 */
int a0sParseStat(const SceUInt8 *p, SceUInt32 len, A0sStat *st);

/*
 * Reads [offset, offset + len) of path into the cache blocks blks[0..n),
 * a block's worth each, and completes them (cache.c) whatever happens.
 * async: returns at once and a worker completes the blocks.  Returns the
 * bytes read (sync), 0 (async) or < 0.
 */
int a0sFetchBlocks(
	const char *path,
	SceUInt64 offset,
	SceUInt32 len,
	A0sBlock **blks,
	SceUInt32 n,
	int async);

/* ---- cache (cache.c) ----------------------------------------------------- */

#define A0S_MAX_SPAN 16     /* cache blocks per READ request */

typedef struct A0sReadState {
	SceUInt32 nextBlock;  /* the block a sequential reader wants next */
	SceUInt32 streak;     /* consecutive sequential accesses */
} A0sReadState;

int a0sCacheInit(SceUInt32 cacheKb, SceUInt32 readaheadKb);

/* reads from the file (path, id, size) through the cache; returns the bytes
 * read or < 0.  rs may be NULL */
int a0sCacheRead(
	const char *path,
	SceUInt64 id,
	SceUInt64 size,
	A0sReadState *rs,
	void *buf,
	SceUInt32 len,
	SceUInt64 offset);

/* the client finished a fetch; err < 0 on failure */
void a0sCacheComplete(A0sBlock **blks, SceUInt32 n, SceUInt32 got, int err);

SceUInt32 a0sCacheGetBlockSize(void);
SceUInt8 *a0sBlockGetData(A0sBlock *b);

/* ---- statistics ---------------------------------------------------------- */

typedef struct A0sStats {
	SceUInt32 requests,
	          reconnects,
	          errors;
	SceUInt64 bytes;
	SceUInt32 hits,
	          misses,
	          prefetches;
} A0sStats;

extern A0sStats g_a0sStats;

#endif
