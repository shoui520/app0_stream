/*
 * Lets kernel code receive from SceNetPs sockets into kernel memory.
 *
 * sceNetRecvfromForDriver passes kernel = 1 down to sceNetPsRecvit, which
 * ignores it: the receive's uio is always UIO_USERSPACE (net_ps_socket.c in
 * the psp2 sources), while sceNetPsSendit uses kernel ? UIO_SYSSPACE :
 * UIO_USERSPACE.  Received data is then copied out with sceKernelCopyToUser,
 * whose unprivileged stores fault on kernel pages (VMACCESS_ERROR_FAULT,
 * 0xC0022005).  The henkaku wiki documents the bug and this fix for 3.60
 * (Bugs: "sceNetRecvfromForDriver 0xC0022005 error on kernel call").
 *
 * In 3.65 the code is the same, at segment 0 + 0x6986 of SceNetPs:
 *
 *   sceNetPsRecvit:  push {r4-r10, lr}; sub sp, #16   (kernel at [sp, #0x30])
 *
 *   6986  vmov.i32  d16, #0            ->  ldr.w  ip, [sp, #0x30]
 *   698a  ldr       r1, [r3]
 *   698c  str       r2, [r4, #8]
 *   698e  ldr.w     r3, [sl, #12]
 *   6992  strd      r5, r5, [r4, #28]  ->  strd   ip, r5, [r4, #28]
 *   6996  str       r5, [r4, #24]
 *   6998  str       r3, [r4, #12]
 *   699a  str       r1, [r4, #36]
 *   699c  vstr      d16, [r4, #16]     ->  strd   r5, r5, [r4, #16]
 *
 * uio_segflg (+0x1C) becomes the caller's kernel flag; r5 is 0 there, so
 * uio_offset (+0x10) is still zeroed.  User callers pass 0 and see no
 * change.  The original bytes are checked before anything is written.
 */

#include <psp2kern/kernel/modulemgr.h>
#include <taihen.h>

#include "a0s.h"
#include "netps_fix.h"

/* taiHEN's taihenModuleUtils, not declared by taihen.h */
int module_get_offset(SceUID pid, SceUID modid, int segidx, size_t offset, uintptr_t *addr);

#define PATCH_OFFSET  0x6986
#define PATCH_SIZE    30

static const SceUInt8 g_original[PATCH_SIZE] = {
	0xC0, 0xEF, 0x10, 0x00,     /* vmov.i32  d16, #0 */
	0x19, 0x68,                 /* ldr       r1, [r3] */
	0xA2, 0x60,                 /* str       r2, [r4, #8] */
	0xDA, 0xF8, 0x0C, 0x30,     /* ldr.w     r3, [sl, #12] */
	0xC4, 0xE9, 0x07, 0x55,     /* strd      r5, r5, [r4, #28] */
	0xA5, 0x61,                 /* str       r5, [r4, #24] */
	0xE3, 0x60,                 /* str       r3, [r4, #12] */
	0x61, 0x62,                 /* str       r1, [r4, #36] */
	0xC4, 0xED, 0x04, 0x0B,     /* vstr      d16, [r4, #16] */
	0xDA, 0xF8, 0x08, 0x10,     /* ldr.w     r1, [sl, #8] */
};

static const SceUInt8 g_patched[PATCH_SIZE] = {
	0xDD, 0xF8, 0x30, 0xC0,     /* ldr.w     ip, [sp, #0x30] */
	0x19, 0x68,
	0xA2, 0x60,
	0xDA, 0xF8, 0x0C, 0x30,
	0xC4, 0xE9, 0x07, 0xC5,     /* strd      ip, r5, [r4, #28] */
	0xA5, 0x61,
	0xE3, 0x60,
	0x61, 0x62,
	0xC4, 0xE9, 0x04, 0x55,     /* strd      r5, r5, [r4, #16] */
	0xDA, 0xF8, 0x08, 0x10,
};

static int same(const SceUInt8 *a, const SceUInt8 *b)
{
	for (int i = 0; i < PATCH_SIZE; ++i)
		if (a[i] != b[i])
			return 0;

	return 1;
}

int a0sNetPsFix(void)
{
	SceUID modid = ksceKernelSearchModuleByName("SceNetPs");
	if (modid < 0) {
		A0S_LOG("SceNetPs not found: %08X\n", (unsigned)modid);
		return modid;
	}

	uintptr_t addr;

	int r = module_get_offset(KERNEL_PID, modid, 0, PATCH_OFFSET, &addr);
	if (r < 0) {
		A0S_LOG("SceNetPs segment 0: %08X\n", (unsigned)r);
		return r;
	}

	const SceUInt8 *code = (const SceUInt8 *)addr;

	/* another plugin got there first */
	if (same(code, g_patched)) {
		A0S_LOG("SceNetPs receive fix: already applied\n");
		return 0;
	}

	if (!same(code, g_original)) {
		A0S_LOG("SceNetPs receive fix: unexpected code at +0x%X, not patching\n", PATCH_OFFSET);
		return -1;
	}

	r = taiInjectDataForKernel(KERNEL_PID, modid, 0, PATCH_OFFSET, g_patched, PATCH_SIZE);
	if (r < 0) {
		A0S_LOG("SceNetPs receive fix: inject failed %08X\n", (unsigned)r);
		return r;
	}

	A0S_LOG("SceNetPs receive fix applied\n");

	return 0;
}
