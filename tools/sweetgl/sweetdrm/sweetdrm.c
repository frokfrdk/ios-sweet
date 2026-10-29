/*
 * sweetdrm: the libdrm subset Mesa's freedreno (msm backend) calls, emulating the
 * msm DRM driver over a sweetdrm backend (ios-sweet plan Stage 7.1). Our own code;
 * the uapi structs and numbers come from Mesa's include/drm-uapi (MIT).
 *
 * The "device fd" is an open /dev/null, so close() and dup() behave; any fd that
 * is /dev/null while the device is open counts as ours (Mesa dup()s it). One
 * device per process.
 *
 * msm mapping:
 *   GET_PARAM          answered from the backend's info (no TIMESTAMP yet)
 *   GEM_NEW/GEM_CLOSE  bo new/free; the backend's handle is the GEM handle
 *   GEM_INFO           iova, a map cookie for sweetdrm_mmap, flags; names ignored
 *   GEM_CPU_PREP       wait for the BO's last submit (a wait also invalidates
 *                      the CPU caches of this client's BOs, patch 0112c)
 *   GEM_SUBMIT         CMD_BUF cmds -> IBs (iova-based, no relocations), at most
 *                      SWEETDRM_MAX_IBS per backend submit; fence = last seqno
 *   WAIT_FENCE         wait for that seqno
 *   SUBMITQUEUE_*      ids only; one ring
 * Reported version: msm 1.7 (softpin submits, GMEM base, robustness, suspends;
 * below 8 so Mesa does not try MSM_BO_CACHED_COHERENT, below 9 for VA_SIZE).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "xf86drm.h"
#include "sweetdrm.h"
#include "drm-uapi/msm_drm.h"

#define MSM_VERSION_MINOR       7
#define GPU_ID                  618
#define MAP_COOKIE_TAG          0x5ull
#define MAP_COOKIE(h)           ((MAP_COOKIE_TAG << 56) | ((uint64_t)(h) << 16))
#define WAIT_CHUNK_MS           10000u  /* SWEET_GPU_WAIT_MAX_MS */

struct bo {
	uint32_t handle;                /* 0 = free slot */
	uint32_t last_seq;              /* 0 = never submitted */
	uint64_t gpu_va, size;
	void *cpu;                      /* an import's is the surface's own mapping, or NULL */
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const struct sweetdrm_backend *be;
static struct sweetdrm_info info;
static bool opened;
static dev_t null_rdev;
static uint32_t next_queue_id;
static struct bo *bos;
static uint32_t nbos, cap;

void
sweetdrm_set_backend(const struct sweetdrm_backend *backend)
{
	be = backend;
}

static void
vsay(const char *who, const char *fmt, va_list ap)
{
	char line[192];
	vsnprintf(line, sizeof(line), fmt, ap);
	fprintf(stderr, "%s: %s\n", who, line);
	if (be && be->report) {
		be->report(line);
	}
}

static void
say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsay("sweetdrm", fmt, ap);
	va_end(ap);
}

void
sweetdrm_say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsay("sweetgl", fmt, ap);
	va_end(ap);
}

static int
fail(int e)
{
	errno = e;
	return -e;
}

static bool
is_ours(int fd)
{
	struct stat st;
	return opened && fd >= 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == null_rdev;
}

static bool
retired(uint32_t seq)
{
	return seq == 0 || (int32_t)(*info.fence - seq) >= 0;
}

/* Lock held */
static struct bo *
find(uint32_t handle)
{
	for (uint32_t i = 0; i < nbos; i++) {
		if (bos[i].handle == handle && handle != 0) {
			return &bos[i];
		}
	}
	return NULL;
}

/* Lock held */
static int
add(uint32_t handle, uint64_t gpu_va, uint64_t size, void *cpu)
{
	struct bo *b = NULL;
	for (uint32_t i = 0; i < nbos && b == NULL; i++) {
		if (bos[i].handle == 0) {
			b = &bos[i];
		}
	}
	if (b == NULL) {
		if (nbos == cap) {
			uint32_t ncap = cap ? cap * 2 : 64;
			struct bo *n = realloc(bos, ncap * sizeof(*n));
			if (n == NULL) {
				return ENOMEM;
			}
			bos = n;
			cap = ncap;
		}
		b = &bos[nbos++];
	}
	*b = (struct bo){ handle, 0, gpu_va, size, cpu };
	return 0;
}

static int64_t
ms_left(const struct drm_msm_timespec *t)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t ns = (t->tv_sec - (int64_t)now.tv_sec) * 1000000000ll + (t->tv_nsec - now.tv_nsec);
	return ns <= 0 ? 0 : (ns + 999999) / 1000000;
}

/* Lock not held: waits can be long and other threads submit meanwhile */
static int
wait_until(uint32_t seq, const struct drm_msm_timespec *deadline)
{
	for (;;) {
		int64_t left = ms_left(deadline);
		int e = be->wait(seq, left > WAIT_CHUNK_MS ? WAIT_CHUNK_MS : (uint32_t)left);
		if (e != ETIMEDOUT || left <= WAIT_CHUNK_MS) {
			return e;
		}
	}
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid;
	(void)type;
	if (name != NULL && strcmp(name, "msm") != 0) {
		return fail(ENODEV);
	}
	pthread_mutex_lock(&lock);
	int e = 0;
	if (!opened) {
		if (be == NULL) {
#if defined(__APPLE__)
			be = &sweetdrm_iokit_backend;
#else
			e = ENODEV;
#endif
		}
		e = e ? e : be->open(&info);
		if (e == 0 && info.fence == NULL) {
			e = ENODEV;
		}
		struct stat st;
		if (e == 0 && stat("/dev/null", &st) == 0) {
			null_rdev = st.st_rdev;
			opened = true;
		}
	}
	pthread_mutex_unlock(&lock);
	if (e) {
		say("no device: %s", strerror(e));
		return fail(e);
	}
	int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	return fd < 0 ? fail(errno) : fd;
}

drmVersionPtr
drmGetVersion(int fd)
{
	if (!is_ours(fd)) {
		errno = EBADF;
		return NULL;
	}
	drmVersionPtr v = calloc(1, sizeof(*v));
	if (v == NULL) {
		return NULL;
	}
	v->version_major = 1;
	v->version_minor = MSM_VERSION_MINOR;
	v->name = strdup("msm");
	v->name_len = 3;
	v->date = strdup("20261028");
	v->date_len = 8;
	v->desc = strdup("sweetdrm over SweetGPU");
	v->desc_len = (int)strlen(v->desc);
	return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
	if (v) {
		free(v->name);
		free(v->date);
		free(v->desc);
		free(v);
	}
}

/* No syncobjs, no prime, nothing else */
int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)capability;
	if (!is_ours(fd)) {
		return fail(EBADF);
	}
	*value = 0;
	return 0;
}

static int
get_param(struct drm_msm_param *p)
{
	switch (p->param) {
	case MSM_PARAM_GPU_ID:          p->value = GPU_ID; return 0;
	case MSM_PARAM_GMEM_SIZE:       p->value = info.gmem_size; return 0;
	case MSM_PARAM_CHIP_ID:         p->value = info.chip_id; return 0;
	case MSM_PARAM_MAX_FREQ:        p->value = (uint64_t)info.max_khz * 1000u; return 0;
	case MSM_PARAM_GMEM_BASE:       p->value = info.gmem_base; return 0;
	case MSM_PARAM_PRIORITIES:      p->value = 1; return 0;
	case MSM_PARAM_PP_PGTABLE:      p->value = 0; return 0;
	case MSM_PARAM_FAULTS:          p->value = 0; return 0;
	case MSM_PARAM_SUSPENDS:        p->value = 0; return 0;
	case MSM_PARAM_VA_START:        p->value = info.window_base; return 0;
	case MSM_PARAM_VA_SIZE:         p->value = info.window_size; return 0;
	default:
		/* TIMESTAMP needs the always-on counter (a kernel change); UCHE_TRAP_BASE has a default */
		return EINVAL;
	}
}

static int
gem_new(struct drm_msm_gem_new *r)
{
	uint32_t h = 0;
	uint64_t va = 0;
	void *cpu = NULL;
	/* All flags give the same BO: CPU-cacheable, kept in step around submit and wait */
	int e = r->size ? be->bo_new(r->size, &h, &va, &cpu) : EINVAL;
	if (e == 0) {
		pthread_mutex_lock(&lock);
		e = add(h, va, r->size, cpu);
		pthread_mutex_unlock(&lock);
		if (e) {
			be->bo_free(h);
		}
	}
	r->handle = e ? 0 : h;
	return e;
}

static int
gem_close(uint32_t handle)
{
	pthread_mutex_lock(&lock);
	struct bo *b = find(handle);
	if (b) {
		b->handle = 0;
	}
	pthread_mutex_unlock(&lock);
	/* the kernel waits (2 s cap) for the GPU before it frees */
	return b ? be->bo_free(handle) : ENOENT;
}

static int
gem_info(struct drm_msm_gem_info *r)
{
	pthread_mutex_lock(&lock);
	struct bo *b = find(r->handle);
	int e = b ? 0 : ENOENT;
	if (b) {
		switch (r->info) {
		case MSM_INFO_GET_OFFSET:
			r->value = b->cpu ? MAP_COOKIE(b->handle) : 0;
			e = b->cpu ? 0 : EINVAL;
			break;
		case MSM_INFO_GET_IOVA:
			r->value = b->gpu_va;
			break;
		case MSM_INFO_GET_FLAGS:
			r->value = MSM_BO_WC;
			break;
		case MSM_INFO_SET_NAME:
			break;
		default:
			/* SET_IOVA (no VM_BIND), names and metadata for sharing */
			e = EINVAL;
		}
	}
	pthread_mutex_unlock(&lock);
	return e;
}

static int
cpu_prep(struct drm_msm_gem_cpu_prep *r)
{
	pthread_mutex_lock(&lock);
	struct bo *b = find(r->handle);
	uint32_t seq = b ? b->last_seq : 0;
	pthread_mutex_unlock(&lock);
	if (b == NULL) {
		return ENOENT;
	}
	if (seq == 0) {
		return 0;
	}
	if ((r->op & MSM_PREP_NOSYNC) && !retired(seq)) {
		return EBUSY;
	}
	/* even when already retired: the wait is what invalidates the CPU's lines */
	return wait_until(seq, &r->timeout);
}

static int
submit(struct drm_msm_gem_submit *r)
{
	if (r->flags & (MSM_SUBMIT_FENCE_FD_IN | MSM_SUBMIT_FENCE_FD_OUT | MSM_SUBMIT_SYNCOBJ_IN |
	    MSM_SUBMIT_SYNCOBJ_OUT | MSM_SUBMIT_FENCE_SN_IN)) {
		say("submit flags %#x: fence fds and syncobjs are not supported", r->flags);
		return EINVAL;
	}
	const struct drm_msm_gem_submit_bo *sb = (const void *)(uintptr_t)r->bos;
	const struct drm_msm_gem_submit_cmd *cmd = (const void *)(uintptr_t)r->cmds;
	struct sweetdrm_ib ib[SWEETDRM_MAX_IBS];
	uint32_t n = 0, seq = 0;
	int e = 0;

	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < r->nr_bos && e == 0; i++) {
		e = find(sb[i].handle) ? 0 : EINVAL;
	}
	for (uint32_t i = 0; i < r->nr_cmds && e == 0; i++) {
		if (cmd[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;       /* IB_TARGET (dumps) and CTX_RESTORE (no context switches here) */
		}
		if (cmd[i].submit_idx >= r->nr_bos || cmd[i].nr_relocs != 0 || (cmd[i].size & 3) ||
		    (cmd[i].submit_offset & 3)) {
			e = EINVAL;
			break;
		}
		ib[n++] = (struct sweetdrm_ib){ sb[cmd[i].submit_idx].handle, cmd[i].submit_offset, cmd[i].size / 4 };
		if (n == SWEETDRM_MAX_IBS || i + 1 == r->nr_cmds) {
			/* the kernel checks each IB lies inside its BO */
			e = be->submit(ib, n, &seq);
			n = 0;
		}
	}
	if (e == 0 && n) {
		e = be->submit(ib, n, &seq);
	}
	if (seq) {
		/* also after a partial failure: what did go out must be waited for */
		for (uint32_t i = 0; i < r->nr_bos; i++) {
			struct bo *b = find(sb[i].handle);
			if (b) {
				b->last_seq = seq;
			}
		}
	}
	pthread_mutex_unlock(&lock);
	r->fence = seq;
	r->fence_fd = -1;
	return e;
}

static int
gem_madvise(struct drm_msm_gem_madvise *r)
{
	pthread_mutex_lock(&lock);
	int e = find(r->handle) ? 0 : ENOENT;
	pthread_mutex_unlock(&lock);
	r->retained = 1;        /* wired pages, never purged */
	return e;
}

static int
queue_query(struct drm_msm_submitqueue_query *r)
{
	if (r->param != MSM_SUBMITQUEUE_PARAM_FAULTS) {
		return EINVAL;
	}
	if (r->data && r->len) {
		memset((void *)(uintptr_t)r->data, 0, r->len < 8 ? r->len : 8);
	}
	r->len = 4;
	return 0;
}

static int
command(unsigned long index, void *data, unsigned long size)
{
	(void)size;
	switch (index) {
	case DRM_MSM_GET_PARAM:         return get_param(data);
	case DRM_MSM_SET_PARAM:         return EINVAL;  /* SYSPROF, COMM, CMDLINE */
	case DRM_MSM_GEM_NEW:           return gem_new(data);
	case DRM_MSM_GEM_INFO:          return gem_info(data);
	case DRM_MSM_GEM_CPU_PREP:      return cpu_prep(data);
	case DRM_MSM_GEM_CPU_FINI:      return 0;
	case DRM_MSM_GEM_SUBMIT:        return submit(data);
	case DRM_MSM_WAIT_FENCE: {
		struct drm_msm_wait_fence *w = data;
		return wait_until(w->fence, &w->timeout);
	}
	case DRM_MSM_GEM_MADVISE:       return gem_madvise(data);
	case DRM_MSM_SUBMITQUEUE_NEW: {
		struct drm_msm_submitqueue *q = data;
		pthread_mutex_lock(&lock);
		q->id = ++next_queue_id;
		pthread_mutex_unlock(&lock);
		return 0;
	}
	case DRM_MSM_SUBMITQUEUE_CLOSE: return 0;
	case DRM_MSM_SUBMITQUEUE_QUERY: return queue_query(data);
	default:
		say("msm command %#lx not supported", index);
		return ENOSYS;
	}
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	if (!is_ours(fd)) {
		return fail(EBADF);
	}
	int e = command(index, data, size);
	return e ? fail(e) : 0;
}

int
drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size)
{
	return drmCommandWriteRead(fd, index, data, size);
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	if (!is_ours(fd)) {
		errno = EBADF;
		return -1;
	}
	unsigned nr = request & 0xffu;
	int e;
	if (nr >= DRM_COMMAND_BASE) {
		e = command(nr - DRM_COMMAND_BASE, arg, 0);
	} else if (request == DRM_IOCTL_GEM_CLOSE) {
		e = gem_close(((struct drm_gem_close *)arg)->handle);
	} else {
		e = ENOSYS;     /* GEM_OPEN, GEM_FLINK, syncobjs: no sharing between processes */
	}
	if (e) {
		errno = e;
		return -1;
	}
	return 0;
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd; (void)handle; (void)flags;
	*prime_fd = -1;
	return fail(ENOSYS);
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	(void)fd; (void)prime_fd;
	*handle = 0;
	return fail(ENOSYS);
}

int drmSyncobjDestroy(int fd, uint32_t handle) { (void)fd; (void)handle; return fail(ENOSYS); }
int drmSyncobjReset(int fd, const uint32_t *h, uint32_t n) { (void)fd; (void)h; (void)n; return fail(ENOSYS); }
int drmSyncobjFDToHandle(int fd, int o, uint32_t *h) { (void)fd; (void)o; (void)h; return fail(ENOSYS); }
int drmSyncobjImportSyncFile(int fd, uint32_t h, int s) { (void)fd; (void)h; (void)s; return fail(ENOSYS); }
int drmSyncobjExportSyncFile(int fd, uint32_t h, int *s) { (void)fd; (void)h; *s = -1; return fail(ENOSYS); }

void *
sweetdrm_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	if (is_ours(fd) && ((uint64_t)offset >> 56) == MAP_COOKIE_TAG) {
		uint32_t h = (uint32_t)((uint64_t)offset >> 16);
		pthread_mutex_lock(&lock);
		struct bo *b = find(h);
		void *cpu = b && length <= b->size + 0x4000u ? b->cpu : NULL;
		pthread_mutex_unlock(&lock);
		if (cpu == NULL) {
			errno = EINVAL;
			return MAP_FAILED;
		}
		return cpu;     /* the kernel mapped it at bo new; addr is a hint Mesa does not need */
	}
	return mmap(addr, length, prot, flags, fd, offset);
}

int
sweetdrm_munmap(void *addr, size_t length)
{
	pthread_mutex_lock(&lock);
	bool ours = false;
	for (uint32_t i = 0; i < nbos && !ours; i++) {
		ours = bos[i].handle && bos[i].cpu == addr;
	}
	pthread_mutex_unlock(&lock);
	return ours ? 0 : munmap(addr, length);  /* ours stays mapped until GEM_CLOSE */
}

int
sweetdrm_import_iosurface(int fd, uint32_t surface_id, void *cpu, uint32_t *handle, uint64_t *size)
{
	if (!is_ours(fd)) {
		return fail(EBADF);
	}
	uint32_t h = 0;
	uint64_t va = 0, sz = 0;
	int e = be->bo_import ? be->bo_import(surface_id, &h, &va, &sz) : ENOSYS;
	if (e == 0) {
		pthread_mutex_lock(&lock);
		e = add(h, va, sz, cpu);
		pthread_mutex_unlock(&lock);
		if (e) {
			be->bo_free(h);
		}
	}
	*handle = e ? 0 : h;
	*size = e ? 0 : sz;
	return e ? fail(e) : 0;
}
