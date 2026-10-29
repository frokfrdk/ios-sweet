/*
 * Host test for sweetdrm.c: the msm calls Mesa makes, against a fake SweetGPU
 * that retires submits only when told to. Build (Linux or macOS):
 *   cc -std=gnu11 -Wall -Wextra -I<mesa>/include -Iinclude \
 *      sweetdrm.c test/test_sweetdrm.c -lpthread -o test_sweetdrm
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include "xf86drm.h"
#include "sweetdrm.h"
#include "drm-uapi/msm_drm.h"

static int failures;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* The fake GPU */
#define FAKE_BOS 64
static struct { uint64_t size; void *cpu; int live; } fbo[FAKE_BOS + 1];
static uint32_t fence_page[1024];
static uint32_t submitted;      /* last seqno handed out */
static uint32_t n_submits, n_waits, last_wait_ms;
static struct sweetdrm_ib last_ib[SWEETDRM_MAX_IBS];
static uint32_t last_count, total_ibs;
static int dead;

static void retire_all(void) { fence_page[0] = submitted; }

static int
f_open(struct sweetdrm_info *i)
{
	*i = (struct sweetdrm_info){ 0x06010800, 0x80000, 0x100000, 710000, 0x100000000ull, 0x100000000ull, fence_page };
	return 0;
}

static int
f_bo_new(uint64_t size, uint32_t *h, uint64_t *va, void **cpu)
{
	if (size > (16u << 20)) {
		return EINVAL;
	}
	for (uint32_t i = 1; i <= FAKE_BOS; i++) {
		if (!fbo[i].live) {
			fbo[i].size = (size + 0x3fff) & ~0x3fffull;
			fbo[i].cpu = calloc(1, fbo[i].size);
			fbo[i].live = 1;
			*h = i;
			*va = 0x100000000ull + (uint64_t)i * 0x1000000;
			*cpu = fbo[i].cpu;
			return 0;
		}
	}
	return ENOMEM;
}

static int
f_bo_import(uint32_t id, uint32_t *h, uint64_t *va, uint64_t *size)
{
	if (id != 77) {
		return ENOENT;
	}
	for (uint32_t i = 1; i <= FAKE_BOS; i++) {
		if (!fbo[i].live) {
			fbo[i] = (typeof(fbo[i])){ 0x10000, NULL, 1 };
			*h = i;
			*va = 0x100000000ull + (uint64_t)i * 0x1000000;
			*size = 0x10000;
			return 0;
		}
	}
	return ENOMEM;
}

static int
f_bo_free(uint32_t h)
{
	if (h == 0 || h > FAKE_BOS || !fbo[h].live) {
		return EINVAL;
	}
	free(fbo[h].cpu);
	fbo[h] = (typeof(fbo[h])){ 0, NULL, 0 };
	return 0;
}

static int
f_submit(const struct sweetdrm_ib *ib, uint32_t count, uint32_t *seq)
{
	if (count == 0 || count > SWEETDRM_MAX_IBS) {
		return EINVAL;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint32_t h = ib[i].handle;
		if (h == 0 || h > FAKE_BOS || !fbo[h].live || (uint64_t)ib[i].offset + 4ull * ib[i].size_dw > fbo[h].size) {
			return EINVAL;
		}
	}
	memcpy(last_ib, ib, count * sizeof(*ib));
	last_count = count;
	total_ibs += count;
	n_submits++;
	*seq = ++submitted;
	return 0;
}

static int
f_wait(uint32_t seq, uint32_t ms)
{
	n_waits++;
	last_wait_ms = ms;
	CHECK(ms <= 10000);
	if (dead) {
		return EIO;
	}
	return (int32_t)(fence_page[0] - seq) >= 0 ? 0 : ETIMEDOUT;
}

static char last_report[200];
static void f_report(const char *l) { snprintf(last_report, sizeof(last_report), "%s", l); }

static const struct sweetdrm_backend fake = { f_open, f_bo_new, f_bo_import, f_bo_free, f_submit, f_wait, f_report };

static struct drm_msm_timespec
in_ms(int64_t ms)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	int64_t ns = (int64_t)t.tv_nsec + ms * 1000000;
	return (struct drm_msm_timespec){ t.tv_sec + ns / 1000000000, ns % 1000000000 };
}

static uint64_t
param(int fd, uint32_t p, int *ret)
{
	struct drm_msm_param r = { .pipe = MSM_PIPE_3D0, .param = p };
	*ret = drmCommandWriteRead(fd, DRM_MSM_GET_PARAM, &r, sizeof(r));
	return r.value;
}

static uint32_t
gem_new(int fd, uint64_t size)
{
	struct drm_msm_gem_new r = { .size = size, .flags = MSM_BO_WC };
	CHECK(drmCommandWriteRead(fd, DRM_MSM_GEM_NEW, &r, sizeof(r)) == 0);
	CHECK(r.handle != 0);
	return r.handle;
}

static int
gem_info(int fd, uint32_t h, uint32_t what, uint64_t *v)
{
	struct drm_msm_gem_info r = { .handle = h, .info = what };
	int ret = drmCommandWriteRead(fd, DRM_MSM_GEM_INFO, &r, sizeof(r));
	*v = r.value;
	return ret;
}

static int
submit(int fd, struct drm_msm_gem_submit_bo *bos, uint32_t nbos, struct drm_msm_gem_submit_cmd *cmds, uint32_t ncmds,
    uint32_t *fence)
{
	struct drm_msm_gem_submit r = {
		.flags = MSM_PIPE_3D0 | MSM_SUBMIT_NO_IMPLICIT, .nr_bos = nbos, .nr_cmds = ncmds,
		.bos = (uintptr_t)bos, .cmds = (uintptr_t)cmds, .queueid = 1,
	};
	int ret = drmCommandWriteRead(fd, DRM_MSM_GEM_SUBMIT, &r, sizeof(r));
	*fence = r.fence;
	return ret;
}

static int
cpu_prep(int fd, uint32_t h, uint32_t op, int64_t ms)
{
	struct drm_msm_gem_cpu_prep r = { .handle = h, .op = op, .timeout = in_ms(ms) };
	return drmCommandWrite(fd, DRM_MSM_GEM_CPU_PREP, &r, sizeof(r));
}

int
main(void)
{
	int ret;
	sweetdrm_set_backend(&fake);

	CHECK(drmOpenWithType("amdgpu", NULL, DRM_NODE_RENDER) < 0);
	int fd = drmOpenWithType("msm", NULL, DRM_NODE_RENDER);
	CHECK(fd >= 0);
	int fd2 = dup(fd);      /* fd_device_new_dup */
	CHECK(drmGetVersion(0x7fff) == NULL);

	drmVersionPtr v = drmGetVersion(fd2);
	CHECK(v && strcmp(v->name, "msm") == 0 && v->version_major == 1 && v->version_minor == 7);
	drmFreeVersion(v);

	uint64_t cap = 1;
	CHECK(drmGetCap(fd, DRM_CAP_SYNCOBJ, &cap) == 0 && cap == 0);

	/* params, as fd_pipe_new and the screen ask */
	CHECK(param(fd, MSM_PARAM_GPU_ID, &ret) == 618 && ret == 0);
	CHECK(param(fd, MSM_PARAM_CHIP_ID, &ret) == 0x06010800 && ret == 0);
	CHECK(param(fd, MSM_PARAM_GMEM_SIZE, &ret) == 0x80000 && ret == 0);
	CHECK(param(fd, MSM_PARAM_GMEM_BASE, &ret) == 0x100000 && ret == 0);
	CHECK(param(fd, MSM_PARAM_MAX_FREQ, &ret) == 710000000ull && ret == 0);
	CHECK(param(fd, MSM_PARAM_PRIORITIES, &ret) == 1 && ret == 0);
	CHECK(param(fd, MSM_PARAM_VA_START, &ret) == 0x100000000ull && ret == 0);
	param(fd, MSM_PARAM_TIMESTAMP, &ret);
	CHECK(ret == -EINVAL && errno == EINVAL);
	param(fd, MSM_PARAM_UCHE_TRAP_BASE, &ret);
	CHECK(ret == -EINVAL);
	param(fd, 0x999, &ret);
	CHECK(ret == -EINVAL);

	struct drm_msm_param sp = { .pipe = MSM_PIPE_3D0, .param = MSM_PARAM_SYSPROF, .value = 1 };
	CHECK(drmCommandWrite(fd, DRM_MSM_SET_PARAM, &sp, sizeof(sp)) == -EINVAL);

	/* submit queues */
	struct drm_msm_submitqueue q = { .flags = 0, .prio = 0 };
	CHECK(drmCommandWriteRead(fd, DRM_MSM_SUBMITQUEUE_NEW, &q, sizeof(q)) == 0 && q.id == 1);
	uint32_t faults = 0xffffffff;
	struct drm_msm_submitqueue_query qq = { .data = (uintptr_t)&faults, .id = q.id, .param = MSM_SUBMITQUEUE_PARAM_FAULTS, .len = 4 };
	CHECK(drmCommandWriteRead(fd, DRM_MSM_SUBMITQUEUE_QUERY, &qq, sizeof(qq)) == 0 && faults == 0);

	/* a BO: iova, map cookie, mmap through the dup'ed fd, munmap keeps it */
	uint32_t a = gem_new(fd, 0x5000);
	uint64_t iova = 0, off = 0, flags = 0;
	CHECK(gem_info(fd, a, MSM_INFO_GET_IOVA, &iova) == 0 && iova == 0x100000000ull + a * 0x1000000ull);
	CHECK(gem_info(fd, a, MSM_INFO_GET_OFFSET, &off) == 0 && off != 0);
	CHECK(gem_info(fd, a, MSM_INFO_GET_FLAGS, &flags) == 0 && flags == MSM_BO_WC);
	CHECK(gem_info(fd, a, MSM_INFO_SET_NAME, &flags) == 0);
	CHECK(gem_info(fd, a, MSM_INFO_SET_IOVA, &flags) == -EINVAL);
	CHECK(gem_info(fd, 999, MSM_INFO_GET_IOVA, &flags) == -ENOENT);
	uint32_t *pa = sweetdrm_mmap(NULL, 0x5000, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, (off_t)off);
	CHECK(pa != MAP_FAILED && pa == fbo[a].cpu);
	CHECK(sweetdrm_mmap(NULL, 0x5000, PROT_READ, MAP_SHARED, fd, (off_t)(off + (1ull << 16) * 50)) == MAP_FAILED);
	pa[0] = 0x12345678;
	CHECK(sweetdrm_munmap(pa, 0x5000) == 0 && ((uint32_t *)fbo[a].cpu)[0] == 0x12345678);
	/* plain mmap still works for anything else */
	void *anon = sweetdrm_mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(anon != MAP_FAILED && sweetdrm_munmap(anon, 4096) == 0);

	struct drm_msm_gem_madvise mv = { .handle = a, .madv = MSM_MADV_WILLNEED };
	CHECK(drmCommandWriteRead(fd, DRM_MSM_GEM_MADVISE, &mv, sizeof(mv)) == 0 && mv.retained == 1);

	/* never submitted: cpu_prep returns at once, no wait */
	uint32_t w0 = n_waits;
	CHECK(cpu_prep(fd, a, MSM_PREP_READ | MSM_PREP_NOSYNC, 0) == 0 && n_waits == w0);

	/* one cmd */
	uint32_t b = gem_new(fd, 0x4000);
	struct drm_msm_gem_submit_bo sb[2] = { { .flags = MSM_SUBMIT_BO_READ, .handle = a }, { .flags = MSM_SUBMIT_BO_WRITE, .handle = b } };
	struct drm_msm_gem_submit_cmd cmd[20];
	memset(cmd, 0, sizeof(cmd));
	cmd[0] = (struct drm_msm_gem_submit_cmd){ .type = MSM_SUBMIT_CMD_BUF, .submit_idx = 0, .submit_offset = 0x100, .size = 64 };
	uint32_t fence = 0;
	CHECK(submit(fd, sb, 2, cmd, 1, &fence) == 0 && fence == 1);
	CHECK(last_count == 1 && last_ib[0].handle == a && last_ib[0].offset == 0x100 && last_ib[0].size_dw == 16);

	/* busy: NOSYNC says EBUSY; a short prep times out; WAIT_FENCE too */
	CHECK(cpu_prep(fd, b, MSM_PREP_READ | MSM_PREP_NOSYNC, 0) == -EBUSY);
	CHECK(cpu_prep(fd, b, MSM_PREP_READ, 5) == -ETIMEDOUT);
	struct drm_msm_wait_fence wf = { .fence = fence, .timeout = in_ms(5), .queueid = q.id };
	CHECK(drmCommandWrite(fd, DRM_MSM_WAIT_FENCE, &wf, sizeof(wf)) == -ETIMEDOUT);
	/* a past deadline still asks the backend once (0 ms) */
	wf.timeout = in_ms(-1000);
	w0 = n_waits;
	CHECK(drmCommandWrite(fd, DRM_MSM_WAIT_FENCE, &wf, sizeof(wf)) == -ETIMEDOUT && n_waits == w0 + 1 && last_wait_ms == 0);
	/* a long deadline is cut into 10 s backend waits */
	retire_all();
	wf.timeout = in_ms(60000);
	CHECK(drmCommandWrite(fd, DRM_MSM_WAIT_FENCE, &wf, sizeof(wf)) == 0 && last_wait_ms == 10000);
	/* retired: still a backend wait (it invalidates), and it succeeds */
	w0 = n_waits;
	CHECK(cpu_prep(fd, b, MSM_PREP_READ, 1000) == 0 && n_waits == w0 + 1);
	CHECK(cpu_prep(fd, b, MSM_PREP_WRITE | MSM_PREP_NOSYNC, 0) == 0);
	CHECK(drmCommandWrite(fd, DRM_MSM_GEM_CPU_FINI, &(struct drm_msm_gem_cpu_fini){ .handle = b }, 8) == 0);

	/* 20 cmds, one of them not CMD_BUF: 16 + 3 IBs, fence is the last */
	for (int i = 0; i < 20; i++) {
		cmd[i] = (struct drm_msm_gem_submit_cmd){ .type = MSM_SUBMIT_CMD_BUF, .submit_idx = i & 1, .submit_offset = 4u * i, .size = 8 };
	}
	cmd[7].type = MSM_SUBMIT_CMD_IB_TARGET_BUF;
	uint32_t s0 = n_submits, i0 = total_ibs;
	CHECK(submit(fd, sb, 2, cmd, 20, &fence) == 0);
	CHECK(n_submits == s0 + 2 && total_ibs == i0 + 19 && last_count == 3 && fence == submitted);
	CHECK(last_ib[2].handle == b && last_ib[2].offset == 76 && last_ib[2].size_dw == 2);
	CHECK(cpu_prep(fd, a, MSM_PREP_READ | MSM_PREP_NOSYNC, 0) == -EBUSY);
	retire_all();

	/* bad submits */
	cmd[0] = (struct drm_msm_gem_submit_cmd){ .type = MSM_SUBMIT_CMD_BUF, .submit_idx = 2, .size = 8 };
	s0 = n_submits;
	CHECK(submit(fd, sb, 2, cmd, 1, &fence) == -EINVAL && n_submits == s0);
	cmd[0].submit_idx = 0;
	cmd[0].nr_relocs = 1;
	CHECK(submit(fd, sb, 2, cmd, 1, &fence) == -EINVAL);
	cmd[0].nr_relocs = 0;
	cmd[0].size = 6;
	CHECK(submit(fd, sb, 2, cmd, 1, &fence) == -EINVAL);
	cmd[0].size = 8;
	struct drm_msm_gem_submit_bo bad = { .handle = 999 };
	CHECK(submit(fd, &bad, 1, cmd, 1, &fence) == -EINVAL && n_submits == s0);
	/* outside the BO: the backend (kernel) refuses */
	cmd[0].submit_offset = 0x7ff0;  /* 0x5000 rounds up to 0x8000 */
	cmd[0].size = 64;
	CHECK(submit(fd, sb, 2, cmd, 1, &fence) == -EINVAL);
	struct drm_msm_gem_submit fs = { .flags = MSM_SUBMIT_FENCE_FD_OUT };
	CHECK(drmCommandWriteRead(fd, DRM_MSM_GEM_SUBMIT, &fs, sizeof(fs)) == -EINVAL && last_report[0] != 0);

	/* IOSurface import: a handle with an iova and no map */
	uint32_t s = 0;
	uint64_t ssize = 0;
	CHECK(sweetdrm_import_iosurface(fd, 77, NULL, &s, &ssize) == 0 && s != 0 && ssize == 0x10000);
	CHECK(gem_info(fd, s, MSM_INFO_GET_IOVA, &iova) == 0 && iova != 0);
	CHECK(gem_info(fd, s, MSM_INFO_GET_OFFSET, &off) == -EINVAL);
	CHECK(sweetdrm_import_iosurface(fd, 78, NULL, &s, &ssize) == -ENOENT);
	/* with the surface's own mapping, which Mesa's mmap then gets back */
	static uint32_t surface_pages[0x10000 / 4];
	uint32_t s2 = 0;
	CHECK(sweetdrm_import_iosurface(fd, 77, surface_pages, &s2, &ssize) == 0 && s2 != 0 && s2 != s);
	CHECK(gem_info(fd, s2, MSM_INFO_GET_OFFSET, &off) == 0 &&
	    sweetdrm_mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off) == (void *)surface_pages);
	CHECK(sweetdrm_munmap(surface_pages, 0x10000) == 0);

	/* drmIoctl: errno convention, msm ioctls through it, GEM_CLOSE */
	struct drm_msm_param pp = { .pipe = MSM_PIPE_3D0, .param = MSM_PARAM_GPU_ID };
	CHECK(drmIoctl(fd, DRM_IOCTL_MSM_GET_PARAM, &pp) == 0 && pp.value == 618);
	pp.param = MSM_PARAM_TIMESTAMP;
	errno = 0;
	CHECK(drmIoctl(fd, DRM_IOCTL_MSM_GET_PARAM, &pp) == -1 && errno == EINVAL);
	struct drm_gem_flink fl = { .handle = a };
	CHECK(drmIoctl(fd, DRM_IOCTL_GEM_FLINK, &fl) == -1 && errno == ENOSYS);
	struct drm_gem_close gc = { .handle = a };
	CHECK(drmIoctl(fd2, DRM_IOCTL_GEM_CLOSE, &gc) == 0 && !fbo[a].live);
	CHECK(drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &gc) == -1 && errno == ENOENT);
	CHECK(gem_info(fd, a, MSM_INFO_GET_IOVA, &iova) == -ENOENT);
	/* the freed slot is reused */
	uint32_t c = gem_new(fd, 0x4000);
	CHECK(c == a);

	/* not our fd */
	int other = open("/etc/passwd", 0);
	CHECK(drmCommandWriteRead(other, DRM_MSM_GET_PARAM, &pp, sizeof(pp)) == -EBADF);
	close(other);

	/* a dead GPU: waits say EIO */
	cmd[0] = (struct drm_msm_gem_submit_cmd){ .type = MSM_SUBMIT_CMD_BUF, .submit_idx = 1, .size = 8 };
	CHECK(submit(fd, sb, 2, cmd, 1, &fence) == 0);
	dead = 1;
	CHECK(cpu_prep(fd, b, MSM_PREP_READ, 100) == -EIO);
	dead = 0;

	close(fd2);
	close(fd);
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("test_sweetdrm: all passed (%u submits, %u IBs, %u waits)\n", n_submits, total_ibs, n_waits);
	return 0;
}
