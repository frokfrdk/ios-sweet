/*
 * sweetgpu-test: stage 6U check of the SweetGPU user client (patch 0112,
 * docs/GPU_A618_PLAN.md). Run as root on a kernel booted with sweetgpu=8
 * sweetsvc=7 after "published SweetGPUReady" is in the log.
 *
 *   1. info (selector 15) and the read-only fence page
 *   2. 64x64 RGBA8 clear, copy and pattern copy in BOs (10, 13, 14, 11),
 *      as the kernel's 6.4 self-test
 *   3. an IOSurface import (12) and a GPU clear of it
 *   4. bad submits (IB past its BO, a handle that is not ours): refused,
 *      and no seqno retires
 *
 * Prints "sweetgpu-test: PASS, 0 bad pixels" or "... FAIL ..." and exits 0/1.
 * Built in CI: xcrun -sdk iphoneos clang ... -framework IOKit -framework
 * IOSurface -framework CoreFoundation, then codesign -s - (ad hoc).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>

/* The iPhoneOS SDK has no IOKit headers; these are the public IOKit.framework and IOSurface.framework calls */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
typedef struct __IOSurface *IOSurfaceRef;
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallMethod(mach_port_t connection, uint32_t selector, const uint64_t *input, uint32_t inputCnt,
    const void *inputStruct, size_t inputStructCnt, uint64_t *output, uint32_t *outputCnt, void *outputStruct,
    size_t *outputStructCnt);
extern const CFStringRef kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat;
IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
uint32_t IOSurfaceGetID(IOSurfaceRef buffer);
kern_return_t IOSurfaceLock(IOSurfaceRef buffer, uint32_t options, uint32_t *seed);
kern_return_t IOSurfaceUnlock(IOSurfaceRef buffer, uint32_t options, uint32_t *seed);
void *IOSurfaceGetBaseAddress(IOSurfaceRef buffer);
size_t IOSurfaceGetBytesPerRow(IOSurfaceRef buffer);

/* The ABI in iokit/Kernel/arm/SweetGPU.cpp */
#define SWEET_GPU_ABI           1u
#define SWEET_GPU_MAX_IBS       16u
enum { SEL_BO_NEW = 10, SEL_BO_FREE, SEL_BO_IMPORT, SEL_SUBMIT, SEL_WAIT, SEL_INFO };
struct sweet_gpu_ib {
	uint32_t handle, offset, size_dw;
};
struct sweet_gpu_submit {
	uint32_t count, reserved;
	struct sweet_gpu_ib ib[SWEET_GPU_MAX_IBS];
};
struct sweet_gpu_info {
	uint32_t chip_id, gmem_size, gmem_base, cur_khz;
	uint32_t max_khz, last_retired, flags, abi;
	uint64_t fence_user_va, window_base, window_size;
};
_Static_assert(sizeof(struct sweet_gpu_submit) == 200, "submit ABI");
_Static_assert(sizeof(struct sweet_gpu_info) == 56, "info ABI");

#define kIOReturnBadArgument    ((kern_return_t)0xe00002c2)

/* PM4 (mainline adreno_pm4.xml / adreno_gpu.h): type 7 header with odd parity */
#define PM4_CP_MEM_WRITE        0x3du
#define PM4_CP_MEM_TO_MEM       0x73u
#define M2M_WAIT_FOR_MEM_WRITES (1u << 30)

#define W                       64u
#define H                       64u
#define WORDS                   (W * H)
#define CLEAR                   0xff336699u
#define POISON                  0x5ee7deadu
#define WAIT_MS                 1000u

static io_connect_t conn;

static uint32_t
parity(uint32_t v)
{
	return (0x9669u >> (0xfu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^
	       (v >> 28)))) & 1u;
}

static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | (parity(cnt) << 15) | ((op & 0x7fu) << 16) | (parity(op) << 23);
}

struct bo {
	uint32_t h;
	uint64_t gva;
	volatile uint32_t *cpu;
	uint32_t size;
};

static kern_return_t
bo_new(uint32_t size, struct bo *b)
{
	uint64_t in[2] = { size, 0 }, out[3] = { 0, 0, 0 };
	uint32_t n = 3;
	kern_return_t kr = IOConnectCallMethod(conn, SEL_BO_NEW, in, 2, NULL, 0, out, &n, NULL, NULL);
	if (kr == KERN_SUCCESS) {
		*b = (struct bo){ (uint32_t)out[0], out[1], (volatile uint32_t *)(uintptr_t)out[2], size };
	}
	return kr;
}

static void
bo_free(struct bo *b)
{
	if (b->h) {
		uint64_t in = b->h;
		kern_return_t kr = IOConnectCallMethod(conn, SEL_BO_FREE, &in, 1, NULL, 0, NULL, NULL, NULL, NULL);
		if (kr) {
			printf("sweetgpu-test: bo free %u: 0x%x\n", b->h, kr);
		}
		b->h = 0;
	}
}

static kern_return_t
submit(const struct sweet_gpu_ib *ibs, uint32_t n, uint32_t *seq)
{
	struct sweet_gpu_submit s;
	memset(&s, 0, sizeof(s));
	s.count = n;
	memcpy(s.ib, ibs, n * sizeof(*ibs));
	uint64_t out = 0;
	uint32_t on = 1;
	__asm__ volatile("dsb sy" ::: "memory");        /* write-combined CPU writes out before the GPU reads */
	kern_return_t kr = IOConnectCallMethod(conn, SEL_SUBMIT, NULL, 0, &s, 8 + n * sizeof(*ibs), &out, &on, NULL, NULL);
	*seq = (uint32_t)out;
	return kr;
}

static kern_return_t
run(uint32_t h, uint32_t dw, const char *what)
{
	struct sweet_gpu_ib ib = { h, 0, dw };
	uint32_t seq = 0;
	kern_return_t kr = submit(&ib, 1, &seq);
	if (kr == KERN_SUCCESS) {
		uint64_t in[2] = { seq, WAIT_MS };
		kr = IOConnectCallMethod(conn, SEL_WAIT, in, 2, NULL, 0, NULL, NULL, NULL, NULL);
	}
	if (kr) {
		printf("sweetgpu-test: %s: submit/wait 0x%x\n", what, kr);
	}
	return kr;
}

static kern_return_t
info(struct sweet_gpu_info *in)
{
	size_t n = sizeof(*in);
	memset(in, 0, sizeof(*in));
	return IOConnectCallMethod(conn, SEL_INFO, NULL, 0, NULL, 0, NULL, NULL, in, &n);
}

static uint32_t
check(volatile const uint32_t *p, uint32_t stride_words, int pattern, const char *what)
{
	uint32_t bad = 0;
	for (uint32_t i = 0; i < WORDS; ++i) {
		uint32_t want = pattern ? (((i / W) << 16) | (i % W)) : CLEAR;
		uint32_t got = p[(i / W) * stride_words + i % W];
		if (got != want) {
			if (bad < 4) {
				printf("sweetgpu-test:   %s: pixel (%u,%u) 0x%08x, want 0x%08x\n", what, i % W, i / W, got, want);
			}
			++bad;
		}
	}
	printf("sweetgpu-test: %s: %u bad pixels\n", what, bad);
	return bad;
}

/* 2: as the kernel self-test; UINT32_MAX if it could not run */
static uint32_t
test_bos(void)
{
	struct bo a = { 0 }, b = { 0 }, c = { 0 }, d = { 0 };
	const uint32_t clear_dw = 3u + WORDS, copy_dw = 6u * WORDS;
	uint32_t bad = UINT32_MAX;
	kern_return_t kr = bo_new(4u * WORDS, &a);
	kr = kr ? kr : bo_new(4u * WORDS, &b);
	kr = kr ? kr : bo_new(4u * clear_dw, &c);
	kr = kr ? kr : bo_new(4u * copy_dw, &d);
	if (kr) {
		printf("sweetgpu-test: bo new: 0x%x\n", kr);
		goto out;
	}
	printf("sweetgpu-test: BOs at gpu 0x%llx 0x%llx 0x%llx 0x%llx\n", a.gva, b.gva, c.gva, d.gva);
	for (uint32_t i = 0; i < WORDS; ++i) {
		a.cpu[i] = POISON;
		b.cpu[i] = POISON;
	}
	uint32_t k = 0;
	c.cpu[k++] = pkt7(PM4_CP_MEM_WRITE, 2u + WORDS);
	c.cpu[k++] = (uint32_t)a.gva;
	c.cpu[k++] = (uint32_t)(a.gva >> 32);
	for (uint32_t i = 0; i < WORDS; ++i) {
		c.cpu[k++] = CLEAR;
	}
	k = 0;
	for (uint32_t i = 0; i < WORDS; ++i) {
		uint64_t dst = b.gva + 4u * i, src = a.gva + 4u * i;
		d.cpu[k++] = pkt7(PM4_CP_MEM_TO_MEM, 5);
		d.cpu[k++] = i == 0 ? M2M_WAIT_FOR_MEM_WRITES : 0;
		d.cpu[k++] = (uint32_t)dst;
		d.cpu[k++] = (uint32_t)(dst >> 32);
		d.cpu[k++] = (uint32_t)src;
		d.cpu[k++] = (uint32_t)(src >> 32);
	}
	if (run(c.h, clear_dw, "clear")) {
		goto out;
	}
	bad = check(a.cpu, W, 0, "BO clear");
	if (run(d.h, copy_dw, "copy")) {
		bad = UINT32_MAX;
		goto out;
	}
	bad += check(b.cpu, W, 0, "BO copy");
	for (uint32_t i = 0; i < WORDS; ++i) {
		a.cpu[i] = ((i / W) << 16) | (i % W);
	}
	if (run(d.h, copy_dw, "pattern copy")) {
		bad = UINT32_MAX;
		goto out;
	}
	bad += check(b.cpu, W, 1, "BO pattern copy");
out:
	bo_free(&a);
	bo_free(&b);
	bo_free(&c);
	bo_free(&d);
	return bad;
}

static void
dict_int(CFMutableDictionaryRef d, CFStringRef key, int32_t v)
{
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
	CFDictionarySetValue(d, key, n);
	CFRelease(n);
}

/* 3: import a 64x64 BGRA surface and clear it row by row with CP_MEM_WRITE */
static uint32_t
test_surface(void)
{
	CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	dict_int(props, kIOSurfaceWidth, W);
	dict_int(props, kIOSurfaceHeight, H);
	dict_int(props, kIOSurfaceBytesPerElement, 4);
	dict_int(props, kIOSurfacePixelFormat, 0x42475241);        /* 'BGRA' */
	IOSurfaceRef s = IOSurfaceCreate(props);
	CFRelease(props);
	if (s == NULL) {
		printf("sweetgpu-test: IOSurfaceCreate failed\n");
		return UINT32_MAX;
	}
	uint32_t bad = UINT32_MAX, rowdw = (uint32_t)(IOSurfaceGetBytesPerRow(s) / 4u);
	struct bo ib = { 0 }, imp = { 0 };
	uint64_t in = IOSurfaceGetID(s), out[3] = { 0, 0, 0 };
	uint32_t n = 3;
	kern_return_t kr = IOConnectCallMethod(conn, SEL_BO_IMPORT, &in, 1, NULL, 0, out, &n, NULL, NULL);
	if (kr) {
		printf("sweetgpu-test: import of surface %llu: 0x%x\n", in, kr);
		goto out;
	}
	imp = (struct bo){ (uint32_t)out[0], out[1], NULL, (uint32_t)out[2] };
	printf("sweetgpu-test: surface %llu (%u bytes per row) imported at gpu 0x%llx, %u bytes\n", in, rowdw * 4u,
	    imp.gva, imp.size);
	const uint32_t ib_dw = H * (3u + W);
	if ((kr = bo_new(4u * ib_dw, &ib))) {
		printf("sweetgpu-test: bo new: 0x%x\n", kr);
		goto out;
	}
	IOSurfaceLock(s, 0, NULL);
	volatile uint32_t *px = IOSurfaceGetBaseAddress(s);
	for (uint32_t y = 0; y < H; ++y) {
		for (uint32_t x = 0; x < W; ++x) {
			px[y * rowdw + x] = POISON;
		}
	}
	IOSurfaceUnlock(s, 0, NULL);
	uint32_t k = 0;
	for (uint32_t y = 0; y < H; ++y) {
		uint64_t row = imp.gva + 4ull * y * rowdw;
		ib.cpu[k++] = pkt7(PM4_CP_MEM_WRITE, 2u + W);
		ib.cpu[k++] = (uint32_t)row;
		ib.cpu[k++] = (uint32_t)(row >> 32);
		for (uint32_t x = 0; x < W; ++x) {
			ib.cpu[k++] = CLEAR;
		}
	}
	if (run(ib.h, ib_dw, "surface clear") == KERN_SUCCESS) {
		IOSurfaceLock(s, 1, NULL);  /* read only */
		bad = check(px, rowdw, 0, "IOSurface clear");
		IOSurfaceUnlock(s, 1, NULL);
	}
out:
	bo_free(&ib);
	bo_free(&imp);
	CFRelease(s);
	return bad;
}

/* 4: both refused with kIOReturnBadArgument, and the last retired seqno does not move */
static int
test_bad(void)
{
	/* one 16 KiB XNU page: the kernel rounds BO sizes up to whole pages */
	const uint32_t size = 16384;
	struct bo b = { 0 };
	kern_return_t kr = bo_new(size, &b);
	if (kr) {
		printf("sweetgpu-test: bo new: 0x%x\n", kr);
		return 0;
	}
	struct sweet_gpu_info i0, i1;
	info(&i0);
	struct sweet_gpu_ib past = { b.h, size - 4u, 2 }, other = { b.h + 1000u, 0, 1 };
	uint32_t seq = 0;
	kern_return_t k1 = submit(&past, 1, &seq), k2 = submit(&other, 1, &seq);
	info(&i1);
	bo_free(&b);
	int ok = k1 == kIOReturnBadArgument && k2 == kIOReturnBadArgument && i0.last_retired == i1.last_retired;
	printf("sweetgpu-test: bad submits: IB past its BO 0x%x, foreign handle 0x%x (want 0x%x both); last retired %u -> %u: %s\n",
	    k1, k2, kIOReturnBadArgument, i0.last_retired, i1.last_retired, ok ? "refused" : "NOT refused");
	return ok;
}

int
main(void)
{
	io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("SweetGPU"));
	if (!svc) {
		printf("sweetgpu-test: no SweetGPU service (needs sweetgpu=8 sweetsvc=7 and 6.7 passed)\n");
		return 1;
	}
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr) {
		printf("sweetgpu-test: IOServiceOpen: 0x%x (root only)\n", kr);
		return 1;
	}
	struct sweet_gpu_info in;
	kr = info(&in);
	volatile const uint32_t *fence = (volatile const uint32_t *)(uintptr_t)in.fence_user_va;
	printf("sweetgpu-test: info 0x%x: abi %u chip_id 0x%08x gmem %#x at %#x, %u/%u kHz, flags %#x, last retired %u, "
	    "fence page 0x%llx (word 0 %u), window 0x%llx + 0x%llx\n", kr, in.abi, in.chip_id, in.gmem_size, in.gmem_base,
	    in.cur_khz, in.max_khz, in.flags, in.last_retired, in.fence_user_va, fence ? fence[0] : 0, in.window_base,
	    in.window_size);
	int ok = kr == KERN_SUCCESS && in.abi == SWEET_GPU_ABI && fence != NULL;
	uint32_t bad_bo = ok ? test_bos() : UINT32_MAX;
	uint32_t bad_surf = ok ? test_surface() : UINT32_MAX;
	int refused = ok && test_bad();
	uint32_t bad = bad_bo == UINT32_MAX || bad_surf == UINT32_MAX ? UINT32_MAX : bad_bo + bad_surf;
	ok = ok && bad == 0 && refused;
	if (bad == UINT32_MAX) {
		printf("sweetgpu-test: FAIL, a pixel test did not run\n");
	} else {
		printf("sweetgpu-test: %s, %u bad pixels\n", ok ? "PASS" : "FAIL", bad);
	}
	IOServiceClose(conn);
	return ok ? 0 : 1;
}
