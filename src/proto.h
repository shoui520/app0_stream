/*
 * app0stream wire protocol, version 1.
 *
 * One TCP connection carries one request at a time.  Every integer is
 * little-endian.  Paths are UTF-8, relative to the server's library root,
 * '/'-separated, without a leading slash ("" is the root).  The server
 * resolves every path element case-insensitively, like the exFAT memory
 * card the games were made for, and reports canonical names back.
 *
 *   request   24 bytes, then pathLen bytes of path
 *
 *     u32 magic      A0S_REQ_MAGIC
 *     u32 seq        echoed in the response
 *     u16 op         A0S_OP_*
 *     u16 pathLen
 *     u32 arg0
 *     u64 arg1
 *
 *   response  16 bytes, then len bytes of payload
 *
 *     u32 magic      A0S_RSP_MAGIC
 *     u32 seq
 *     s32 status     >= 0 success (meaning per op), < 0 an SCE error code
 *     u32 len
 *
 *   HELLO     arg0 = A0S_VERSION.  status = the server's version, no payload.
 *   STAT      path.  payload = one record.
 *   READ      path, arg0 = length (<= A0S_MAX_READ), arg1 = offset.
 *             payload = the bytes read (fewer at the end of the file).
 *   READDIR   path, arg0 = first entry index, arg1 = max entries (<= 64).
 *             status = records in the payload; fewer than asked means the
 *             listing is complete.  "." and ".." are not listed.
 *   LOG       path = a line of text for the server's log (trace = 1).
 *             status 0, no payload.
 *
 *   record    A0S_STAT_SIZE bytes, then nameLen bytes of name
 *
 *     u32 type       A0S_TYPE_FILE or A0S_TYPE_DIR
 *     u32 reserved
 *     u64 size
 *     u64 id         stable per canonical path (vnode file id, cache key)
 *     u16 year, month, day, hour, minute, second    mtime, UTC
 *     u32 microsecond
 *     u16 nameLen   the canonical last path element ("" for the root)
 */

#ifndef A0S_PROTO_H
#define A0S_PROTO_H

#define A0S_REQ_MAGIC     0x51533041u   /* "A0SQ" */
#define A0S_RSP_MAGIC     0x52533041u   /* "A0SR" */
#define A0S_VERSION       1

#define A0S_OP_HELLO      1
#define A0S_OP_STAT       2
#define A0S_OP_READ       3
#define A0S_OP_READDIR    4
#define A0S_OP_LOG        5

#define A0S_TYPE_FILE     1
#define A0S_TYPE_DIR      2

#define A0S_REQ_SIZE      24
#define A0S_RSP_SIZE      16
#define A0S_STAT_SIZE     42            /* fixed part of a record */

#define A0S_MAX_PATH      255           /* bytes, without the terminator */
#define A0S_MAX_NAME      255
#define A0S_MAX_READ      (1024 * 1024)
#define A0S_MAX_DIRENTS   64

#define A0S_DEFAULT_PORT  7350

/* the SCE error codes a server may answer with (SCE_ERROR_ERRNO_*) */
#define A0S_ENOENT        ((int)0x80010002)
#define A0S_EIO           ((int)0x80010005)
#define A0S_EACCES        ((int)0x8001000D)
#define A0S_ENOTDIR       ((int)0x80010014)
#define A0S_EISDIR        ((int)0x80010015)
#define A0S_EINVAL        ((int)0x80010016)

#endif
