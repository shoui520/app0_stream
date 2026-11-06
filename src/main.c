/*
 * app0stream.skprx: play PS Vita games whose data stays on a PC.
 *
 * How a game's files reach it, from the psp2 sources (appmgr mount.c):
 * appmgr mounts an application's directory, ux0:app/TITLEID, with
 * scePfsMount.  PFS content (an encrypted game: sce_pfs/ or sce_sys/clearsign
 * present, appmgrIsPfsContent) is mounted in mode 2 with the klicensee, other
 * content in mode 20.  The eboot is loaded through that mount
 * (appmgrPrepareProcessContent resolves it to "<drive>:eboot.bin"), and app0:
 * is that mount.  A patch is mounted over it from ux0:patch/TITLEID (mount id
 * 1001, mode 2).
 *
 * The hook below changes the lower path scePfsMount is given: for a
 * streamed title, net0:/.m<n>/app/TITLEID instead of ux0:app/TITLEID (a
 * root of its own for every mount, see vfs.c).  A title is streamed when its
 * stub carries the marker file (A0S_MARKER) or config.txt lists it, so a
 * game is launched from its LiveArea bubble like any other.  PFS stacks
 * on net0: exactly as it would on the memory card and decrypts on the
 * console.  The content record appmgr keeps (appDirectory) still says ux0:,
 * so the patch path, the add-on content path, the license lookup
 * (ux0:license/...) and every other check behave as for an installed game.
 * Only the PFS mount's own record of its lower path is changed to net0:, as
 * the eboot is opened through it (see pfsMountHook).  Only appmgr calls
 * scePfsMount, which it imports from ScePfsMgrForKernel (library NID
 * 0xA067B56F, function 0xA772209C in appmgr/fw/3.65-CEX.cmake).
 *
 * appmgrMount shares a PFS mount between the processes that mount the same
 * lower path.  It cannot find a streamed one by its changed record, so every
 * process gets a PFS mount of its own; the block cache is shared.
 *
 * What stays on the memory card is a stub of the game: sce_sys/ (param.sfo,
 * the package header and the license files appmgr reads straight from
 * ux0:app/TITLEID) and sce_pfs/ (so appmgrIsPfsContent still sees PFS
 * content), plus the marker.  app0stream_server.py makes it.
 */

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/iofilemgr.h>
#include <psp2kern/kernel/suspend.h>
#include <taihen.h>

#include <stdio.h>
#include <string.h>

#include "a0s.h"
#include "netps_fix.h"
#include "vfs.h"

#define PFS_MGR_NID       0xA067B56F    /* ScePfsMgrForKernel */
#define PFS_MOUNT_NID     0xA772209C    /* scePfsMount */
#define PFS_MODE_CHECK    0x28          /* scePfsMount mode with no path */
#define PFS_RECORD_SIZE   292           /* AppmgrPfsMount appPath */
#define PROBE_TIMEOUT_S   10
#define LOG_TICK_US       (50 * 1000)
#define LOG_TICKS_PER_S   20
#define LOG_DIR           "ux0:data/app0stream"

static tai_hook_ref_t g_pfsRef;
static SceUID         g_pfsHook = -1;
static SceUInt32      g_mountSeq;

static A0sMutex  g_lock = -1;
static A0sConfig g_config;
static char      g_configText[8192];

static volatile int g_logToFile = 1;
static int          g_netOk;

static int isUpperOrDigit(char ch)
{
	return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
}

static int startsWith(const char *s, const char *prefix)
{
	while (*prefix)
		if (*s++ != *prefix++)
			return 0;

	return 1;
}

/*
 * "ux0:app/PCSE00001", "ux0:/app/PCSE00001" (appmgrNormalizePath puts a
 * slash after the drive), with or without a trailing slash, and the same
 * under patch/.  Returns 0 and the title ID, or -1.
 */
static int parseTarget(const char *path, int *patch, char title[10])
{
	if (!startsWith(path, "ux0:"))
		return -1;

	const char *p = path + 4;
	while (*p == '/')
		++p;

	if (startsWith(p, "app/")) {
		*patch = 0;
		p += 4;
	} else if (startsWith(p, "patch/")) {
		*patch = 1;
		p += 6;
	} else {
		return -1;
	}

	for (int i = 0; i < 9; ++i) {
		if (!isUpperOrDigit(p[i]))
			return -1;
		title[i] = p[i];
	}

	title[9] = '\0';
	p += 9;

	if (*p == '/')
		++p;

	return *p == '\0' ? 0 : -1;
}

static void loadConfig(void)
{
	int n = a0sReadFile(A0S_CONFIG_PATH, g_configText, sizeof(g_configText) - 1);

	if (n < 0) {
		a0sConfigSetDefaults(&g_config);
		return;
	}

	int bad = a0sConfigParse(&g_config, g_configText, (SceUInt32)n);
	if (bad)
		A0S_LOG("config: %d line(s) not understood\n", bad);

	g_logToFile = g_config.logToFile;
	g_a0sTrace  = g_config.trace;
}

/* the stub's marker, or a title or patch line in config.txt */
static int isStreamed(int patch, const char *title)
{
	if (a0sConfigHasTitle(&g_config, patch, title))
		return 1;

	char marker[64];

	snprintf(marker, sizeof(marker), "ux0:%s/%s/" A0S_MARKER, patch ? "patch" : "app", title);

	return a0sFileExists(marker);
}

/* the client and the cache start with the first configuration that asks for
 * them; later ones only change the server */
static int ensureClient(void)
{
	const A0sConfig *c = &g_config;

	if (a0sClientIsStarted()) {
		a0sClientSetServer(c->serverIp, c->port, c->timeoutSec);
		return 0;
	}

	int r = a0sCacheInit(c->cacheKb, c->readaheadKb);
	if (r < 0) {
		A0S_LOG("cache init failed: %08X\n", (unsigned)r);
		return r;
	}

	r = a0sClientInit(c);
	if (r < 0) {
		A0S_LOG("client init failed: %08X\n", (unsigned)r);
		return r;
	}

	return 0;
}

/* the path scePfsMount gets instead of path; < 0 to leave it alone */
static int redirect(const char *path, char *out, SceUInt32 cap)
{
	char title[10];
	int  patch;

	if (parseTarget(path, &patch, title) < 0)
		return -1;

	a0sLockMutex(g_lock);

	loadConfig();

	int r = -1;

	if (!isStreamed(patch, title))
		goto out;

	if (!g_netOk) {
		A0S_LOG("%s is streamed, but kernel sockets cannot receive (see above)\n", title);
		goto out;
	}

	if (g_config.serverIp == 0) {
		A0S_LOG("%s is streamed, but no server is set in config.txt\n", title);
		goto out;
	}

	if (ensureClient() < 0)
		goto out;

	char remote[32];
	snprintf(remote, sizeof(remote), "%s/%s", patch ? "patch" : "app", title);

	/* a server that is off or lacks the game: the local stub is mounted
	 * and the launch fails cleanly instead of hanging */
	A0sStat st;

	r = a0sStatPathTimeout(remote, &st, PROBE_TIMEOUT_S);

	if (r < 0 || st.type != A0S_TYPE_DIR) {
		A0S_LOG("%s is not on the server (%08X), using the local copy\n",
			remote, (unsigned)r);
		r = -1;
		goto out;
	}

	++g_mountSeq;
	snprintf(out, cap, A0S_ASSIGN "/" A0S_ALIAS "%u/%s", (unsigned)g_mountSeq, remote);

	r = 0;

out:
	a0sUnlockMutex(g_lock);

	return r;
}

static int pfsMountHook(
	const char *path,
	const char *name,
	SceUInt64 authId,
	const void *klicensee,
	SceUInt32 mode)
{
	char lower[48];

	int streamed = mode != PFS_MODE_CHECK
	            && path
	            && redirect(path, lower, sizeof(lower)) == 0;

	if (!streamed)
		return TAI_CONTINUE(int, g_pfsRef, path, name, authId, klicensee, mode);

	int r = TAI_CONTINUE(int, g_pfsRef, lower, name, authId, klicensee, mode);

	A0S_LOG("scePfsMount(%s as %s, %s, mode %u) = %08X\n",
		path, lower, name ? name : "-", (unsigned)mode, (unsigned)r);

	/*
	 * PFS covers the directory it was given (vfs_mount_pfs enters its root
	 * in the parent's name cache), so only the net0: directory reads
	 * decrypted now; ux0:app/TITLE is the stub's raw ciphertext.  appmgr turns the
	 * eboot's "PD...:eboot.bin" back into a lower path from its record of
	 * the mount (appmgrResolvePfsDevicePath) for sceAppMgrGetEbootParam and
	 * sceKernelCreateProcess, and path is that record: AppmgrPfsMount's
	 * appPath[292].  Pointing it at net0: sends both through PFS.
	 */
	if (r == 0)
		strncpy((char *)path, lower, PFS_RECORD_SIZE - 1);

	return r;
}

/* on resume every connection is stale: the Wi-Fi went down with the console */
static int sysEventHandler(int resume, int eventid, void *args, void *opt)
{
	(void)eventid;
	(void)args;
	(void)opt;

	if (resume)
		a0sClientResume();

	return 0;
}

static void logStats(void)
{
	A0sStats s = g_a0sStats;

	A0S_LOG("stats: %u requests, %u KiB, %u hits, %u misses, %u prefetches, "
		"%u reconnects, %u errors\n",
		(unsigned)s.requests,
		(unsigned)(s.bytes / 1024),
		(unsigned)s.hits,
		(unsigned)s.misses,
		(unsigned)s.prefetches,
		(unsigned)s.reconnects,
		(unsigned)s.errors);
}

/*
 * Drains the log ring every 50 ms: to the TTY at once, so that serve sees
 * the last lines even when the console goes down, and to ux0: once a
 * second.  While games stream, a line of statistics every half a minute.
 */
static int logThread(void *arg)
{
	(void)arg;

	static char chunk[2048],
	            line[256],
	            pending[8192];

	SceUInt32 npending     = 0,
	          lastRequests = 0;

	ksceIoMkdir(LOG_DIR, 0777);

	for (SceUInt32 tick = 1; ; ++tick) {
		a0sDelayThread(LOG_TICK_US);

		if (tick % (30 * LOG_TICKS_PER_S) == 0 && g_a0sStats.requests != lastRequests) {
			lastRequests = g_a0sStats.requests;
			logStats();
		}

		SceUInt32 n;
		while ((n = a0sLogTake(chunk, sizeof(chunk))) > 0) {
			for (SceUInt32 i = 0; i < n; ) {
				SceUInt32 len = 0;

				while (i < n && len < sizeof(line) - 1) {
					line[len++] = chunk[i++];
					if (line[len - 1] == '\n')
						break;
				}

				line[len] = '\0';
				a0sDebugPrint(line);
			}

			if (g_logToFile && npending + n <= sizeof(pending)) {
				memcpy(pending + npending, chunk, n);
				npending += n;
			}
		}

		if (npending && (tick % LOG_TICKS_PER_S == 0 || npending > sizeof(pending) / 2)) {
			a0sAppendFile(A0S_LOG_PATH, pending, npending);
			npending = 0;
		}
	}

	return 0;
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void *argp)
{
	(void)args;
	(void)argp;

	a0sLogInit();

	/* without it no reply from the server can be received; streaming is
	 * then refused and games start from the memory card as usual */
	g_netOk = a0sNetPsFix() >= 0;

	int r = a0sCreateMutex(&g_lock, "A0sMain");
	if (r < 0)
		return SCE_KERNEL_START_FAILED;

	r = a0sVfsRegister();
	if (r < 0) {
		A0S_LOG("ksceVfsAddVfs: %08X\n", (unsigned)r);
		return SCE_KERNEL_START_FAILED;
	}

	r = a0sVfsMount();
	if (r < 0) {
		A0S_LOG("vfsMount(" A0S_ASSIGN "): %08X\n", (unsigned)r);
		return SCE_KERNEL_START_FAILED;
	}

	g_pfsHook = taiHookFunctionExportForKernel(
		KERNEL_PID, &g_pfsRef, "ScePfsMgr", PFS_MGR_NID, PFS_MOUNT_NID, pfsMountHook);

	if (g_pfsHook < 0) {
		A0S_LOG("hooking scePfsMount: %08X\n", (unsigned)g_pfsHook);
		return SCE_KERNEL_START_FAILED;
	}

	r = ksceKernelRegisterSysEventHandler("A0sSysEvent", sysEventHandler, NULL);
	if (r < 0)
		A0S_LOG("sysevent handler: %08X (no reconnect on resume)\n", (unsigned)r);

	r = a0sStartThread("A0sLog", logThread, NULL, 0x2000);
	if (r < 0)
		return SCE_KERNEL_START_FAILED;

	A0S_LOG("app0stream ready, " A0S_ASSIGN " mounted\n");

	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp)
{
	(void)args;
	(void)argp;

	/* net0: stays mounted and PFS may be reading from it: never unload */
	return SCE_KERNEL_STOP_CANCEL;
}
