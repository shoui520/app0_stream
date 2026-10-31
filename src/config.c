/*
 * ux0:data/app0stream/config.txt
 *
 *   server       = 192.168.1.20   the PC running app0stream_server.py
 *   port         = 7350
 *   title        = PCSE00001      stream ux0:app/PCSE00001 (a line per game)
 *   patch        = PCSE00001      also stream ux0:patch/PCSE00001
 *   connections  = 2              parallel TCP connections, 1 to 4
 *   cache_kb     = 256            RAM cache, in kernel memory (games need it too)
 *   readahead_kb = 256            prefetch window for sequential reads
 *   timeout_s    = 120            retry a request this long before failing it
 *   log          = 1              write ux0:data/app0stream/log.txt
 *   trace        = 0              log every file system request
 *
 * '#' starts a comment.  Keys are case-insensitive.
 */

#include "a0s.h"

void a0sConfigSetDefaults(A0sConfig *c)
{
	__builtin_memset(c, 0, sizeof(*c));

	c->port        = A0S_DEFAULT_PORT;
	c->connections = 2;
	c->logToFile   = 1;
	c->cacheKb     = 256;
	c->readaheadKb = 256;
	c->timeoutSec  = 120;
}

static int isSpace(char ch)
{
	return ch == ' ' || ch == '\t' || ch == '\r';
}

static char lower(char ch)
{
	return ch >= 'A' && ch <= 'Z' ? (char)(ch + 0x20) : ch;
}

static SceUInt32 clamp(SceUInt32 v, SceUInt32 lo, SceUInt32 hi)
{
	return v < lo ? lo
	     : v > hi ? hi
	     : v;
}

static int keyIs(const char *k, SceUInt32 klen, const char *name)
{
	for (SceUInt32 i = 0; i < klen; ++i)
		if (name[i] == '\0' || lower(k[i]) != name[i])
			return 0;

	return name[klen] == '\0';
}

static int parseUint(const char *s, SceUInt32 len, SceUInt32 *out)
{
	if (len == 0 || len > 9)
		return -1;

	SceUInt32 v = 0;
	for (SceUInt32 i = 0; i < len; ++i) {
		if (s[i] < '0' || s[i] > '9')
			return -1;
		v = 10*v + (SceUInt32)(s[i] - '0');
	}

	*out = v;
	return 0;
}

int a0sParseIpv4(const char *s, SceUInt32 len, SceUInt32 *ip)
{
	SceUInt32 v      = 0,
	          part   = 0,
	          parts  = 0,
	          digits = 0;

	for (SceUInt32 i = 0; i <= len; ++i) {
		if (i == len || s[i] == '.') {
			if (digits == 0 || part > 255 || parts == 4)
				return -1;

			v = v << 8 | part;
			++parts;
			part = digits = 0;
		} else if (s[i] >= '0' && s[i] <= '9' && digits < 3) {
			part = 10*part + (SceUInt32)(s[i] - '0');
			++digits;
		} else {
			return -1;
		}
	}

	if (parts != 4)
		return -1;

	*ip = v;
	return 0;
}

/* a title ID: 4 letters and 5 digits (PCSE00001, NPXS10031) */
static int parseTitleId(const char *s, SceUInt32 len, char out[10])
{
	if (len != 9)
		return -1;

	for (SceUInt32 i = 0; i < 9; ++i) {
		char ch = s[i];

		if (ch >= 'a' && ch <= 'z')
			ch = (char)(ch - 0x20);

		int ok = i < 4 ? ch >= 'A' && ch <= 'Z'
		               : ch >= '0' && ch <= '9';
		if (!ok)
			return -1;

		out[i] = ch;
	}

	out[9] = '\0';
	return 0;
}

static int addTitle(char (*list)[10], SceUInt16 *n, const char *v, SceUInt32 vlen)
{
	char id[10];

	if (parseTitleId(v, vlen, id) < 0 || *n >= A0S_MAX_TITLES)
		return -1;

	__builtin_memcpy(list[(*n)++], id, 10);
	return 0;
}

static int set(A0sConfig *c, const char *k, SceUInt32 klen, const char *v, SceUInt32 vlen)
{
	if (keyIs(k, klen, "server"))
		return a0sParseIpv4(v, vlen, &c->serverIp);
	if (keyIs(k, klen, "title"))
		return addTitle(c->titles, &c->titleCount, v, vlen);
	if (keyIs(k, klen, "patch"))
		return addTitle(c->patches, &c->patchCount, v, vlen);

	/* the rest are numbers */

	SceUInt32 n;
	if (parseUint(v, vlen, &n) < 0)
		return -1;

	if (keyIs(k, klen, "port")) {
		if (n == 0 || n > 65535)
			return -1;
		c->port = (SceUInt16)n;
	} else if (keyIs(k, klen, "connections")) {
		c->connections = (SceUInt8)clamp(n, 1, 4);
	} else if (keyIs(k, klen, "cache_kb")) {
		c->cacheKb = clamp(n, 256, 16384);
	} else if (keyIs(k, klen, "readahead_kb")) {
		c->readaheadKb = clamp(n, 0, 1024);
	} else if (keyIs(k, klen, "timeout_s")) {
		c->timeoutSec = clamp(n, 5, 3600);
	} else if (keyIs(k, klen, "log")) {
		c->logToFile = n != 0;
	} else if (keyIs(k, klen, "trace")) {
		c->trace = n != 0;
	} else {
		return -1;
	}

	return 0;
}

int a0sConfigParse(A0sConfig *c, const char *text, SceUInt32 len)
{
	a0sConfigSetDefaults(c);

	int bad = 0;

	for (SceUInt32 i = 0; i < len; ++i) {
		SceUInt32 start = i;
		while (i < len && text[i] != '\n')
			++i;

		/* the line without its comment */
		SceUInt32 end = start;
		while (end < i && text[end] != '#')
			++end;

		SceUInt32 ks = start;
		while (ks < end && isSpace(text[ks]))
			++ks;

		if (ks == end)
			continue;   /* blank */

		SceUInt32 eq = ks;
		while (eq < end && text[eq] != '=')
			++eq;

		if (eq == end) {
			++bad;
			continue;
		}

		SceUInt32 ke = eq,
		          vs = eq + 1,
		          ve = end;

		while (ke > ks && isSpace(text[ke - 1]))
			--ke;
		while (vs < ve && isSpace(text[vs]))
			++vs;
		while (ve > vs && isSpace(text[ve - 1]))
			--ve;

		if (set(c, text + ks, ke - ks, text + vs, ve - vs) < 0)
			++bad;
	}

	return bad;
}

int a0sConfigHasTitle(const A0sConfig *c, int patch, const char *titleid)
{
	const char (*list)[10] = patch ? c->patches    : c->titles;
	SceUInt16 n            = patch ? c->patchCount : c->titleCount;

	for (SceUInt16 i = 0; i < n; ++i) {
		int k = 0;
		while (k < 9 && list[i][k] == titleid[k])
			++k;

		if (k == 9)
			return 1;
	}

	return 0;
}
