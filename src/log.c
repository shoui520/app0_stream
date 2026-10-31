/*
 * Logging into a RAM ring.
 *
 * Messages come from VFS operations, which run with vnode locks held and on
 * small borrowed stacks, so nothing here does file I/O: main.c's log thread
 * writes the ring out to ux0: once a second.  Trace lines go to the server
 * instead, at once, so that a crash cannot hold them back.
 */

#include <stdarg.h>
#include <stdio.h>

#include "a0s.h"

#define RING 8192

static char g_ring[RING];

static SceUInt32 g_head,  /* bytes ever written */
                 g_tail;  /* bytes ever taken */

static A0sMutex g_lock = -1;

volatile int g_a0sTrace;

void a0sLogInit(void)
{
	if (g_lock < 0)
		a0sCreateMutex(&g_lock, "A0sLog");
}

#define LINE 192

/* "[seconds.ms] text\n" into line; returns its length, 0 if nothing */
static int formatLine(char *line, const char *fmt, va_list ap)
{
	SceUInt64 ms = a0sGetSystemTime() / 1000;

	int n = snprintf(line, LINE, "[%5u.%03u] ", (unsigned)(ms / 1000), (unsigned)(ms % 1000));
	if (n < 0)
		return 0;

	int m = vsnprintf(line + n, LINE - (SceUInt32)n, fmt, ap);
	if (m < 0)
		return 0;

	/* truncated or not, the line ends with a newline */
	n += m;
	if (n > LINE - 1)
		n = LINE - 1;

	if (line[n - 1] != '\n') {
		if (n == LINE - 1)
			--n;
		line[n++] = '\n';
	}

	line[n] = '\0';
	return n;
}

static void ringPut(const char *line, int n)
{
	if (g_lock < 0)
		return;

	a0sLockMutex(g_lock);

	for (int i = 0; i < n; ++i)
		g_ring[g_head++ % RING] = line[i];

	/* the oldest text is lost */
	if (g_head - g_tail > RING)
		g_tail = g_head - RING;

	a0sUnlockMutex(g_lock);
}

void a0sLog(const char *fmt, ...)
{
	char line[LINE];

	va_list ap;
	va_start(ap, fmt);
	int n = formatLine(line, fmt, ap);
	va_end(ap);

	ringPut(line, n);
}

void a0sTrace(const char *fmt, ...)
{
	char line[LINE];

	va_list ap;
	va_start(ap, fmt);
	int n = formatLine(line, fmt, ap);
	va_end(ap);

	if (n == 0)
		return;

	/* not into the ring: at one line per read it would crowd everything else
	 * out of the TTY, and the server has it anyway.  It logs it without the
	 * newline */
	line[n - 1] = '\0';
	a0sSendLog(line);
}

SceUInt32 a0sLogTake(char *out, SceUInt32 cap)
{
	if (g_lock < 0)
		return 0;

	SceUInt32 n = 0;

	a0sLockMutex(g_lock);
	while (g_tail != g_head && n < cap)
		out[n++] = g_ring[g_tail++ % RING];
	a0sUnlockMutex(g_lock);

	return n;
}
