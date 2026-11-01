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
	int client_clean;                       /* the backend takes SWEETDRM_SUBMIT_CLEAN (patch 0121) */
};

/*
 * Submit flag: sweetdrm cleaned to PoC, by this task's mappings, every BO of
 * the submit the CPU may have written, so the kernel need not clean all of them.
 */
#define SWEETDRM_SUBMIT_CLEAN   1u

/* Every call returns 0 or a positive errno */
struct sweetdrm_backend {
	int (*open)(struct sweetdrm_info *info);
	int (*bo_new)(uint64_t size, uint32_t *handle, uint64_t *gpu_va, void **cpu);
	int (*bo_import)(uint32_t surface_id, uint32_t *handle, uint64_t *gpu_va, uint64_t *size);
	int (*bo_free)(uint32_t handle);
	int (*submit)(const struct sweetdrm_ib *ib, uint32_t count, uint32_t flags, uint32_t *seqno);
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
 * Mesa then maps the BO to, or NULL for a BO Mesa must never map. Before submits
 * sweetdrm cleans it by cpu (or, with no cpu, has the kernel clean all BOs); the
 * kernel invalidates after waits, as for its own BOs.
 * Returns 0 or -errno.
 */
int sweetdrm_import_iosurface(int fd, uint32_t surface_id, void *cpu, uint32_t *handle, uint64_t *size);

/* The newest seqno submitted so far (0 before the first submit), and whether seq has retired */
uint32_t sweetdrm_last_submitted(void);
int sweetdrm_seq_retired(uint32_t seq);

/*
 * Told after each submit which imported surfaces it used (their IOSurface ids;
 * seq is the submit's seqno), and when an import's handle is closed. Called
 * without sweetdrm's lock, on the thread that submitted or closed. Set before
 * the first submit; NULL members are skipped.
 * seed (G7v): the surface's IOSurface seed, which changes when a CPU writer
 * unlocks it; 0 if unknown. Called WITH sweetdrm's lock held, before a submit's
 * clean, so it must not call into sweetdrm. An import whose seed is the one it
 * had at its last clean is not cleaned again.
 */
struct sweetdrm_surface_hooks {
	void (*submitted)(const uint32_t *surface_ids, uint32_t count, uint32_t seq);
	void (*closed)(uint32_t surface_id);
	int (*seed)(uint32_t surface_id, uint32_t *seed);
};
void sweetdrm_set_surface_hooks(const struct sweetdrm_surface_hooks *hooks);

/*
 * Mesa is about to touch an idle BO with the CPU without asking the kernel
 * (patch 0003): clean and invalidate that range, so lines the CPU fetched while
 * the GPU was writing are not used or written back over the GPU's data.
 */
void sweetdrm_cpu_access(void *addr, uint64_t size);

/*
 * Mesa mapped (or unmapped) a texture's BO for writing (patch 0004). From then
 * on the submit clean skips that BO unless it was written since its last
 * clean; persistent != 0 keeps it cleaned at every submit.
 */
void sweetdrm_bo_cpu_write(uint32_t handle, int persistent);

/*
 * G7p timing: each kind counts its calls with the time they took (monotonic),
 * the total and the longest. sweetdrm times its own kinds; the EAGL shim times
 * the GL ones. sweetdrm_time_report logs them with this process's CPU time
 * since the last report and starts over.
 */
enum sweetdrm_time_kind {
	SWEETDRM_T_GL,                  /* inside any shim slot, GL lock held */
	SWEETDRM_T_LOCK,                /* waiting for the GL lock */
	SWEETDRM_T_DRAW,                /* glDraw*, with the barrier */
	SWEETDRM_T_FBBARRIER,           /* the framebuffer-fetch barrier alone */
	SWEETDRM_T_FLUSH,
	SWEETDRM_T_FINISH,              /* glFinish, FinishObjectAPPLE, ClientWaitSync */
	SWEETDRM_T_UPLOAD,              /* glTexImage2D and the like */
	SWEETDRM_T_BIND,                /* an IOSurface bound as a texture or renderbuffer */
	SWEETDRM_T_SUBMIT,
	SWEETDRM_T_WAIT,                /* GEM_CPU_PREP and WAIT_FENCE */
	SWEETDRM_T_BO_NEW,
	SWEETDRM_T_BO_FREE,             /* GEM_CLOSE: the kernel waits for the GPU first */
	SWEETDRM_T_IMPORT,
	SWEETDRM_T_CPU_ACCESS,
	SWEETDRM_T_SUBMIT_CLEAN,        /* G7q: the clean of a submit's BOs, inside SUBMIT */
	SWEETDRM_T_KINDS
};
uint64_t sweetdrm_now_ns(void);
void sweetdrm_time_add(enum sweetdrm_time_kind kind, uint64_t start_ns);
void sweetdrm_time_report(unsigned secs);

/* One log line to stderr and the backend's report (the kernel log on iOS, selector 16) */
void sweetdrm_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif
