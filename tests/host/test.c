/*
 * Host tests for the plugin's portable code.
 *
 *   test SERVER_IP PORT LIBRARY_DIR TITLEID
 *
 * Checks config parsing and name decoding offline, then reads every file of
 * LIBRARY_DIR/app/TITLEID through the real client and cache from a running
 * app0stream_server.py: sequentially, in random chunks and from several
 * threads at once, comparing each byte with the local copy.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "a0s.h"

static int g_failures;

#define CHECK(cond, ...)                                    \
	do {                                                    \
		if (!(cond)) {                                      \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
			printf(__VA_ARGS__);                            \
			printf("\n");                                   \
			++g_failures;                                   \
		}                                                   \
	} while (0)

/* ---- offline ------------------------------------------------------------- */

static void testConfig(void)
{
	static const char text[] =
		"# a comment\n"
		"server = 192.168.1.20\r\n"
		"PORT=7351\n"
		"title = pcse00001   # trailing comment\n"
		"title = PCSG00611\n"
		"patch = PCSG00611\n"
		"connections = 9\n"
		"cache_kb = 100\n"
		"bogus = 1\n"
		"title = TOOLONG123\n"
		"\n";

	A0sConfig c;
	int bad = a0sConfigParse(&c, text, sizeof(text) - 1);

	CHECK(bad == 2,                               "bad lines: %d", bad);
	CHECK(c.serverIp == 0xC0A80114,               "server %08X", c.serverIp);
	CHECK(c.port == 7351,                         "port %u", c.port);
	CHECK(c.titleCount == 2 && c.patchCount == 1, "titles %u, patches %u", c.titleCount, c.patchCount);
	CHECK(a0sConfigHasTitle(&c, 0, "PCSE00001"),  "title PCSE00001");
	CHECK(a0sConfigHasTitle(&c, 1, "PCSG00611"),  "patch PCSG00611");
	CHECK(!a0sConfigHasTitle(&c, 1, "PCSE00001"), "no patch PCSE00001");
	CHECK(c.connections == 4,                     "connections %u", c.connections);
	CHECK(c.cacheKb == 256,                       "cache_kb %u", c.cacheKb);

	SceUInt32 ip;
	CHECK(a0sParseIpv4("1.2.3", 5, &ip) < 0,     "short address");
	CHECK(a0sParseIpv4("1.2.3.256", 9, &ip) < 0, "octet range");
}

static void testDecode(void)
{
	static const struct {
		const char *in, *out;
		SceUInt32 elem;
	} cases[] = {
		{ "eboot.bin",               "EBOOT.BIN",                 9 },
		{ "sce_sys/param.sfo",       "SCE_SYS",                   7 },
		{ "\xc3\xa9t\xc3\xa9",       "\xc3\x89T\xc3\x89",         5 },  /* été */
		{ "\xd0\xb4\xd0\xb0",        "\xd0\x94\xd0\x90",          4 },  /* да */
		{ "\xef\xbd\x81",            "\xef\xbc\xa1",              3 },  /* fullwidth a */
		{ "\xff\x61",                "\xff" "A",                  2 },  /* not UTF-8 */
		{ "",                        "",                          0 },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
		char buf[64];
		SceUInt32 elem;
		SceUInt32 n = a0sDecodePathElem(cases[i].in, buf, sizeof(buf), &elem);

		CHECK(strcmp(buf, cases[i].out) == 0 && n == strlen(cases[i].out) && elem == cases[i].elem,
			"decode case %zu: \"%s\" (%u, elem %u)", i, buf, n, elem);
	}

	/* a buffer too small: a terminated prefix and the full length */
	char small[4];
	SceUInt32 elem;
	SceUInt32 n = a0sDecodePathElem("abcdef", small, sizeof(small), &elem);

	CHECK(n == 6 && strcmp(small, "ABC") == 0, "truncated decode: \"%s\" %u", small, n);
}

/* ---- against the server -------------------------------------------------- */

typedef struct {
	char      rel[A0S_MAX_PATH + 1];    /* as the plugin asks for it */
	char     *local;
	int       fd;           /* the local copy */
	SceUInt64 size,
	          id;
} TestFile;

static TestFile g_files[4096];
static int      g_nfiles;

/* walks the remote tree with READDIR, checking it against the local one */
static void walk(const char *rel, const char *local)
{
	A0sStat ents[64];
	SceUInt32 index = 0;

	for (;;) {
		int n = a0sReadDirPath(rel, index, ents, 64);
		CHECK(n >= 0, "readdir %s: %08X", rel, (unsigned)n);
		if (n <= 0)
			return;

		for (int i = 0; i < n; ++i) {
			A0sStat *e = &ents[i];

			char crel[A0S_MAX_PATH + 1], clocal[4096];
			snprintf(crel,   sizeof(crel),   "%s/%s", rel,   e->name);
			snprintf(clocal, sizeof(clocal), "%s/%s", local, e->name);

			struct stat st;
			CHECK(stat(clocal, &st) == 0, "%s is not local", clocal);

			if (e->type == A0S_TYPE_DIR) {
				walk(crel, clocal);
				continue;
			}

			CHECK((SceUInt64)st.st_size == e->size, "%s: size %llu, local %llu",
				crel, (unsigned long long)e->size, (unsigned long long)st.st_size);

			if (g_nfiles == (int)(sizeof(g_files) / sizeof(*g_files)))
				continue;

			TestFile *f = &g_files[g_nfiles++];

			/* ask in upper case, as PFS would after decoding */
			for (size_t k = 0; k <= strlen(crel); ++k)
				f->rel[k] = crel[k] >= 'a' && crel[k] <= 'z' ? crel[k] - 0x20 : crel[k];

			f->local = strdup(clocal);
			f->size  = e->size;
			f->id    = e->id;
		}

		index += (SceUInt32)n;
		if (n < 64)
			return;
	}
}

/* buf and ref hold len bytes each */
static int readCompare(
	TestFile *f,
	A0sReadState *rs,
	SceUInt64 off,
	SceUInt32 len,
	SceUInt8 *buf,
	SceUInt8 *ref)
{
	int r = a0sCacheRead(f->rel, f->id, f->size, rs, buf, len, off);

	SceUInt64 want = off >= f->size ? 0 : f->size - off < len ? f->size - off : len;

	if (r < 0 || (SceUInt64)r != want) {
		printf("FAIL read %s @%llu+%u: %d, want %llu\n",
			f->rel, (unsigned long long)off, len, r, (unsigned long long)want);
		return -1;
	}

	if (pread(f->fd, ref, (size_t)r, (off_t)off) != r || memcmp(buf, ref, (size_t)r) != 0) {
		printf("FAIL read %s @%llu+%u: wrong data\n", f->rel, (unsigned long long)off, len);
		return -1;
	}

	return 0;
}

/* reads files front to back, as PFS does, until budget bytes are read */
static void testSequential(SceUInt64 budget)
{
	enum { CHUNK = 0x8000 };

	static SceUInt8 buf[CHUNK], ref[CHUNK];

	SceUInt64 total = 0, t0 = a0sGetSystemTime();
	int nfiles = 0;

	for (int i = 0; i < g_nfiles && total < budget; ++i, ++nfiles) {
		TestFile *f = &g_files[i];
		A0sReadState rs = { .nextBlock = 0xFFFFFFFFu };

		for (SceUInt64 off = 0; off < f->size && total < budget; off += CHUNK) {
			if (readCompare(f, &rs, off, CHUNK, buf, ref) < 0) {
				++g_failures;
				break;
			}

			total += f->size - off < CHUNK ? f->size - off : CHUNK;
		}
	}

	double s = (double)(a0sGetSystemTime() - t0) / 1e6;

	printf("sequential: %d files, %.1f MiB in %.2f s (%.1f MiB/s)\n",
		nfiles, (double)total / 1048576, s, (double)total / 1048576 / s);
}

static SceUInt32 rnd(SceUInt32 *state)
{
	*state ^= *state << 13;
	*state ^= *state >> 17;
	*state ^= *state << 5;

	return *state;
}

typedef struct {
	SceUInt32 seed;
	int reads,
	    failures;
} RandomJob;

static volatile int g_randomLeft;

static int randomReader(void *arg)
{
	RandomJob *j = arg;

	SceUInt8 *buf = malloc(300 * 1024),
	         *ref = malloc(300 * 1024);

	for (int i = 0; i < j->reads; ++i) {
		TestFile *f = &g_files[rnd(&j->seed) % (SceUInt32)g_nfiles];

		SceUInt64 off = f->size ? rnd(&j->seed) % (f->size + 1) : 0;
		SceUInt32 len = 1 + rnd(&j->seed) % (300 * 1024);

		if (readCompare(f, NULL, off, len, buf, ref) < 0)
			++j->failures;
	}

	free(buf);
	free(ref);
	__sync_fetch_and_sub(&g_randomLeft, 1);

	return 0;
}

static void testRandom(int threads, int reads)
{
	RandomJob jobs[8];

	SceUInt64 t0 = a0sGetSystemTime();

	g_randomLeft = threads;

	for (int i = 0; i < threads; ++i) {
		jobs[i] = (RandomJob){ .seed = 0x9E3779B9u * (SceUInt32)(i + 1), .reads = reads };
		a0sStartThread("random", randomReader, &jobs[i], 0);
	}

	while (g_randomLeft)
		a0sDelayThread(10000);

	int failures = 0;
	for (int i = 0; i < threads; ++i)
		failures += jobs[i].failures;

	g_failures += failures;

	printf("random: %d threads x %d reads, %d failures, %.2f s\n",
		threads, reads, failures, (double)(a0sGetSystemTime() - t0) / 1e6);
}

static void testErrors(const char *title)
{
	A0sStat st;
	char path[64];

	snprintf(path, sizeof(path), "app/%s/NO_SUCH_FILE", title);
	int r = a0sStatPath(path, &st);
	CHECK(r == A0S_ENOENT, "stat missing: %08X", (unsigned)r);

	snprintf(path, sizeof(path), "app/%s/eboot.bin/x", title);
	r = a0sStatPath(path, &st);
	CHECK(r == A0S_ENOTDIR || r == A0S_ENOENT, "stat through a file: %08X", (unsigned)r);

	r = a0sStatPath("app/../app", &st);
	CHECK(r == A0S_EINVAL, "stat ..: %08X", (unsigned)r);

	snprintf(path, sizeof(path), "APP/%s", title);
	r = a0sStatPath(path, &st);
	CHECK(r == 0 && st.type == A0S_TYPE_DIR && strcmp(st.name, title) == 0,
		"stat title dir: %08X %s", (unsigned)r, st.name);
}

int main(int argc, char *argv[])
{
	if (argc != 5) {
		printf("usage: test SERVER_IP PORT LIBRARY_DIR TITLEID\n");
		return 2;
	}

	a0sLogInit();

	testConfig();
	testDecode();

	A0sConfig c;
	a0sConfigSetDefaults(&c);

	if (a0sParseIpv4(argv[1], (SceUInt32)strlen(argv[1]), &c.serverIp) < 0) {
		printf("bad address %s\n", argv[1]);
		return 2;
	}

	c.port        = (SceUInt16)atoi(argv[2]);
	c.connections = 3;
	c.cacheKb     = 4096;
	c.timeoutSec  = 10;

	if (a0sCacheInit(c.cacheKb, c.readaheadKb) < 0 || a0sClientInit(&c) < 0) {
		printf("init failed\n");
		return 1;
	}

	testErrors(argv[4]);

	char rel[64], local[4096];
	snprintf(rel,   sizeof(rel),   "app/%s", argv[4]);
	snprintf(local, sizeof(local), "%s/app/%s", argv[3], argv[4]);

	walk(rel, local);
	CHECK(g_nfiles > 0, "no files listed");

	for (int i = 0; i < g_nfiles; ++i) {
		g_files[i].fd = open(g_files[i].local, O_RDONLY);
		CHECK(g_files[i].fd >= 0, "opening %s", g_files[i].local);
	}

	if (g_failures == 0) {
		testSequential(256ull << 20);
		testRandom(4, 400);
	}

	A0sStats s = g_a0sStats;
	printf("stats: %u requests, %llu KiB, %u hits, %u misses, %u prefetches, %u reconnects\n",
		s.requests, (unsigned long long)(s.bytes / 1024), s.hits, s.misses, s.prefetches,
		s.reconnects);

	char log[8192];
	SceUInt32 n = a0sLogTake(log, sizeof(log) - 1);
	log[n] = '\0';
	printf("--- plugin log ---\n%s", log);

	printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);

	return g_failures != 0;
}
