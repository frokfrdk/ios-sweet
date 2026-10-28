/*
 * sweetdrm backend: the SweetGPU user client (patch 0112/0112c selectors 10-16,
 * the same ABI tools/sweetgpu/sweetgpu-test.c exercises). Apple builds only.
 */
#if defined(__APPLE__)

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>

#include "sweetdrm.h"

/* IOKit.framework, declared here as sweetgpu-test does (no IOKit headers in the iOS SDK) */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallMethod(mach_port_t connection, uint32_t selector, const uint64_t *input, uint32_t inputCnt,
    const void *inputStruct, size_t inputStructCnt, uint64_t *output, uint32_t *outputCnt, void *outputStruct,
    size_t *outputStructCnt);

/* The ABI in iokit/Kernel/arm/SweetGPU.cpp */
#define SWEET_GPU_ABI           1u
enum { SEL_BO_NEW = 10, SEL_BO_FREE, SEL_BO_IMPORT, SEL_SUBMIT, SEL_WAIT, SEL_INFO, SEL_REPORT };
#define REPORT_MAX              192u
struct sweet_gpu_submit {
	uint32_t count, reserved;
	struct sweetdrm_ib ib[SWEETDRM_MAX_IBS];
};
struct sweet_gpu_info {
	uint32_t chip_id, gmem_size, gmem_base, cur_khz;
	uint32_t max_khz, last_retired, flags, abi;
	uint64_t fence_user_va, window_base, window_size;
};
_Static_assert(sizeof(struct sweet_gpu_submit) == 200, "submit ABI");
_Static_assert(sizeof(struct sweet_gpu_info) == 56, "info ABI");

#define kIOReturnNoMemory       ((kern_return_t)0xe00002bd)
#define kIOReturnNotPrivileged  ((kern_return_t)0xe00002c1)
#define kIOReturnBadArgument    ((kern_return_t)0xe00002c2)
#define kIOReturnNotFound       ((kern_return_t)0xe00002f0)
#define kIOReturnTimeout        ((kern_return_t)0xe00002d6)
#define kIOReturnNotResponding  ((kern_return_t)0xe00002ed)
#define kIOReturnNoResources    ((kern_return_t)0xe00002be)

static io_connect_t conn;

static int
err(kern_return_t kr)
{
	switch (kr) {
	case KERN_SUCCESS:              return 0;
	case kIOReturnTimeout:          return ETIMEDOUT;
	case kIOReturnNotResponding:    return EIO;
	case kIOReturnBadArgument:      return EINVAL;
	case kIOReturnNoMemory:
	case kIOReturnNoResources:      return ENOMEM;
	case kIOReturnNotPrivileged:    return EPERM;
	case kIOReturnNotFound:         return ENOENT;
	default:                        return EIO;
	}
}

static int
k_open(struct sweetdrm_info *out)
{
	io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("SweetGPU"));
	if (!svc) {
		return ENODEV;
	}
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr) {
		return err(kr);         /* root only */
	}
	struct sweet_gpu_info in;
	size_t n = sizeof(in);
	memset(&in, 0, sizeof(in));
	kr = IOConnectCallMethod(conn, SEL_INFO, NULL, 0, NULL, 0, NULL, NULL, &in, &n);
	if (kr || in.abi != SWEET_GPU_ABI || in.fence_user_va == 0) {
		return kr ? err(kr) : ENODEV;
	}
	*out = (struct sweetdrm_info){
		in.chip_id, in.gmem_size, in.gmem_base, in.max_khz, in.window_base, in.window_size,
		(volatile const uint32_t *)(uintptr_t)in.fence_user_va,
	};
	return 0;
}

static int
k_bo_new(uint64_t size, uint32_t *handle, uint64_t *gpu_va, void **cpu)
{
	uint64_t in[2] = { size, 0 }, o[3] = { 0, 0, 0 };
	uint32_t n = 3;
	kern_return_t kr = IOConnectCallMethod(conn, SEL_BO_NEW, in, 2, NULL, 0, o, &n, NULL, NULL);
	if (kr == KERN_SUCCESS) {
		*handle = (uint32_t)o[0];
		*gpu_va = o[1];
		*cpu = (void *)(uintptr_t)o[2];
	}
	return err(kr);
}

static int
k_bo_import(uint32_t surface_id, uint32_t *handle, uint64_t *gpu_va, uint64_t *size)
{
	uint64_t in = surface_id, o[3] = { 0, 0, 0 };
	uint32_t n = 3;
	kern_return_t kr = IOConnectCallMethod(conn, SEL_BO_IMPORT, &in, 1, NULL, 0, o, &n, NULL, NULL);
	if (kr == KERN_SUCCESS) {
		*handle = (uint32_t)o[0];
		*gpu_va = o[1];
		*size = o[2];
	}
	return err(kr);
}

static int
k_bo_free(uint32_t handle)
{
	uint64_t in = handle;
	return err(IOConnectCallMethod(conn, SEL_BO_FREE, &in, 1, NULL, 0, NULL, NULL, NULL, NULL));
}

static int
k_submit(const struct sweetdrm_ib *ib, uint32_t count, uint32_t *seqno)
{
	struct sweet_gpu_submit s;
	memset(&s, 0, sizeof(s));
	s.count = count;
	memcpy(s.ib, ib, count * sizeof(*ib));
	uint64_t o = 0;
	uint32_t on = 1;
	__asm__ volatile("dsb sy" ::: "memory");        /* CPU writes done; the kernel cleans the BOs to PoC */
	kern_return_t kr = IOConnectCallMethod(conn, SEL_SUBMIT, NULL, 0, &s, 8 + count * sizeof(*ib), &o, &on, NULL, NULL);
	*seqno = kr ? 0 : (uint32_t)o;
	return err(kr);
}

static int
k_wait(uint32_t seqno, uint32_t timeout_ms)
{
	uint64_t in[2] = { seqno, timeout_ms };
	return err(IOConnectCallMethod(conn, SEL_WAIT, in, 2, NULL, 0, NULL, NULL, NULL, NULL));
}

static void
k_report(const char *line)
{
	size_t n = strcspn(line, "\n");
	n = n < REPORT_MAX ? n : REPORT_MAX;
	if (conn && n) {
		IOConnectCallMethod(conn, SEL_REPORT, NULL, 0, line, n, NULL, NULL, NULL, NULL);
	}
}

const struct sweetdrm_backend sweetdrm_iokit_backend = {
	k_open, k_bo_new, k_bo_import, k_bo_free, k_submit, k_wait, k_report,
};

#endif
