/*
 * "app0stream_netfs": a read-only file system whose files live on the PC.
 *
 * Mounted as net0: (root path /net/a0s).  Nothing reaches it directly: the
 * scePfsMount hook (main.c) mounts a game's PFS image from
 * net0:/.m<n>/app/TITLEID instead of ux0:app/TITLEID, and PFS then reads its
 * lower files here with sceVopLookup, sceVopOpen and sceVopPread (pfsmgr
 * bufmgr/file_access.c).
 *
 * What PFS needs from a lower file system, from the psp2 sources:
 *
 *   vfs_decode_path_elem   called before every lower lookup and for every
 *                          files.db key.  A NULL slot makes the dispatcher
 *                          answer EUNSUP, which file_access_lookup takes for
 *                          an error.  Names decode the way exFAT decodes
 *                          them (upcase.c).
 *
 *   vfs_devctl 0x3001      SceIoDevInfo.  pfsCoreMount fails the mount when
 *                          it returns < 0 (pfsmgr handler/pfops_impl.c).
 *
 *   vnode sizes            sceVopPread returns 0 at or past vp->size without
 *                          calling the file system (iofilemgr vfs_vnops.c).
 *
 * The vnode contracts follow exfatfs (exfatfs/vfs_exfatfs.c): lookup returns
 * a new vnode still locked, with refCount 1; set_root fills the vnode that
 * iofilemgr allocated and locks it itself; read advances file->fpos, pread
 * does not; lseek stores and returns the new position; dread returns 1 per
 * entry and 0 at the end; inactive frees the node and clears nodeData.
 */

#include <psp2kern/kernel/iofilemgr.h>

#include "a0s.h"
#include "psp2_vfs.h"
#include "vfs.h"

#define ERR_NOENT        ((int)0x80010002)
#define ERR_NOMEM        ((int)0x8001000C)
#define ERR_BUSY         ((int)0x80010010)
#define ERR_EXIST        ((int)0x80010011)
#define ERR_NOTDIR       ((int)0x80010014)
#define ERR_ISDIR        ((int)0x80010015)
#define ERR_INVAL        ((int)0x80010016)
#define ERR_ROFS         ((int)0x8001001E)
#define ERR_UNSUP        ((int)0x80010030)
#define ERR_NAMETOOLONG  ((int)0x8001005B)

/* FAT attributes, which exFAT keeps in aclData[0] and st_attr.  Access
 * class 0 (acmgr_fatfs_attr.c): every thread may read */
#define ATTR_DIR     0x10
#define ATTR_FILE    0x20

/* st_mode for access class 0 (fatfsModes[0] in acmgr_fatfs_attr.c) */
#define MODE_CLASS0  0x186

#define DIR_PAGE     16

typedef struct A0sNode {
	SceUInt64   id;
	SceUInt64   size;
	SceUInt32   type;       /* A0S_TYPE_* */
	SceDateTime mtime;

	SceUInt16   pathLen;
	char        path[A0S_MAX_PATH + 1];     /* relative to the server's root */
} A0sNode;

typedef struct A0sOpenFile {
	A0sReadState rs;
} A0sOpenFile;

typedef struct A0sOpenDir {
	SceUInt32 index;    /* of the next entry the server has */
	SceUInt32 count,    /* entries in page[] */
	          pos;      /* the next one to return */
	int eof;
	A0sStat page[DIR_PAGE];
} A0sOpenDir;

static A0sNode g_root;

static void fillStat(const A0sNode *n, SceIoStat *st)
{
	__builtin_memset(st, 0, sizeof(*st));

	if (n->type == A0S_TYPE_DIR) {
		st->st_mode = SCE_S_IFDIR | MODE_CLASS0;
		st->st_attr = ATTR_DIR;
	} else {
		st->st_mode = SCE_S_IFREG | MODE_CLASS0;
		st->st_attr = ATTR_FILE;
		st->st_size = (SceOff)n->size;
	}

	st->st_ctime = n->mtime;
	st->st_atime = n->mtime;
	st->st_mtime = n->mtime;
}

static void setMtime(SceDateTime *t, const A0sStat *s)
{
	t->year        = s->year;
	t->month       = s->month;
	t->day         = s->day;
	t->hour        = s->hour;
	t->minute      = s->minute;
	t->second      = s->second;
	t->microsecond = s->microsecond;
}

/* ---- file system operations ---------------------------------------------- */

static int netfsVfsMount(SceVfsOpMountArgs *argp)
{
	SceVfsMount *mnt = argp->mnt;

	/* argp->devFilePath is the anchor device (tty0:), nothing to open */

	g_root.type    = A0S_TYPE_DIR;
	g_root.pathLen = 0;
	g_root.path[0] = '\0';

	mnt->pData             = &g_root;
	mnt->availableEntryNum = 0x40;

	return 0;
}

static int netfsVfsUmount(SceVfsOpUmountArgs *argp)
{
	(void)argp;

	/* mounted until shutdown */
	return ERR_BUSY;
}

static int netfsVfsSetRoot(SceVfsOpSetRootArgs *argp)
{
	SceVfsVnode *vp = argp->vp;

	vfsLockVnode(vp);

	vp->core.nodeData   = &g_root;
	vp->core.mnt        = argp->mnt;
	vp->core.dd         = NULL;
	vp->core.type       = SCE_VNODE_TYPE_ROOTDIR;
	vp->core.state      = SCE_VNODE_STATE_ACTIVE;
	vp->core.aclData[0] = ATTR_DIR;
	vp->core.aclData[1] = 0;
	vp->core.fid[0]     = 0;
	vp->core.fid[1]     = 0;
	vp->core.size       = 0;

	vfsUnlockVnode(vp);

	return 0;
}

/* nothing to do for these */

static int netfsVfsSync(SceVfsOpSyncArgs *argp)  { (void)argp; return 0; }
static int netfsVfsInit(SceVfsOpInitArgs *argp)  { (void)argp; return 0; }
static int netfsVfsFini(SceVfsOpFiniArgs *argp)  { (void)argp; return 0; }

static int netfsVfsDevctl(SceVfsOpDevctlArg *argp)
{
	if (argp->cmd != 0x3001)
		return ERR_UNSUP;

	/* like exfatfs: SceIoDevInfo (0x18), or the 0x28 form with a volume
	 * serial and label after it */
	SceIoDevInfo *info = argp->buf;

	if (!info || (argp->bufLen != 0x18 && argp->bufLen != 0x28))
		return ERR_INVAL;

	__builtin_memset(info, 0, argp->bufLen);

	info->max_size     = 0x10000000000ULL;  /* 1 TiB, nominal */
	info->free_size    = 0;                 /* read-only */
	info->cluster_size = 0x8000;

	return 0;
}

static int netfsVfsDecodePathElem(SceVfsOpDecodePathElemArgs *argp)
{
	*argp->decodeLen = 0;

	if (!argp->path || !argp->buf || argp->bufLen == 0)
		return ERR_INVAL;

	SceUInt32 elemLen;
	SceUInt32 out = a0sDecodePathElem(argp->path, argp->buf, argp->bufLen, &elemLen);

	if (out >= argp->bufLen)
		return ERR_NAMETOOLONG;

	*argp->path2     = argp->path;
	*argp->path3     = argp->path + elemLen;
	*argp->decodeLen = out;

	return 0;
}

/* ---- vnode operations ---------------------------------------------------- */

/* name is ".m<n>" (upper-cased or not, see upcase.c); 1 and n, or 0 */
static int parseAlias(const char *name, SceUInt32 len, SceUInt32 *n)
{
	if (len < 3 || len > 10 || name[0] != '.' || (name[1] | 0x20) != 'm')
		return 0;

	SceUInt32 v = 0;

	for (SceUInt32 i = 2; i < len; ++i) {
		if (name[i] < '0' || name[i] > '9')
			return 0;
		v = v * 10 + (SceUInt32)(name[i] - '0');
	}

	*n = v;
	return 1;
}

/* the node for name in parent, from the server; < 0 if there is none */
static int makeNode(const A0sNode *parent, const char *name, SceUInt32 len, A0sNode **out)
{
	if (len == 0)
		return ERR_NOENT;

	/*
	 * A PFS mount covers its lower directory (vfs_mount_pfs enters its root
	 * in the parent's name cache), and a second mount on the same one fails
	 * with EBUSY.  appmgr mounts a game twice, once to load the eboot and
	 * once as the process's app0:, and shares the first mount only when it
	 * finds it by its lower path, which main.c changes.  So every mount gets
	 * a root of its own: net0:/.m<n>/app/TITLEID.  Its files keep the
	 * server's ids, so the cache is shared.
	 */
	SceUInt32 alias;

	if (parent == &g_root && parseAlias(name, len, &alias)) {
		A0sNode *n = a0sAlloc(sizeof(*n));
		if (!n)
			return ERR_NOMEM;

		n->id    = 0xA05A000000000000ULL | alias;
		n->type  = A0S_TYPE_DIR;
		n->mtime = g_root.mtime;

		*out = n;
		return 0;
	}

	for (SceUInt32 i = 0; i < len; ++i)
		if (name[i] == '\0' || name[i] == '/')
			return ERR_NOENT;

	/* parent's path, a slash unless parent is the root, then name */
	SceUInt32 plen = parent->pathLen;
	if (plen)
		++plen;

	if (plen + len > A0S_MAX_PATH)
		return ERR_NAMETOOLONG;

	char path[A0S_MAX_PATH + 1];

	__builtin_memcpy(path, parent->path, parent->pathLen);
	if (plen)
		path[plen - 1] = '/';

	__builtin_memcpy(path + plen, name, len);
	path[plen + len] = '\0';

	A0sStat st;

	int r = a0sStatPath(path, &st);
	if (r < 0)
		return r == A0S_ENOENT || r == A0S_ENOTDIR ? ERR_NOENT : r;

	if (st.type != A0S_TYPE_DIR && st.type != A0S_TYPE_FILE)
		return ERR_NOENT;

	A0sNode *n = a0sAlloc(sizeof(*n));
	if (!n)
		return ERR_NOMEM;

	/* keep the server's spelling of the name, so later requests need no
	 * case-insensitive search */
	const char *spelling = st.nameLen == len ? st.name : path + plen;

	__builtin_memcpy(n->path, path, plen);
	__builtin_memcpy(n->path + plen, spelling, len);

	n->pathLen          = (SceUInt16)(plen + len);
	n->path[plen + len] = '\0';

	n->id   = st.id;
	n->size = st.type == A0S_TYPE_FILE ? st.size : 0;
	n->type = st.type;
	setMtime(&n->mtime, &st);

	*out = n;
	return 0;
}

static int netfsVopLookup(SceVopLookupArgs *argp)
{
	SceVfsVnode   *dvp    = argp->dvp;
	const A0sNode *parent = dvp->core.nodeData;

	if (!parent || parent->type != A0S_TYPE_DIR)
		return ERR_NOTDIR;

	A0sNode *n;

	int r = makeNode(parent, argp->path->name, argp->path->nameLength, &n);
	if (r < 0)
		return r;

	SceVfsVnode *vp = NULL;

	r = vfsGetNewVnode(dvp->core.mnt, dvp->core.mnt->mntVfsInf->defaultVops, 0, &vp);
	if (r < 0) {
		a0sFree(n);
		return r;
	}

	r = vfsLockVnode(vp);
	if (r < 0) {
		vfsFreeVnode(vp);
		a0sFree(n);
		return r;
	}

	int dir = n->type == A0S_TYPE_DIR;

	vp->core.nodeData   = n;
	vp->core.mnt        = dvp->core.mnt;
	vp->core.dd         = dvp;
	vp->core.state      = SCE_VNODE_STATE_ACTIVE;
	vp->core.type       = dir ? SCE_VNODE_TYPE_DIR : SCE_VNODE_TYPE_REG;
	vp->core.size       = (SceOff)n->size;
	vp->core.aclData[0] = dir ? ATTR_DIR : ATTR_FILE;
	vp->core.aclData[1] = 0;
	vp->core.fid[0]     = (SceUInt32)n->id;
	vp->core.fid[1]     = (SceUInt32)(n->id >> 32);
	vp->core.refCount   = 1;

	*argp->vpp = vp;

	return 0;
}

static int netfsVopOpen(SceVopOpenArgs *argp)
{
	const A0sNode *n = argp->vp->core.nodeData;

	if (!n)
		return ERR_NOENT;
	if (argp->flags & (SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_APPEND))
		return ERR_ROFS;
	if (n->type == A0S_TYPE_DIR)
		return ERR_ISDIR;

	A0sOpenFile *f = a0sAlloc(sizeof(*f));
	if (!f)
		return ERR_NOMEM;

	f->rs.nextBlock = 0xFFFFFFFFu;
	argp->file->fd = (SceUInt32)f;

	A0S_TRACE("open %s file %p thread %X\n", n->path, (void *)f, (unsigned)a0sGetThreadId());

	return 0;
}

static int netfsVopClose(SceVopCloseArgs *argp)
{
	A0S_TRACE("close file %p thread %X\n", (void *)argp->file->fd, (unsigned)a0sGetThreadId());

	a0sFree((A0sOpenFile *)argp->file->fd);
	argp->file->fd = 0;

	return 0;
}

static int readAt(SceVfsVnode *vp, SceVfsFile *file, void *buf, SceSize nbyte, SceOff off)
{
	const A0sNode *n = vp->core.nodeData;
	A0sOpenFile   *f = (A0sOpenFile *)file->fd;

	if (!n || n->type != A0S_TYPE_FILE)
		return ERR_ISDIR;
	if (off < 0)
		return ERR_INVAL;

	unsigned thread = (unsigned)a0sGetThreadId();

	A0S_TRACE("read %s @%llX +%X to %p file %p thread %X\n",
		n->path, (unsigned long long)off, (unsigned)nbyte, buf, (void *)f, thread);

	int r = a0sCacheRead(n->path, n->id, n->size, f ? &f->rs : NULL, buf, nbyte, (SceUInt64)off);

	A0S_TRACE("read %s @%llX thread %X = %d\n", n->path, (unsigned long long)off, thread, r);

	return r;
}

static SceSSize netfsVopRead(SceVopReadArgs *argp)
{
	int r = readAt(argp->vp, argp->file, argp->buf, argp->nbyte, argp->file->fpos);

	if (r > 0)
		argp->file->fpos += r;

	return r;
}

static SceSSize netfsVopPread(SceVopPreadArgs *argp)
{
	return readAt(argp->vp, argp->file, argp->buf, argp->nbyte, argp->offset);
}

static SceOff netfsVopLseek(SceVopLseekArgs *argp)
{
	SceOff pos = argp->offset;

	switch (argp->whence) {
	case SCE_SEEK_SET:                                break;
	case SCE_SEEK_CUR:  pos += argp->file->fpos;      break;
	case SCE_SEEK_END:  pos += argp->vp->core.size;   break;
	default:            return ERR_INVAL;
	}

	if (pos < 0)
		return ERR_INVAL;

	argp->file->fpos = pos;

	return pos;
}

static int netfsVopIoctl(SceVopIoctlArgs *argp)
{
	(void)argp;
	return ERR_UNSUP;
}

static int netfsVopDopen(SceVopDopenArgs *argp)
{
	const A0sNode *n = argp->vp->core.nodeData;

	if (!n)
		return ERR_NOENT;
	if (n->type != A0S_TYPE_DIR)
		return ERR_NOTDIR;

	A0sOpenDir *d = a0sAlloc(sizeof(*d));
	if (!d)
		return ERR_NOMEM;

	argp->file->fd = (SceUInt32)d;

	return 0;
}

static int netfsVopDclose(SceVopDcloseArgs *argp)
{
	a0sFree((A0sOpenDir *)argp->file->fd);
	argp->file->fd = 0;

	return 0;
}

static int netfsVopDread(SceVopDreadArgs *argp)
{
	SceVfsFile    *file = argp->file;
	const A0sNode *n    = file->vnode->core.nodeData;
	A0sOpenDir    *d    = (A0sOpenDir *)file->fd;

	if (!n || !d)
		return ERR_INVAL;

	/* the next page of entries */
	if (d->pos == d->count) {
		if (d->eof)
			return 0;

		int r = a0sReadDirPath(n->path, d->index, d->page, DIR_PAGE);
		if (r < 0)
			return r;

		d->count  = (SceUInt32)r;
		d->pos    = 0;
		d->index += (SceUInt32)r;
		d->eof    = r < DIR_PAGE;

		if (r == 0)
			return 0;
	}

	const A0sStat *e = &d->page[d->pos++];

	A0sNode tmp = {
		.type = e->type,
		.size = e->size,
	};
	setMtime(&tmp.mtime, e);

	SceIoDirent *dir = argp->dir;

	fillStat(&tmp, &dir->d_stat);

	SceUInt32 len = e->nameLen;
	if (len > sizeof(dir->d_name) - 1)
		len = sizeof(dir->d_name) - 1;

	__builtin_memset(dir->d_name, 0, sizeof(dir->d_name));
	__builtin_memcpy(dir->d_name, e->name, len);
	dir->d_private = NULL;

	return 1;
}

static int netfsVopGetstat(SceVopGetstatArgs *argp)
{
	const A0sNode *n = argp->vp->core.nodeData;

	if (!n)
		return ERR_NOENT;

	fillStat(n, argp->stat);

	return 0;
}

static int netfsVopFgetstat(SceVopFgetstatArgs *argp)
{
	const A0sNode *n = argp->vp->core.nodeData;

	if (!n)
		return ERR_NOENT;

	fillStat(n, argp->stat);

	return 0;
}

static int netfsVopInactive(SceVopInactiveArgs *argp)
{
	A0sNode *n = argp->vp->core.nodeData;

	if (n != &g_root)
		a0sFree(n);

	argp->vp->core.nodeData = NULL;

	return 0;
}

static int netfsVopSync(SceVopSyncArgs *argp)        { (void)argp; return 0; }
static int netfsVopCleanup(SceVopCleanupArgs *argp)  { (void)argp; return 0; }

/* the write side stays NULL: iofilemgr answers EUNSUP for it, and the mount
 * is read-only anyway */
static const SceVopTable g_vops = {
	.vop_open     = netfsVopOpen,
	.vop_close    = netfsVopClose,
	.vop_lookup   = netfsVopLookup,
	.vop_read     = netfsVopRead,
	.vop_lseek    = netfsVopLseek,
	.vop_ioctl    = netfsVopIoctl,
	.vop_dopen    = netfsVopDopen,
	.vop_dclose   = netfsVopDclose,
	.vop_dread    = netfsVopDread,
	.vop_getstat  = netfsVopGetstat,
	.vop_pread    = netfsVopPread,
	.vop_inactive = netfsVopInactive,
	.vop_sync     = netfsVopSync,
	.vop_fgetstat = netfsVopFgetstat,
	.vop_cleanup  = netfsVopCleanup,
};

static const SceVfsOpTable g_vfsOps = {
	.vfs_mount            = netfsVfsMount,
	.vfs_umount           = netfsVfsUmount,
	.vfs_set_root         = netfsVfsSetRoot,
	.vfs_sync             = netfsVfsSync,
	.vfs_init             = netfsVfsInit,
	.vfs_fini             = netfsVfsFini,
	.vfs_devctl           = netfsVfsDevctl,
	.vfs_decode_path_elem = netfsVfsDecodePathElem,
};

static SceVfsInfo g_vfsInfo = {
	.vfsOps      = &g_vfsOps,
	.vfsName     = A0S_VFS_NAME,
	.vfsNameLen  = sizeof(A0S_VFS_NAME),
	.type        = SCE_VFS_TYPE_FS,
	.defaultVops = &g_vops,
};

/*
 * vfs_mount_fsroot (iofilemgr vfs.c) wants a device vnode under the mount.
 * This file system has no device, so it borrows tty0:, the debug console
 * iofilemgr itself mounts at boot (DummyTtyStart, dummy_stdio.c).  It is a
 * character device, which vfs_check_vnode_read_access lets anyone use.
 */
static SceVfsMountData g_mountData = {
	.assignName         = A0S_ASSIGN,
	.fsName             = A0S_VFS_NAME,
	.blockdevName       = "tty0:",
	.blockdevNameNoPart = NULL,
	.mntId              = 0x7E00,
};

static SceVfsMountParam g_mountParam = {
	.rootPath     = "/net/a0s",
	.blockdevName = "tty0:",
	.fsType       = SCE_VFS_FS_TYPE_FS,

	/*
	 * The memory card's device class and I/O scheduler queue, as for a
	 * game installed on ux0: (mount.c: ux0 is mounted with 0x202).  PFS
	 * mounts inherit it (vfs_mount_pfs: mnt->opt |= lower->opt), and a
	 * sequential read on a PFS file is served only for storage classes
	 * 0x1xx and 0x2xx (sceIoSchedulerIsStorageIo): those become a pread
	 * through ScePfsFacadeForKernel_4238D2D2, the rest go to
	 * ScePfsFacadeForKernel_BD5B21F6, which always fails with 0x8014231C.
	 *
	 * PFS also takes the class for the weight that picks its crypt buffer
	 * pool (bufmgr/buf_alloc.c: 0x202 is ScePfsCryptBufRM), so a streamed
	 * game shares the memory card's queue and buffers exactly as the same
	 * game installed on ux0: would.
	 */
	.opt = 0x202,

	.mntFlags = SCE_VFS_MOUNT_TYPE_FSROOT
	          | SCE_VFS_MOUNT_FLAG_RDONLY
	          | SCE_VFS_MOUNT_FLAG_NOBUF,

	.vfsName = A0S_VFS_NAME,
	.data    = NULL,
	.misc    = &g_mountData,
	.vops    = NULL,
};

/* both answer EEXIST when the plugin is loaded a second time */

int a0sVfsRegister(void)
{
	int r = ksceVfsAddVfs(&g_vfsInfo);

	return r == ERR_EXIST ? 0 : r;
}

int a0sVfsMount(void)
{
	int r = vfsMount(&g_mountParam);

	return r == ERR_EXIST ? 0 : r;
}
