/*
 * The port layer on the console: SceThreadmgrForDriver, SceSysmemForDriver,
 * SceNetPsForDriver and SceIofilemgrForDriver.
 *
 * Kernel sockets need no sceNetInit: sceNetPsSocket(..., kernel = 1) only
 * allocates a socket id (net_ps_socket.c), and SceNetPs is part of the boot
 * image (bootimage modules.cmake), resident before any taiHEN plugin.  It
 * returns negative errnos (sceNetPsUnlockResult), not SCE error codes.
 */

#include <psp2kern/kernel/debug.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/iofilemgr.h>
#include <psp2kern/netps.h>

#include "a0s.h"

static SceUID g_heap = -1;

/* ---- synchronisation ----------------------------------------------------- */

int a0sCreateMutex(A0sMutex *l, const char *name)
{
	SceUID u = ksceKernelCreateMutex(name, 0, 0, NULL);
	if (u < 0)
		return u;

	*l = u;
	return 0;
}

void a0sLockMutex(A0sMutex l)
{
	ksceKernelLockMutex(l, 1, NULL);
}

void a0sUnlockMutex(A0sMutex l)
{
	ksceKernelUnlockMutex(l, 1);
}

int a0sCreateSema(A0sSema *s, const char *name, int init, int max)
{
	SceUID u = ksceKernelCreateSema(name, 0, init, max, NULL);
	if (u < 0)
		return u;

	*s = u;
	return 0;
}

void a0sWaitSema(A0sSema s)
{
	ksceKernelWaitSema(s, 1, NULL);
}

void a0sSignalSema(A0sSema s)
{
	ksceKernelSignalSema(s, 1);
}

int a0sCreateEvent(A0sEvent *e, const char *name)
{
	SceUID u = ksceKernelCreateEventFlag(name, 0, 0, NULL);
	if (u < 0)
		return u;

	*e = u;
	return 0;
}

void a0sWaitEvent(A0sEvent e)
{
	ksceKernelWaitEventFlag(e, 1, SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR_PAT, NULL, NULL);
}

void a0sSetEvent(A0sEvent e)
{
	ksceKernelSetEventFlag(e, 1);
}

/* ---- time and threads ---------------------------------------------------- */

void a0sDelayThread(SceUInt32 us)
{
	ksceKernelDelayThread(us);
}

SceUInt64 a0sGetSystemTime(void)
{
	return (SceUInt64)ksceKernelGetSystemTimeWide();
}

int a0sGetThreadId(void)
{
	return ksceKernelGetThreadId();
}

void a0sDebugPrint(const char *text)
{
	ksceKernelPrintf("a0s: %s", text);
}

typedef struct {
	int (*fn)(void *);
	void *arg;
} A0sThreadStart;

static int threadEntry(SceSize args, void *argp)
{
	(void)args;

	A0sThreadStart *t = argp;

	return t->fn(t->arg);
}

int a0sStartThread(const char *name, int (*fn)(void *), void *arg, SceUInt32 stack)
{
	SceUID th = ksceKernelCreateThread(name, threadEntry, 0x10000100, stack, 0, 0, NULL);
	if (th < 0)
		return th;

	/* the kernel copies t onto the new thread's stack */
	A0sThreadStart t = { fn, arg };

	int r = ksceKernelStartThread(th, sizeof(t), &t);

	return r < 0 ? r : 0;
}

/* ---- memory -------------------------------------------------------------- */

static int heapInit(void)
{
	SceKernelHeapCreateOpt opt;

	__builtin_memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = SCE_KERNEL_HEAP_ATTR_HAS_AUTO_EXTEND;

	g_heap = ksceKernelCreateHeap("A0sHeap", 0x20000, &opt);

	return g_heap < 0 ? g_heap : 0;
}

void *a0sAlloc(SceUInt32 size)
{
	if (g_heap < 0 && heapInit() < 0)
		return NULL;

	void *p = ksceKernelAllocHeapMemory(g_heap, size);
	if (p)
		__builtin_memset(p, 0, size);

	return p;
}

void a0sFree(void *p)
{
	if (p)
		ksceKernelFreeHeapMemory(g_heap, p);
}

void *a0sAllocCache(SceUInt32 size)
{
	size = (size + 0xFFF) & ~0xFFFu;

	SceUID b = ksceKernelAllocMemBlock("A0sCache", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, size, NULL);
	if (b < 0) {
		A0S_LOG("cache of %u KiB: %08X\n", (unsigned)(size / 1024), (unsigned)b);
		return NULL;
	}

	void *base = NULL;
	if (ksceKernelGetMemBlockBase(b, &base) < 0)
		return NULL;

	return base;
}

/* ---- network ------------------------------------------------------------- */

static void setOpt(int s, int level, int name, int v)
{
	ksceNetSetsockopt(s, level, name, &v, sizeof(v));
}

int a0sNetConnect(
	SceUInt32 ip,
	SceUInt16 port,
	SceUInt32 connectTimeoutUs,
	SceUInt32 ioTimeoutUs)
{
	int s = ksceNetSocket("app0stream", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
	if (s < 0)
		return s;

	/* sceNetPsConnectSocket gives up on a blocking connect after the send
	 * timeout (so_snd.sb_timeo) */
	setOpt(s, SCE_NET_SOL_SOCKET,  SCE_NET_SO_SNDTIMEO, (int)connectTimeoutUs);
	setOpt(s, SCE_NET_SOL_SOCKET,  SCE_NET_SO_RCVTIMEO, (int)ioTimeoutUs);
	setOpt(s, SCE_NET_SOL_SOCKET,  SCE_NET_SO_RCVBUF,   256 * 1024);
	setOpt(s, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, 1);

	SceNetSockaddrIn addr;

	__builtin_memset(&addr, 0, sizeof(addr));
	addr.sin_len         = sizeof(addr);
	addr.sin_family      = SCE_NET_AF_INET;
	addr.sin_port        = __builtin_bswap16(port);
	addr.sin_addr.s_addr = __builtin_bswap32(ip);

	int r = ksceNetConnect(s, (const SceNetSockaddr *)&addr, sizeof(addr));
	if (r < 0) {
		ksceNetClose(s);
		return r;
	}

	setOpt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, (int)ioTimeoutUs);

	return s;
}

int a0sNetSend(int s, const void *buf, SceUInt32 len)
{
	return ksceNetSendto(s, buf, len, 0, NULL, 0);
}

int a0sNetRecv(int s, void *buf, SceUInt32 len)
{
	return ksceNetRecvfrom(s, buf, len, 0, NULL, NULL);
}

void a0sNetClose(int s)
{
	ksceNetClose(s);
}

/* ---- files --------------------------------------------------------------- */

int a0sReadFile(const char *path, char *buf, SceUInt32 cap)
{
	SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return fd;

	SceUInt32 total = 0;

	while (total < cap) {
		int n = ksceIoRead(fd, buf + total, cap - total);
		if (n <= 0)
			break;

		total += (SceUInt32)n;
	}

	ksceIoClose(fd);

	return (int)total;
}

int a0sAppendFile(const char *path, const char *buf, SceUInt32 len)
{
	SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd < 0)
		return fd;

	int r = ksceIoWrite(fd, buf, len);

	ksceIoClose(fd);

	return r;
}
