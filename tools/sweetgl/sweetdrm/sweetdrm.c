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
#include <sys/resource.h>
#include <sys/stat.h>

#include "xf86drm.h"
#include "sweetdrm.h"
#include "drm-uapi/msm_drm.h"

#define MSM_VERSION_MINOR       7
#define GPU_ID                  618
#define MAP_COOKIE_TAG          0x5ull
#define MAP_COOKIE(h)           ((MAP_COOKIE_TAG << 56) | ((uint64_t)(h) << 16))
#define WAIT_CHUNK_MS           10000u  /* SWEET_GPU_WAIT_MAX_MS */
#define SUBMIT_SURFACES_MAX     128u    /* imports reported per submit */

struct bo {
	uint32_t handle;                /* 0 = free slot */
	uint32_t last_seq;              /* 0 = never submitted */
	uint64_t gpu_va, size;
	void *cpu;                      /* an import's is the surface's own mapping, or NULL */
	uint32_t surface_id;            /* imports only */
	bool mapped;                    /* Mesa has mapped it (sweetdrm_mmap): cleaned before submits */
	bool seeded;                    /* an import cleaned when its surface had seed `seed` */
	uint32_t seed;
	bool tracked;                   /* G7w: a texture Mesa reports its CPU writes for (sweetdrm_bo_cpu_write) */
	bool dirty;                     /* tracked: written since its last clean */
	bool sticky;                    /* tracked: mapped persistently, so always cleaned */
	uint32_t new_flags;             /* G7x: GEM_NEW's flags (own BOs) */
	uint32_t cleans;                /* G7x: submit cleans since the last time report */
	uint64_t cleaned;               /* G7x: bytes of those */
	uint8_t *shadow;                /* G7y: copy taken after a sampled clean (large own BOs) */
	bool armed;                     /* G7y: shadow is current; compare at the next clean */
	uint32_t samples, pages;        /* G7y: compared cleans and changed pages, since the last time report */
	uint32_t max_pages, lo, hi;     /* G7y: most changed pages in one, lowest and highest changed page */
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const struct sweetdrm_backend *be;
static struct sweetdrm_info info;
static bool opened;
static dev_t null_rdev;
static uint32_t next_queue_id;
static uint32_t last_submitted;         /* newest seqno submitted */
static struct bo *bos;
static uint32_t nbos, cap;
static const struct sweetdrm_surface_hooks *hooks;

void
sweetdrm_set_backend(const struct sweetdrm_backend *backend)
{
	be = backend;
}

void
sweetdrm_set_surface_hooks(const struct sweetdrm_surface_hooks *h)
{
	hooks = h;
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

/* ---- G7p timing ---- */

static struct {
	uint32_t n;
	uint64_t ns, max_ns;
} times[SWEETDRM_T_KINDS];

/* G7v: submit clean of own BOs, of imports, imports skipped; G7w: tracked own BOs skipped, cleaned */
static uint64_t clean_bytes[5];

uint64_t
sweetdrm_now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void
sweetdrm_time_add(enum sweetdrm_time_kind kind, uint64_t start_ns)
{
	uint64_t d = sweetdrm_now_ns() - start_ns, m;
	__atomic_add_fetch(&times[kind].n, 1u, __ATOMIC_RELAXED);
	__atomic_add_fetch(&times[kind].ns, d, __ATOMIC_RELAXED);
	m = __atomic_load_n(&times[kind].max_ns, __ATOMIC_RELAXED);
	while (d > m && !__atomic_compare_exchange_n(&times[kind].max_ns, &m, d, true, __ATOMIC_RELAXED,
	    __ATOMIC_RELAXED)) {
	}
}

/* "calls/total ms/longest ms" of one kind, taken and cleared */
static const char *
time_take(enum sweetdrm_time_kind kind, char *buf, size_t len)
{
	uint32_t n = __atomic_exchange_n(&times[kind].n, 0u, __ATOMIC_RELAXED);
	uint64_t ns = __atomic_exchange_n(&times[kind].ns, 0u, __ATOMIC_RELAXED);
	uint64_t max = __atomic_exchange_n(&times[kind].max_ns, 0u, __ATOMIC_RELAXED);
	snprintf(buf, len, "%u/%llu/%llu", n, (unsigned long long)(ns / 1000000u), (unsigned long long)(max / 1000000u));
	return buf;
}

/*
 * G7x: G7w still cleaned 11-22 GB of own BOs per 5 s and none were textures.
 * List the BOs that took most of it (the first TOP_REPORTS reports with any),
 * then reset every BO's count for the next period.
 */
#define TOP_BOS         8u
#define TOP_REPORTS     12u

/*
 * G7y: the four 4 MiB BOs G7x listed are (by size and flags) Mesa's
 * suballocation heap blocks, cleaned in full at each submit. For own BOs of at
 * least SAMPLE_MIN, every SAMPLE_EVERY-th clean keeps a copy, and the next
 * clean counts the SAMPLE_PAGE pages that differ from it: how much of the
 * block one submit really writes. SWEETDRM_SAMPLE=0 turns it off.
 */
#define SAMPLE_MIN      (1u << 20)
#define SAMPLE_EVERY    64u
#define SAMPLE_PAGE     4096u

static void
top_cleaned(unsigned secs)
{
	static unsigned reports;
	struct bo top[TOP_BOS];
	unsigned ntop = 0, nbo = 0;
	uint64_t all = 0;
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < nbos; i++) {
		struct bo *b = &bos[i];
		if (b->handle == 0 || b->cleans == 0) {
			continue;
		}
		nbo++;
		all += b->cleaned;
		unsigned k = ntop < TOP_BOS ? ntop++ : TOP_BOS;
		for (; k > 0 && top[k - 1].cleaned < b->cleaned; k--) {
			if (k < TOP_BOS) {
				top[k] = top[k - 1];
			}
		}
		if (k < TOP_BOS) {
			top[k] = *b;
		}
	}
	for (uint32_t i = 0; i < nbos; i++) {
		bos[i].cleans = 0;
		bos[i].cleaned = 0;
		bos[i].samples = bos[i].pages = bos[i].max_pages = bos[i].hi = 0;
		bos[i].lo = UINT32_MAX;
	}
	pthread_mutex_unlock(&lock);
	if (nbo == 0 || reports >= TOP_REPORTS) {
		return;
	}
	reports++;
	sweetdrm_say("7.7 time %us: cleaned %u BOs, %llu KiB; largest:", secs, nbo, (unsigned long long)(all >> 10));
	for (unsigned k = 0; k < ntop; k++) {
		const struct bo *b = &top[k];
		sweetdrm_say("7.7 time %us:   bo %u size %llu KiB flags 0x%x%s%s%s cleaned %u times, %llu KiB", secs, b->handle,
		    (unsigned long long)(b->size >> 10), b->new_flags, b->surface_id ? " import" : "",
		    b->tracked ? " texture" : "", b->mapped ? " mapped" : "", b->cleans, (unsigned long long)(b->cleaned >> 10));
		if (b->samples) {
			sweetdrm_say("7.7 time %us:     changed between cleans: %u samples, avg %u max %u of %llu pages, "
			    "pages %u-%u", secs, b->samples, b->pages / b->samples, b->max_pages,
			    (unsigned long long)(b->size / SAMPLE_PAGE), b->lo == UINT32_MAX ? 0 : b->lo, b->hi);
		}
	}
}

void
sweetdrm_time_report(unsigned secs)
{
	static uint64_t cpu_before;
	struct rusage ru;
	uint64_t cpu = 0;
	if (getrusage(RUSAGE_SELF, &ru) == 0) {
		cpu = ((uint64_t)ru.ru_utime.tv_sec + (uint64_t)ru.ru_stime.tv_sec) * 1000u +
		    ((uint64_t)ru.ru_utime.tv_usec + (uint64_t)ru.ru_stime.tv_usec) / 1000u;
	}
	if (__atomic_load_n(&times[SWEETDRM_T_GL].n, __ATOMIC_RELAXED) == 0 &&
	    __atomic_load_n(&times[SWEETDRM_T_SUBMIT].n, __ATOMIC_RELAXED) == 0) {
		cpu_before = cpu;
		return;         /* nothing drawn: keep the log short */
	}
	char a[24], b[24], c[24], d[24], e[24], f[24], g[24];
	sweetdrm_say("7.7 time %us: cpu %llu ms; gl %s lock %s draw %s fbb %s (calls/ms/max ms)", secs,
	    (unsigned long long)(cpu - cpu_before), time_take(SWEETDRM_T_GL, a, sizeof(a)),
	    time_take(SWEETDRM_T_LOCK, b, sizeof(b)), time_take(SWEETDRM_T_DRAW, c, sizeof(c)),
	    time_take(SWEETDRM_T_FBBARRIER, d, sizeof(d)));
	sweetdrm_say("7.7 time %us: flush %s finish %s upload %s bind %s", secs, time_take(SWEETDRM_T_FLUSH, a, sizeof(a)),
	    time_take(SWEETDRM_T_FINISH, b, sizeof(b)), time_take(SWEETDRM_T_UPLOAD, c, sizeof(c)),
	    time_take(SWEETDRM_T_BIND, d, sizeof(d)));
	sweetdrm_say("7.7 time %us: submit %s sclean %s wait %s bonew %s bofree %s import %s cpuacc %s", secs,
	    time_take(SWEETDRM_T_SUBMIT, a, sizeof(a)), time_take(SWEETDRM_T_SUBMIT_CLEAN, g, sizeof(g)),
	    time_take(SWEETDRM_T_WAIT, b, sizeof(b)), time_take(SWEETDRM_T_BO_NEW, c, sizeof(c)),
	    time_take(SWEETDRM_T_BO_FREE, d, sizeof(d)), time_take(SWEETDRM_T_IMPORT, e, sizeof(e)),
	    time_take(SWEETDRM_T_CPU_ACCESS, f, sizeof(f)));
	sweetdrm_say("7.7 time %us: sclean KiB own %llu import %llu import skipped %llu own skipped %llu (texture) "
	    "own texture %llu", secs,
	    (unsigned long long)(__atomic_exchange_n(&clean_bytes[0], 0u, __ATOMIC_RELAXED) >> 10),
	    (unsigned long long)(__atomic_exchange_n(&clean_bytes[1], 0u, __ATOMIC_RELAXED) >> 10),
	    (unsigned long long)(__atomic_exchange_n(&clean_bytes[2], 0u, __ATOMIC_RELAXED) >> 10),
	    (unsigned long long)(__atomic_exchange_n(&clean_bytes[3], 0u, __ATOMIC_RELAXED) >> 10),
	    (unsigned long long)(__atomic_exchange_n(&clean_bytes[4], 0u, __ATOMIC_RELAXED) >> 10));
	top_cleaned(secs);
	cpu_before = cpu;
}

/*
 * Clean and invalidate [addr, addr + size) to PoC by this task's mapping.
 * Not sys_dcache_flush: on iOS arm64 that is only a barrier (Apple's devices
 * are coherent; observed in G7q, where the GPU then ran a stale IB). XNU lets
 * EL0 run DC CIVAC (SCTLR_EL1.UCI; libplatform runs IC IVAU at EL0). 64 bytes
 * is the data cache line of the sunfish cores (Cortex-A76 and A55).
 */
static void
dcache_clean_invalidate(const void *addr, uint64_t size)
{
#if defined(__aarch64__)
	uintptr_t p = (uintptr_t)addr & ~(uintptr_t)63, end = (uintptr_t)addr + (uintptr_t)size;
	for (; p < end; p += 64) {
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
#else
	(void)addr;
	(void)size;
#endif
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
add(uint32_t handle, uint64_t gpu_va, uint64_t size, void *cpu, uint32_t surface_id)
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
	*b = (struct bo){ handle, 0, gpu_va, size, cpu, surface_id, false, false, 0, false, false, false, 0, 0, 0,
	    NULL, false, 0, 0, 0, UINT32_MAX, 0 };
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
		e = add(h, va, r->size, cpu, 0);
		if (e == 0) {
			find(h)->new_flags = r->flags;
		}
		pthread_mutex_unlock(&lock);
		if (e) {
			be->bo_free(h);
		}
	}
	r->handle = e ? 0 : h;
	/* G6u: Mesa crashes soon after a refused BO; say whether the BO table or the GPU window ran out */
	static unsigned refused;
	if (e && __atomic_fetch_add(&refused, 1u, __ATOMIC_RELAXED) < 8u) {
		uint32_t live = 0;
		uint64_t bytes = 0;
		pthread_mutex_lock(&lock);
		for (uint32_t i = 0; i < nbos; i++) {
			if (bos[i].handle != 0) {
				live++;
				bytes += bos[i].size;
			}
		}
		pthread_mutex_unlock(&lock);
		say("bo new %llu bytes refused: %s; %u BOs live, %llu KiB of the %llu KiB window", (unsigned long long)r->size,
		    strerror(e), live, (unsigned long long)(bytes >> 10), (unsigned long long)(info.window_size >> 10));
	}
	return e;
}

static int
gem_close(uint32_t handle)
{
	pthread_mutex_lock(&lock);
	struct bo *b = find(handle);
	uint32_t sid = b ? b->surface_id : 0;
	if (b) {
		b->handle = 0;
		free(b->shadow);
		b->shadow = NULL;
	}
	pthread_mutex_unlock(&lock);
	if (sid && hooks && hooks->closed) {
		hooks->closed(sid);
	}
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

/*
 * Lock held; the BOs are known to exist. G7q (patch 0121): the kernel cleaned
 * every page of every BO of ours before each submit, 7-10 ms each in
 * backboardd (G7p). Clean only what the CPU may have written instead, by our
 * own mappings: BOs Mesa mapped, and imports (other tasks write surfaces too).
 * An import with no mapping here leaves the clean to the kernel.
 * SWEETDRM_CLEAN=0 always leaves it to the kernel.
 * G7v: G7r spent ~85% of each submit here, most of it (guess) on the 6 MB
 * display surfaces only the GPU writes. An import whose IOSurface seed is
 * unchanged since its last clean has had no CPU writer, so it is skipped.
 * SWEETDRM_SEEDSKIP=0 cleans every import every time again.
 * G7w: G7v cleaned 11-20 GB of own BOs per 5 s (~75% of submit time). A
 * texture Mesa reports CPU writes for (patch 0004) is cleaned only when it was
 * written since its last clean; other own BOs (buffers, rings, Mesa's own) as
 * before. SWEETDRM_OWNSKIP=0 cleans every mapped own BO every time again.
 */
/* Lock held. G7y: before a clean, compare with the copy; after it, maybe take one */
static void
sample_changes(struct bo *b, bool after)
{
	static int on = -1;
	if (on < 0) {
		const char *v = getenv("SWEETDRM_SAMPLE");
		on = !(v && v[0] == '0');
		say("submit: count pages of large own BOs changed between cleans: %s", on ? "on" : "off (SWEETDRM_SAMPLE=0)");
	}
	if (!on || b->surface_id || b->size < SAMPLE_MIN) {
		return;
	}
	const uint8_t *cpu = b->cpu;
	if (!after) {
		if (!b->armed) {
			return;
		}
		b->armed = false;
		uint32_t changed = 0;
		for (uint32_t p = 0; p < b->size / SAMPLE_PAGE; p++) {
			if (memcmp(cpu + (uint64_t)p * SAMPLE_PAGE, b->shadow + (uint64_t)p * SAMPLE_PAGE, SAMPLE_PAGE) != 0) {
				changed++;
				b->lo = p < b->lo ? p : b->lo;
				b->hi = p > b->hi ? p : b->hi;
			}
		}
		b->samples++;
		b->pages += changed;
		b->max_pages = changed > b->max_pages ? changed : b->max_pages;
		return;
	}
	if (b->cleans % SAMPLE_EVERY != 1) {
		return;
	}
	if (b->shadow == NULL && (b->shadow = malloc(b->size)) == NULL) {
		return;
	}
	memcpy(b->shadow, cpu, b->size);
	b->armed = true;
}

static uint32_t
clean_submit(const struct drm_msm_gem_submit_bo *sb, uint32_t n)
{
	static int on = -1, seedskip, ownskip;
	if (on < 0) {
		const char *v = getenv("SWEETDRM_CLEAN"), *s = getenv("SWEETDRM_SEEDSKIP"), *o = getenv("SWEETDRM_OWNSKIP");
		on = info.client_clean && !(v && v[0] == '0');
		seedskip = !(s && s[0] == '0');
		ownskip = !(o && o[0] == '0');
		say("submit: skip textures the CPU has not written since their last clean: %s",
		    ownskip ? "on" : "off (SWEETDRM_OWNSKIP=0)");
		say("submit: clean (dc civac) the submit's CPU-written BOs here, not all BOs in the kernel: %s",
		    on ? "on" : info.client_clean ? "off (SWEETDRM_CLEAN=0)" : "off (kernel ABI 1)");
		say("submit: skip imports whose IOSurface seed is unchanged since their last clean: %s",
		    !seedskip ? "off (SWEETDRM_SEEDSKIP=0)" : hooks && hooks->seed ? "on" : "off (no seed hook)");
	}
	if (!on) {
		return 0;
	}
	for (uint32_t i = 0; i < n; i++) {
		const struct bo *b = find(sb[i].handle);
		if (b->surface_id && b->cpu == NULL) {
			return 0;
		}
	}
	uint64_t t0 = sweetdrm_now_ns();
	for (uint32_t i = 0; i < n; i++) {
		struct bo *b = find(sb[i].handle);
		if (b->cpu == NULL || !(b->mapped || b->surface_id)) {
			continue;
		}
		if (b->surface_id) {
			uint32_t seed = 0;
			bool known = seedskip && hooks && hooks->seed && hooks->seed(b->surface_id, &seed);
			if (known && b->seeded && b->seed == seed) {
				__atomic_add_fetch(&clean_bytes[2], b->size, __ATOMIC_RELAXED);
				continue;
			}
			b->seeded = known;
			b->seed = seed;
		} else if (b->tracked && ownskip) {
			if (!b->dirty && !b->sticky) {
				__atomic_add_fetch(&clean_bytes[3], b->size, __ATOMIC_RELAXED);
				continue;
			}
			b->dirty = false;
			__atomic_add_fetch(&clean_bytes[4], b->size, __ATOMIC_RELAXED);
		}
		sample_changes(b, false);
		dcache_clean_invalidate(b->cpu, b->size);
		__atomic_add_fetch(&clean_bytes[b->surface_id ? 1 : 0], b->size, __ATOMIC_RELAXED);
		b->cleans++;
		b->cleaned += b->size;
		sample_changes(b, true);
	}
	sweetdrm_time_add(SWEETDRM_T_SUBMIT_CLEAN, t0);
	return SWEETDRM_SUBMIT_CLEAN;
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
	uint32_t sids[SUBMIT_SURFACES_MAX];
	uint32_t n = 0, seq = 0, nsids = 0, flags = 0;
	int e = 0;

	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < r->nr_bos && e == 0; i++) {
		e = find(sb[i].handle) ? 0 : EINVAL;
	}
	if (e == 0) {
		flags = clean_submit(sb, r->nr_bos);
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
			e = be->submit(ib, n, flags, &seq);
			n = 0;
		}
	}
	if (e == 0 && n) {
		e = be->submit(ib, n, flags, &seq);
	}
	if (seq) {
		last_submitted = seq;
		/* also after a partial failure: what did go out must be waited for */
		for (uint32_t i = 0; i < r->nr_bos; i++) {
			struct bo *b = find(sb[i].handle);
			if (b) {
				b->last_seq = seq;
				if (b->surface_id && nsids < SUBMIT_SURFACES_MAX) {
					sids[nsids++] = b->surface_id;
				}
			}
		}
	}
	pthread_mutex_unlock(&lock);
	if (nsids && hooks && hooks->submitted) {
		hooks->submitted(sids, nsids, seq);
	}
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
timed(enum sweetdrm_time_kind kind, uint64_t start_ns, int e)
{
	sweetdrm_time_add(kind, start_ns);
	return e;
}

static int
command(unsigned long index, void *data, unsigned long size)
{
	(void)size;
	uint64_t t0 = sweetdrm_now_ns();
	switch (index) {
	case DRM_MSM_GET_PARAM:         return get_param(data);
	case DRM_MSM_SET_PARAM:         return EINVAL;  /* SYSPROF, COMM, CMDLINE */
	case DRM_MSM_GEM_NEW:           return timed(SWEETDRM_T_BO_NEW, t0, gem_new(data));
	case DRM_MSM_GEM_INFO:          return gem_info(data);
	case DRM_MSM_GEM_CPU_PREP:      return timed(SWEETDRM_T_WAIT, t0, cpu_prep(data));
	case DRM_MSM_GEM_CPU_FINI:      return 0;
	case DRM_MSM_GEM_SUBMIT:        return timed(SWEETDRM_T_SUBMIT, t0, submit(data));
	case DRM_MSM_WAIT_FENCE: {
		struct drm_msm_wait_fence *w = data;
		return timed(SWEETDRM_T_WAIT, t0, wait_until(w->fence, &w->timeout));
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
		uint64_t t0 = sweetdrm_now_ns();
		e = timed(SWEETDRM_T_BO_FREE, t0, gem_close(((struct drm_gem_close *)arg)->handle));
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
		if (cpu) {
			b->mapped = true;
		}
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

void
sweetdrm_bo_cpu_write(uint32_t handle, int persistent)
{
	pthread_mutex_lock(&lock);
	struct bo *b = find(handle);
	if (b && b->surface_id == 0) {
		b->tracked = true;
		b->dirty = true;
		b->sticky = b->sticky || persistent;
	}
	pthread_mutex_unlock(&lock);
}

uint32_t
sweetdrm_last_submitted(void)
{
	pthread_mutex_lock(&lock);
	uint32_t seq = last_submitted;
	pthread_mutex_unlock(&lock);
	return seq;
}

int
sweetdrm_seq_retired(uint32_t seq)
{
	return opened && retired(seq);
}

/*
 * GET_FLAGS says MSM_BO_WC, so Mesa does no cache maintenance and, once its own
 * fences say a BO is idle, maps it without GEM_CPU_PREP, so the kernel's
 * invalidate after a wait never runs. SWEETDRM_CPUINV=0 turns this off.
 */
void
sweetdrm_cpu_access(void *addr, uint64_t size)
{
	static int on = -1;
	static uint64_t calls, bytes;
	if (on < 0) {
		const char *v = getenv("SWEETDRM_CPUINV");
		on = !(v && v[0] == '0');
		say("cpu access: invalidate idle BOs (dc civac) before Mesa maps them: %s", on ? "on" : "off (SWEETDRM_CPUINV=0)");
	}
	if (!on || addr == NULL || size == 0) {
		return;
	}
	uint64_t t0 = sweetdrm_now_ns();
	dcache_clean_invalidate(addr, size);
	sweetdrm_time_add(SWEETDRM_T_CPU_ACCESS, t0);
	uint64_t c = __atomic_add_fetch(&calls, 1u, __ATOMIC_RELAXED);
	uint64_t b = __atomic_add_fetch(&bytes, size, __ATOMIC_RELAXED);
	if ((c & (c - 1)) == 0) {
		say("cpu access: %llu idle BO maps invalidated, %llu KiB in all", (unsigned long long)c,
		    (unsigned long long)(b >> 10));
	}
}

int
sweetdrm_import_iosurface(int fd, uint32_t surface_id, void *cpu, uint32_t *handle, uint64_t *size)
{
	if (!is_ours(fd)) {
		return fail(EBADF);
	}
	uint32_t h = 0;
	uint64_t va = 0, sz = 0, t0 = sweetdrm_now_ns();
	int e = be->bo_import ? be->bo_import(surface_id, &h, &va, &sz) : ENOSYS;
	sweetdrm_time_add(SWEETDRM_T_IMPORT, t0);
	if (e == 0) {
		pthread_mutex_lock(&lock);
		e = add(h, va, sz, cpu, surface_id);
		pthread_mutex_unlock(&lock);
		if (e) {
			be->bo_free(h);
		}
	}
	*handle = e ? 0 : h;
	*size = e ? 0 : sz;
	return e ? fail(e) : 0;
}
