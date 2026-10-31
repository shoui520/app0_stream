/*
 * The block cache.  Every read of a streamed file goes through it.
 *
 * Files are cut into BLOCK_SIZE blocks keyed by (file id, block index).  A
 * miss fetches the blocks the read needs in one READ request, up to
 * A0S_MAX_SPAN of them.  A reader that goes forward block after block gets
 * the next readahead window fetched in the background while it consumes the
 * current one, so a sequential stream (PFS reading an encrypted file, a
 * movie, an audio bank) does not wait a network round trip at every block.
 *
 * The data never changes on the server while a game runs, so blocks are
 * replaced but never invalidated.
 */

#include "a0s.h"

#define BLOCK_SIZE (64 * 1024)
#define BLOCK_KB   (BLOCK_SIZE / 1024)
#define NONE       0xFFFFFFFFu

enum { BLK_EMPTY, BLK_LOADING, BLK_VALID };

struct A0sBlock {
	SceUInt8 *data;

	SceUInt64 id;
	SceUInt32 index,
	          len,        /* valid bytes, fewer than BLOCK_SIZE at the end */
	          lru,
	          state;
};

static A0sBlock  *g_blks;
static SceUInt32  g_nblks,
                  g_ra,       /* readahead, in blocks */
                  g_clock;

static A0sMutex g_lock = -1;

SceUInt32 a0sCacheGetBlockSize(void)
{
	return BLOCK_SIZE;
}

SceUInt8 *a0sBlockGetData(A0sBlock *b)
{
	return b->data;
}

int a0sCacheInit(SceUInt32 cacheKb, SceUInt32 readaheadKb)
{
	if (g_blks)
		return 0;

	int r = a0sCreateMutex(&g_lock, "A0sCache");
	if (r < 0)
		return r;

	SceUInt32 n = cacheKb / BLOCK_KB;
	if (n < 4)
		n = 4;

	/* a smaller cache rather than none when kernel memory is short */
	SceUInt8 *data = NULL;
	for (; n >= 4; n /= 2) {
		data = a0sAllocCache(n * BLOCK_SIZE);
		if (data)
			break;
	}

	if (!data)
		return -1;

	g_blks = a0sAlloc(n * sizeof(*g_blks));
	if (!g_blks)
		return -1;

	for (SceUInt32 i = 0; i < n; ++i)
		g_blks[i].data = data + (size_t)i * BLOCK_SIZE;

	/* a window must not evict itself */
	SceUInt32 ra = (readaheadKb + BLOCK_KB - 1) / BLOCK_KB;
	if (ra > A0S_MAX_SPAN) ra = A0S_MAX_SPAN;
	if (ra > n / 4)        ra = n / 4;

	g_nblks = n;
	g_ra    = ra;

	A0S_LOG("cache: %u blocks of %u KiB, readahead %u blocks\n",
		(unsigned)n, (unsigned)BLOCK_KB, (unsigned)ra);

	return 0;
}

static A0sBlock *find(SceUInt64 id, SceUInt32 index)
{
	for (SceUInt32 i = 0; i < g_nblks; ++i) {
		A0sBlock *b = &g_blks[i];

		if (b->state != BLK_EMPTY && b->id == id && b->index == index)
			return b;
	}

	return NULL;
}

/* an empty block, else the least recently used one that is not loading */
static A0sBlock *victim(void)
{
	A0sBlock *best = NULL;

	for (SceUInt32 i = 0; i < g_nblks; ++i) {
		A0sBlock *b = &g_blks[i];

		if (b->state == BLK_EMPTY)
			return b;

		if (b->state == BLK_VALID && (!best || (SceInt32)(b->lru - best->lru) < 0))
			best = b;
	}

	return best;
}

/* claims up to want absent blocks from index on, stopping at the first one
 * present, and marks them loading; returns how many */
static SceUInt32 claim(SceUInt64 id, SceUInt32 index, SceUInt32 want, A0sBlock **out)
{
	SceUInt32 k = 0;

	for (; k < want; ++k) {
		if (k && find(id, index + k))
			break;

		A0sBlock *b = victim();
		if (!b)
			break;

		b->state = BLK_LOADING;
		b->id    = id;
		b->index = index + k;
		b->len   = 0;
		b->lru   = ++g_clock;

		out[k] = b;
	}

	return k;
}

void a0sCacheComplete(A0sBlock **blks, SceUInt32 n, SceUInt32 got, int err)
{
	a0sLockMutex(g_lock);

	for (SceUInt32 k = 0; k < n; ++k) {
		A0sBlock *b = blks[k];

		SceUInt32 start = k * BLOCK_SIZE,
		          len   = 0;

		if (err >= 0 && got > start)
			len = got - start < BLOCK_SIZE ? got - start : BLOCK_SIZE;

		b->len   = len;
		b->state = len ? BLK_VALID : BLK_EMPTY;
		b->lru   = ++g_clock;
	}

	a0sUnlockMutex(g_lock);
}

static void track(A0sReadState *rs, SceUInt32 index)
{
	if (!rs)
		return;

	/* re-reading the block just read is not a jump */
	if (index == rs->nextBlock)
		++rs->streak;
	else if (index + 1 != rs->nextBlock)
		rs->streak = 0;

	rs->nextBlock = index + 1;
}

/* fetches blocks [index, index + n) of a file of the given size */
static int fetch(
	const char *path,
	SceUInt64 size,
	SceUInt32 index,
	A0sBlock **blks,
	SceUInt32 n,
	int async)
{
	SceUInt64 start = (SceUInt64)index * BLOCK_SIZE,
	          want  = (SceUInt64)n * BLOCK_SIZE;

	if (want > size - start)
		want = size - start;

	return a0sFetchBlocks(path, start, (SceUInt32)want, blks, n, async);
}

int a0sCacheRead(
	const char *path,
	SceUInt64 id,
	SceUInt64 size,
	A0sReadState *rs,
	void *buf,
	SceUInt32 len,
	SceUInt64 offset)
{
	if (offset >= size || len == 0)
		return 0;

	if (len > size - offset)
		len = (SceUInt32)(size - offset);

	SceUInt8 *dst = buf;

	SceUInt32 fileBlocks = (SceUInt32)((size + BLOCK_SIZE - 1) / BLOCK_SIZE),
	          lastBlock  = (SceUInt32)((offset + len - 1) / BLOCK_SIZE),
	          tracked    = NONE,
	          done       = 0;

	while (done < len) {
		SceUInt64 pos = offset + done;

		SceUInt32 index = (SceUInt32)(pos / BLOCK_SIZE),
		          boff  = (SceUInt32)(pos % BLOCK_SIZE);

		a0sLockMutex(g_lock);

		if (index != tracked) {
			track(rs, index);
			tracked = index;
		}

		A0sBlock *b = find(id, index);

		/* a hit */

		if (b && b->state == BLK_VALID) {
			if (boff >= b->len) {
				a0sUnlockMutex(g_lock);
				break;  /* the file shrank */
			}

			SceUInt32 n = b->len - boff;
			if (n > len - done)
				n = len - done;

			__builtin_memcpy(dst + done, b->data + boff, n);
			b->lru = ++g_clock;
			++g_a0sStats.hits;

			/* a sequential reader: keep the next window coming */
			A0sBlock *pf[A0S_MAX_SPAN];
			SceUInt32 npf = 0, pfIndex = 0;

			if (rs && rs->streak >= 2 && g_ra) {
				for (SceUInt32 k = 1; k <= g_ra && index + k < fileBlocks; ++k) {
					if (find(id, index + k))
						continue;

					pfIndex = index + k;
					npf = fileBlocks - pfIndex;
					if (npf > g_ra)
						npf = g_ra;

					npf = claim(id, pfIndex, npf, pf);
					break;
				}
			}

			a0sUnlockMutex(g_lock);

			done += n;

			if (npf) {
				++g_a0sStats.prefetches;
				fetch(path, size, pfIndex, pf, npf, 1);
			}

			continue;
		}

		/* loading, for another reader or a prefetch */

		if (b) {
			a0sUnlockMutex(g_lock);
			a0sDelayThread(500);
			continue;
		}

		/* a miss: fetch what this read needs, more for a sequential reader */

		++g_a0sStats.misses;

		SceUInt32 span = lastBlock - index + 1;

		if (rs && rs->streak >= 1 && span < g_ra) span = g_ra;
		if (span > fileBlocks - index)            span = fileBlocks - index;
		if (span > A0S_MAX_SPAN)                  span = A0S_MAX_SPAN;
		if (span > g_nblks / 2)                   span = g_nblks / 2;
		if (span == 0)                            span = 1;

		A0sBlock *got[A0S_MAX_SPAN];
		span = claim(id, index, span, got);

		a0sUnlockMutex(g_lock);

		/* every block is loading */
		if (span == 0) {
			a0sDelayThread(500);
			continue;
		}

		int r = fetch(path, size, index, got, span, 0);

		if (r < 0)
			return done ? (int)done : r;

		/* nothing there: the file shrank */
		if (r == 0)
			break;
	}

	return (int)done;
}
