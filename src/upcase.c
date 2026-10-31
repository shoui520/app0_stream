/*
 * Path element decoding, as the memory card's exFAT driver does it.
 *
 * PFS keeps the names of an encrypted game's files in sce_pfs/files.db, a
 * B-tree it searches by comparing names decoded through the lower file
 * system's vfs_decode_path_elem (pfsmgr btree/btree.c, btree/fsdb.c in the
 * psp2 sources).  exFAT decodes an element by upper-casing it with the
 * volume's up-case table (sceExfatfsVopDecodePathElem, UVFAT_ToUpperBuf).
 * For lookups in files.db to find what they find on the memory card, this
 * file system has to decode names the same way.
 *
 * Below is the part of the default exFAT up-case table that game file names
 * can plausibly use: ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic and
 * fullwidth Latin.  Other code points are kept as they are.
 */

#include "a0s.h"

#define NOT_UTF8 0xFFFFFFFFu

static int in(SceUInt32 c, SceUInt32 lo, SceUInt32 hi)
{
	return c >= lo && c <= hi;
}

static SceUInt32 upcase(SceUInt32 c)
{
	/* ASCII */
	if (c < 0x80)
		return in(c, 'a', 'z') ? c - 0x20 : c;

	/* Latin-1 */
	if (c < 0x100) {
		if (c == 0xB5) return 0x39C;
		if (c == 0xFF) return 0x178;

		return in(c, 0xE0, 0xFE) && c != 0xF7 ? c - 0x20 : c;
	}

	/* Latin Extended-A: pairs, upper case first (or second) */
	if (c < 0x180) {
		int oddPair  = in(c, 0x100, 0x12F) || in(c, 0x132, 0x137) || in(c, 0x14A, 0x177),
		    evenPair = in(c, 0x139, 0x148) || in(c, 0x179, 0x17E);

		if (oddPair && (c & 1))
			return c - 1;
		if (evenPair && !(c & 1))
			return c - 1;

		return c;
	}

	/* Greek */
	if (in(c, 0x370, 0x3FF)) {
		if (c == 0x3AC)            return 0x386;
		if (in(c, 0x3AD, 0x3AF))   return c - 0x25;
		if (c == 0x3C2)            return 0x3A3;
		if (in(c, 0x3B1, 0x3C1))   return c - 0x20;
		if (in(c, 0x3C3, 0x3CB))   return c - 0x20;
		if (c == 0x3CC)            return 0x38C;
		if (in(c, 0x3CD, 0x3CE))   return c - 0x3F;

		return c;
	}

	/* Cyrillic */
	if (in(c, 0x400, 0x4FF)) {
		if (in(c, 0x430, 0x44F))   return c - 0x20;
		if (in(c, 0x450, 0x45F))   return c - 0x50;

		int pair = in(c, 0x460, 0x481) || in(c, 0x48A, 0x4BF);

		return pair && (c & 1) ? c - 1 : c;
	}

	/* fullwidth Latin */
	return in(c, 0xFF41, 0xFF5A) ? c - 0x20 : c;
}

static int cont(SceUInt8 b)
{
	return (b & 0xC0) == 0x80;
}

/* the UTF-8 sequence at p (n bytes left); *len gets its length */
static SceUInt32 utf8Get(const SceUInt8 *p, SceUInt32 n, SceUInt32 *len)
{
	SceUInt32 c = p[0];

	if (in(c, 0xC2, 0xDF) && n >= 2 && cont(p[1])) {
		*len = 2;
		return (c & 0x1F) << 6 | (p[1] & 0x3F);
	}

	if (in(c, 0xE0, 0xEF) && n >= 3 && cont(p[1]) && cont(p[2])) {
		SceUInt32 v = (c & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F);

		if (v >= 0x800) {
			*len = 3;
			return v;
		}
	}

	/* ASCII, or a byte that is kept as it is */
	*len = 1;
	return c < 0x80 ? c : NOT_UTF8;
}

static SceUInt32 utf8Put(SceUInt32 c, SceUInt8 *out)
{
	if (c < 0x80) {
		out[0] = (SceUInt8)c;
		return 1;
	}

	if (c < 0x800) {
		out[0] = (SceUInt8)(0xC0 | c >> 6);
		out[1] = (SceUInt8)(0x80 | (c & 0x3F));
		return 2;
	}

	out[0] = (SceUInt8)(0xE0 | c >> 12);
	out[1] = (SceUInt8)(0x80 | (c >> 6 & 0x3F));
	out[2] = (SceUInt8)(0x80 | (c & 0x3F));
	return 3;
}

SceUInt32 a0sDecodePathElem(
	const char *path,
	char *buf,
	SceUInt32 cap,
	SceUInt32 *elemLen)
{
	const SceUInt8 *p = (const SceUInt8 *)path;

	SceUInt32 n = 0;
	while (p[n] != '\0' && p[n] != '/')
		++n;

	*elemLen = n;

	SceUInt32 out = 0;

	for (SceUInt32 i = 0; i < n; ) {
		SceUInt8  tmp[3];
		SceUInt32 len, w;

		SceUInt32 c = utf8Get(p + i, n - i, &len);

		if (c == NOT_UTF8) {
			tmp[0] = p[i];
			w = 1;
		} else {
			w = utf8Put(upcase(c), tmp);
		}

		for (SceUInt32 k = 0; k < w; ++k, ++out)
			if (out + 1 < cap)
				buf[out] = (char)tmp[k];

		i += len;
	}

	if (cap)
		buf[out < cap ? out : cap - 1] = '\0';

	return out;
}
