/*
 * sweetdrm: what lies under the libdrm subset (xf86drm.h). The msm emulation in
 * sweetdrm.c talks to a backend: the SweetGPU user client on iOS
 * (sweetdrm_iokit.c, patch 0112's selectors 10-16) or a fake GPU in the host
 * test. Also the one call beyond libdrm, for the EAGL shim (7.5): IOSurface import.
 */
#ifndef SWEETDRM_H
#define SWEETDRM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWEETDRM_MAX_IBS        16u     /* per backend submit, as SWEET_GPU_MAX_IBS */

struct sweetdrm_ib {
	uint32_t handle, offset, size_dw;
};

struct sweetdrm_info {
	uint32_t chip_id, gmem_size, gmem_base, max_khz;
	uint64_t window_base, window_size;
	volatile const uint32_t *fence;         /* word 0 = last retired seqno */
};

/* Every call returns 0 or a positive errno */
struct sweetdrm_backend {
	int (*open)(struct sweetdrm_info *info);
	int (*bo_new)(uint64_t size, uint32_t *handle, uint64_t *gpu_va, void **cpu);
	int (*bo_import)(uint32_t surface_id, uint32_t *handle, uint64_t *gpu_va, uint64_t *size);
	int (*bo_free)(uint32_t handle);
	int (*submit)(const struct sweetdrm_ib *ib, uint32_t count, uint32_t *seqno);
	int (*wait)(uint32_t seqno, uint32_t timeout_ms);   /* ETIMEDOUT, or EIO once the GPU is dead */
	void (*report)(const char *line);                    /* one log line; may be NULL */
};

/* The SweetGPU user client (Apple builds only) */
extern const struct sweetdrm_backend sweetdrm_iokit_backend;

/* Use another backend (the host test); before the first drmOpenWithType */
void sweetdrm_set_backend(const struct sweetdrm_backend *backend);

/*
 * Import an IOSurface as a GEM handle on the sweetdrm fd, for fd_bo_from_handle().
 * cpu is the surface's base address in this task (IOSurfaceGetBaseAddress), which
 * Mesa then maps the BO to, or NULL for a BO Mesa must never map. The kernel keeps
 * the CPU caches in step around submits and waits, as for its own BOs.
 * Returns 0 or -errno.
 */
int sweetdrm_import_iosurface(int fd, uint32_t surface_id, void *cpu, uint32_t *handle, uint64_t *size);

/* The newest seqno submitted so far (0 before the first submit), and whether seq has retired */
uint32_t sweetdrm_last_submitted(void);
int sweetdrm_seq_retired(uint32_t seq);

/* One log line to stderr and the backend's report (the kernel log on iOS, selector 16) */
void sweetdrm_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif
