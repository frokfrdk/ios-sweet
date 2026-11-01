/*
 * sweeteagl (ios-sweet plan Stage 7.5): EAGLContext on Mesa's freedreno, so
 * that Core Animation's GLES renderer draws through SweetGPU. Our own code,
 * written from docs/EAGL_NOTES.md (Stage 7.4).
 *
 * The shim does not replace OpenGLES.framework. Once that framework's
 * EAGLContext class is in the process, sweeteagl_install() swaps the
 * implementations of the methods CA uses for ours:
 *
 *   -initWithAPI:[properties:|sharegroup:]  a sweetgl context (GLES 3.2 on
 *                     freedreno) and a _private block laid out as OpenGLES's,
 *                     whose 1036-slot macro table points at Mesa
 *   +setCurrentContext: / +currentContext  TSD slot 30 (as OpenGLES, so its gl*
 *                     exports reach the same table) plus sweetgl_make_current
 *   -getMacroContextPrivate, -API, -setParameter:to:, -getParameter:to:
 *   -attachImage:toCoreSurface:invertedRender:, -texImageIOSurface:...  the
 *                     bound texture takes the IOSurface's pages as storage (a
 *                     sweetgl image through glEGLImageTargetTexture2DOES)
 *   -renderbufferStorage:fromDrawable:, -presentRenderbuffer:  logged, NO: CA's
 *                     server path presents through IOMFB itself
 *   -dealloc
 *
 * Built without ARC. It does not link OpenGLES.framework: the class is looked up
 * at run time, so the gl* names used here are always Mesa's.
 *
 * The GPU device is opened on the first context. FD_MESA_DEBUG defaults to
 * nogmem,noubwc and GALLIUM_THREAD to 0, the configuration 7.3 passed with
 * (G7s added inorder), and IR3_SHADER_DEBUG to nofp16 (G7y).
 * In backboardd (7.7) that first context can come before the kernel has
 * published SweetGPU, so the open waits for it, up to SWEETEAGL_WAIT_S.
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>

#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include "xf86drm.h"
#include "sweetdrm.h"
#include "sweeteagl.h"

/* The public IOSurface.framework calls (the iPhoneOS SDK has no headers for them) */
typedef struct __IOSurface *IOSurfaceRef;
uint32_t IOSurfaceGetID(IOSurfaceRef buffer);
void *IOSurfaceGetBaseAddress(IOSurfaceRef buffer);
size_t IOSurfaceGetBytesPerRow(IOSurfaceRef buffer);
size_t IOSurfaceGetWidth(IOSurfaceRef buffer);
size_t IOSurfaceGetHeight(IOSurfaceRef buffer);
uint32_t IOSurfaceGetPixelFormat(IOSurfaceRef buffer);
size_t IOSurfaceGetBytesPerElement(IOSurfaceRef buffer);
void IOSurfaceIncrementUseCount(IOSurfaceRef buffer);
void IOSurfaceDecrementUseCount(IOSurfaceRef buffer);
Boolean IOSurfaceIsInUse(IOSurfaceRef buffer);
IOSurfaceRef IOSurfaceLookup(uint32_t csid);
uint32_t IOSurfaceGetSeed(IOSurfaceRef buffer);

#define TSD_EAGL_SLOT           30      /* OpenGLES's thread-specific slot for the current priv */
#define PNAME_MULTITHREADED     0x139
#define SWEETEAGL_WAIT_S        300u    /* for SweetGPU; G6o published it ~260 s after launchd */
#define BIND_LOG_MAX            16u     /* IOSurface binds reported */
#define BIND_SAMPLE_MAX         32u     /* small BGRA/RGBA surfaces whose pixels are sampled at bind */
#define CONTEXT_LOG_MAX         4u      /* contexts reported */
#define DIAG_SURFACES           16u     /* bound surfaces whose pixels the diag thread samples; G6x: the
                                         * 128x128 icon candidates are empty at bind, so follow them after */
#define DIAG_PERIOD_S           5u
#define DIAG_REPORTS            120u    /* 10 minutes */
#define DIAG_GRID               16u     /* samples per row and per column */

/* bsd/sys/kern_memorystatus.h (private): only to log the jetsam limit the process runs under */
#define MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES        8
typedef struct {
	int32_t memlimit_active;        /* MB */
	uint32_t memlimit_active_attr;
	int32_t memlimit_inactive;      /* MB */
	uint32_t memlimit_inactive_attr;
} memlimit_props;
extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t buffersize);

#define SWEETEAGL_EXPORT        __attribute__((visibility("default")))

static Class eagl;                      /* EAGLContext */
static ptrdiff_t private_offset;        /* of its _private ivar */
static struct sweetgl_display *display;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC egl_image_target_texture_2d;

/* ---- the current context: TSD slot 30, as OpenGLES keeps it ---- */

static inline void **
tsd(void)
{
	uintptr_t t;
	__asm__ volatile("mrs %0, tpidrro_el0" : "=r"(t));
	return (void **)(t & ~(uintptr_t)7);
}

static inline struct sweeteagl_priv *
current_priv(void)
{
	return tsd()[TSD_EAGL_SLOT];
}

static inline struct sweeteagl_priv *
priv_of(id ctx)
{
	return *(struct sweeteagl_priv **)((char *)ctx + private_offset);
}

#define HEAL_LOG_MAX            8u      /* slot calls that had to make their context current, reported */

void
sweeteagl_ensure_current(void *ctx)
{
	struct sweeteagl_priv *p = ctx;
	struct sweetgl_context *was = sweetgl_get_current();
	if (p == NULL || was == p->gl) {
		return;
	}
	atomic_fetch_add_explicit(&sweeteagl_diag.not_current, 1u, memory_order_relaxed);
	bool ok = sweetgl_make_current(p->gl);
	static unsigned logged;
	if (__atomic_fetch_add(&logged, 1u, __ATOMIC_RELAXED) < HEAL_LOG_MAX) {
		uint64_t tid = 0;
		pthread_threadid_np(NULL, &tid);
		sweetdrm_say("7.7 eagl: slot call on thread %llu had Mesa context %p, not %p (TSD slot %s): made current -> %s",
		    (unsigned long long)tid, (void *)was, (void *)p->gl, current_priv() == p ? "has it" : "does not",
		    ok ? "ok" : "FAILED");
	}
}

#define CONTENTION_LOG_MAX      8u

static pthread_mutex_t gl_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

/*
 * G6y: the 128x128 icon surfaces stay empty after drawing, so nothing CA does
 * reaches their pages. After the first ones are bound, log the calls that
 * follow (with the bound draw framebuffer and 2D texture) to see how CA means
 * to fill them. Guarded by gl_lock.
 */
#define TRACE_CALLS             60u     /* calls logged after each traced bind */
#define TRACE_BINDS             3u      /* icon-sized binds that start a trace */
static unsigned trace_left;
static uint64_t gl_since;       /* G7p: when the holder took gl_lock */

int
sweeteagl_enter(void *p, const char *fn)
{
	if (pthread_mutex_trylock(&gl_lock) != 0) {
		static unsigned logged;
		uint64_t t0 = sweetdrm_now_ns();
		atomic_fetch_add_explicit(&sweeteagl_diag.contended, 1u, memory_order_relaxed);
		if (__atomic_fetch_add(&logged, 1u, __ATOMIC_RELAXED) < CONTENTION_LOG_MAX) {
			uint64_t tid = 0;
			pthread_threadid_np(NULL, &tid);
			sweetdrm_say("7.7 eagl: thread %llu waits for the GL lock: another thread is in Mesa",
			    (unsigned long long)tid);
		}
		pthread_mutex_lock(&gl_lock);
		sweetdrm_time_add(SWEETDRM_T_LOCK, t0);
	}
	gl_since = sweetdrm_now_ns();
	sweeteagl_ensure_current(p);
	if (trace_left > 0) {
		trace_left--;
		/* -1: a shim method with no context of its own, nothing to ask */
		GLint fb = -1, tex = -1;
		if (p != NULL) {
			glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
		}
		sweetdrm_say("7.7 trace: %s (fb %d tex %d)", fn, fb, tex);
	}
	return 1;
}

void
sweeteagl_leave(int *entered)
{
	(void)entered;
	sweetdrm_time_add(SWEETDRM_T_GL, gl_since);
	pthread_mutex_unlock(&gl_lock);
}

/* ---- the GPU, opened once ---- */

static pthread_once_t display_once = PTHREAD_ONCE_INIT;

static unsigned
footprint_mb(void)
{
	task_vm_info_data_t vm;
	mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &n) != KERN_SUCCESS) {
		return 0;
	}
	return (unsigned)(vm.phys_footprint >> 20);
}

static void use_install(void);

static void
open_display(void)
{
	/* G7s: inorder (no batch reordering) with the client clean (G7r) ended the flashing; neither did alone */
	setenv("FD_MESA_DEBUG", "nogmem,noubwc,inorder", 0);
	setenv("GALLIUM_THREAD", "0", 0);
	/* G7y: test whether fwidth() on mediump (fp16) values makes the striped Clock hands */
	setenv("IR3_SHADER_DEBUG", "nofp16", 0);
	sweetdrm_say("7.7 eagl: FD_MESA_DEBUG=%s IR3_SHADER_DEBUG=%s", getenv("FD_MESA_DEBUG"),
	    getenv("IR3_SHADER_DEBUG"));
	/* ENODEV: no SweetGPU service yet; anything else (EPERM: not allowed to open it) is final */
	unsigned waited = 0;
	int fd;
	while ((fd = drmOpenWithType("msm", NULL, 0)) < 0 && errno == ENODEV && waited < SWEETEAGL_WAIT_S) {
		sleep(1);
		++waited;
	}
	if (fd < 0) {
		sweetdrm_say("7.5 eagl: no SweetGPU device (%d) after %u s; no GL contexts", fd, waited);
		return;
	}
	use_install();
	memlimit_props m = { 0 };
	int r = memorystatus_control(MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES, getpid(), 0, &m, sizeof(m));
	sweetdrm_say("7.7 eagl: %s pid %d uid %d: SweetGPU open after %u s; footprint %u MB, jetsam limit %d MB "
	    "(get %d)", getprogname(), getpid(), getuid(), waited, footprint_mb(), m.memlimit_active, r);
	display = sweetgl_display_open(fd);
	if (display == NULL) {
		sweetdrm_say("7.5 eagl: no Mesa screen on SweetGPU");
		close(fd);
		return;
	}
	sweetdrm_say("7.5 eagl: display \"%s\"", sweetgl_display_name(display));
}

/*
 * ---- 7.7 diagnostics ----
 * Every DIAG_PERIOD_S a thread logs the slot counters (when they changed) and
 * samples the pixels of the surfaces CA bound, read through their CPU
 * mapping: the GPU renders into those pages, and MDP scans them out. The
 * surfaces are retained so their pages stay mapped while sampled.
 */

static pthread_mutex_t diag_lock = PTHREAD_MUTEX_INITIALIZER;
static IOSurfaceRef diag_surf[DIAG_SURFACES];
static uint32_t diag_surf_id[DIAG_SURFACES];

static void
diag_record_surface(IOSurfaceRef surf, uint32_t sid)
{
	pthread_mutex_lock(&diag_lock);
	for (unsigned k = 0; k < DIAG_SURFACES; k++) {
		if (diag_surf[k] != NULL && diag_surf_id[k] == sid) {
			break;
		}
		if (diag_surf[k] == NULL) {
			CFRetain(surf);
			diag_surf[k] = surf;
			diag_surf_id[k] = sid;
			break;
		}
	}
	pthread_mutex_unlock(&diag_lock);
}

/* nonzero samples of a DIAG_GRID x DIAG_GRID grid, and the centre pixel */
static unsigned
diag_sample(IOSurfaceRef surf, uint32_t *centre)
{
	const uint8_t *base = IOSurfaceGetBaseAddress(surf);
	size_t w = IOSurfaceGetWidth(surf), h = IOSurfaceGetHeight(surf), bpr = IOSurfaceGetBytesPerRow(surf);
	*centre = 0;
	if (base == NULL || w == 0 || h == 0) {
		return 0;
	}
	unsigned nonzero = 0;
	for (unsigned y = 0; y < DIAG_GRID; y++) {
		for (unsigned x = 0; x < DIAG_GRID; x++) {
			size_t px = (2 * x + 1) * w / (2 * DIAG_GRID), py = (2 * y + 1) * h / (2 * DIAG_GRID);
			nonzero += *(const volatile uint32_t *)(base + py * bpr + px * 4) != 0;
		}
	}
	*centre = *(const volatile uint32_t *)(base + (h / 2) * bpr + (w / 2) * 4);
	return nonzero;
}

#define LOAD(f) atomic_load_explicit(&sweeteagl_diag.f, memory_order_relaxed)

static void use_reap(void);

static void *
diag_thread(void *arg)
{
	(void)arg;
	unsigned prev[16] = { 0 }, prev_nz[DIAG_SURFACES], prev_c[DIAG_SURFACES];
	memset(prev_nz, 0xff, sizeof(prev_nz));
	memset(prev_c, 0xff, sizeof(prev_c));
	for (unsigned r = 1; r <= DIAG_REPORTS; r++) {
		sleep(DIAG_PERIOD_S);
		use_reap();     /* in case CA stops flushing: release what has retired */
		sweetdrm_time_report(r * DIAG_PERIOD_S);
		unsigned now[16] = { LOAD(draws), LOAD(clears), LOAD(fb_binds), LOAD(programs), LOAD(flushes),
		    LOAD(finishes), LOAD(not_current), LOAD(compiles), LOAD(compile_fails), LOAD(links), LOAD(link_fails),
		    LOAD(fb_incomplete), LOAD(gl_errors), LOAD(wait_fails), LOAD(gen_calls), LOAD(unknown_calls) };
		if (memcmp(now, prev, sizeof(now)) != 0 || r == 1) {
			sweetdrm_say("7.7 diag %us: draw %u clear %u fbbind %u prog %u flush %u finish %u NOTCUR %u", r * DIAG_PERIOD_S,
			    now[0], now[1], now[2], now[3], now[4], now[5], now[6]);
			sweetdrm_say("7.7 diag %us: compile %u fail %u link %u fail %u fbinc %u glerr %u (last 0x%x) waitfail %u "
			    "gen %u unknown %u contended %u", r * DIAG_PERIOD_S, now[7], now[8], now[9], now[10], now[11], now[12],
			    LOAD(last_gl_error), now[13], now[14], now[15], LOAD(contended));
			memcpy(prev, now, sizeof(prev));
		}
		pthread_mutex_lock(&diag_lock);
		for (unsigned k = 0; k < DIAG_SURFACES && diag_surf[k] != NULL; k++) {
			uint32_t c;
			unsigned nz = diag_sample(diag_surf[k], &c);
			if (nz != prev_nz[k] || c != prev_c[k]) {
				sweetdrm_say("7.7 diag %us: surface %u: %u/%u samples nonzero, centre 0x%08x", r * DIAG_PERIOD_S,
				    diag_surf_id[k], nz, DIAG_GRID * DIAG_GRID, c);
				prev_nz[k] = nz;
				prev_c[k] = c;
			}
		}
		pthread_mutex_unlock(&diag_lock);
	}
	sweetdrm_say("7.7 diag: done after %u reports", DIAG_REPORTS);
	return NULL;
}

static void
diag_start(void)
{
	pthread_t t;
	if (pthread_create(&t, NULL, diag_thread, NULL) == 0) {
		pthread_detach(t);
	} else {
		sweetdrm_say("7.7 diag: no thread");
	}
}

static pthread_once_t diag_once = PTHREAD_ONCE_INIT;

/*
 * ---- 7.7 IOSurface use counts (G7i, per submit since G7j) ----
 * Apple's GPU driver keeps a surface "in use" while queued GPU work reads or
 * writes it, and CA's clients check that (IOSurfaceIsInUse) before they draw
 * into a backing store in place. Holding only from a bind to the next flush
 * (3572fed) missed nearly all of it: CA binds a surface once (32 binds in
 * 415 s of G7j) and then samples the texture frame after frame. sweetdrm now
 * says which imported surfaces each submit used, and each holds a use count
 * until the newest submit using it has retired. A bind holds too, until the
 * first flush after it, for the time before CA's draws are submitted.
 * SWEETEAGL_USE=0 turns all of it off.
 */
#define USE_TRACK_MAX           512u

struct use_entry {
	uint32_t sid;
	IOSurfaceRef surf;              /* retained while the entry lives */
	uint32_t seq;                   /* newest submit using it; 0 = bound, no flush yet */
	bool held;                      /* one use count of ours on it */
	bool pending;                   /* bound since the last flush */
};
static pthread_mutex_t use_lock = PTHREAD_MUTEX_INITIALIZER;
static struct use_entry use_track[USE_TRACK_MAX];
static unsigned use_n;
static int use_on = -1;
static unsigned use_binds, use_submits, use_refs, use_busy_at_hold, use_holds, use_released, use_closed, use_full,
    use_nolookup;

static bool
use_enabled(void)
{
	if (use_on < 0) {
		const char *v = getenv("SWEETEAGL_USE");
		use_on = !(v && v[0] == '0');
	}
	return use_on;
}

/* use_lock held */
static struct use_entry *
use_find_locked(uint32_t sid)
{
	for (unsigned i = 0; i < use_n; i++) {
		if (use_track[i].sid == sid) {
			return &use_track[i];
		}
	}
	return NULL;
}

/* use_lock held; surf is retained for the entry by the caller */
static struct use_entry *
use_add_locked(uint32_t sid, IOSurfaceRef surf)
{
	if (use_n == USE_TRACK_MAX) {
		use_full++;
		CFRelease(surf);
		return NULL;
	}
	use_track[use_n] = (struct use_entry){ sid, surf, 0, false, false };
	return &use_track[use_n++];
}

/* use_lock held */
static void
use_take_locked(struct use_entry *e)
{
	if (!e->held) {
		if (IOSurfaceIsInUse(e->surf)) {
			use_busy_at_hold++;
		}
		IOSurfaceIncrementUseCount(e->surf);
		e->held = true;
		use_holds++;
	}
}

/* use_lock held: give back the counts whose work has retired; entries stay until the handle closes */
static void
use_reap_locked(void)
{
	for (unsigned i = 0; i < use_n; i++) {
		struct use_entry *e = &use_track[i];
		if (e->held && !e->pending && sweetdrm_seq_retired(e->seq)) {
			IOSurfaceDecrementUseCount(e->surf);
			e->held = false;
			use_released++;
		}
	}
}

static void
use_log(void)
{
	pthread_mutex_lock(&use_lock);
	unsigned n = use_n, held = 0;
	for (unsigned i = 0; i < use_n; i++) {
		held += use_track[i].held;
	}
	unsigned s = use_submits, refs = use_refs, b = use_binds, busy = use_busy_at_hold, holds = use_holds,
	    rel = use_released, cl = use_closed, full = use_full, nl = use_nolookup;
	pthread_mutex_unlock(&use_lock);
	sweetdrm_say("7.7 eagl: use counts: %u submits with surfaces (%u refs), %u binds; %u holds, %u already in use "
	    "then, %u released, %u held now of %u tracked, %u closed, %u untracked, %u lookups failed", s, refs, b, holds,
	    busy, rel, held, n, cl, full, nl);
}

static void
use_hold(IOSurfaceRef surf)
{
	if (!use_enabled()) {
		return;
	}
	uint32_t sid = IOSurfaceGetID(surf);
	pthread_mutex_lock(&use_lock);
	use_binds++;
	use_reap_locked();
	struct use_entry *e = use_find_locked(sid);
	if (e == NULL) {
		CFRetain(surf);
		e = use_add_locked(sid, surf);
	}
	if (e) {
		use_take_locked(e);
		e->pending = true;
	}
	pthread_mutex_unlock(&use_lock);
}

/* sweetdrm: a submit with seqno seq used these imported surfaces */
static void
use_submitted(const uint32_t *sids, uint32_t count, uint32_t seq)
{
	pthread_mutex_lock(&use_lock);
	unsigned s = ++use_submits;
	use_refs += count;
	for (uint32_t k = 0; k < count; k++) {
		struct use_entry *e = use_find_locked(sids[k]);
		if (e == NULL) {
			IOSurfaceRef surf = IOSurfaceLookup(sids[k]);
			if (surf == NULL) {
				use_nolookup++;
				continue;
			}
			e = use_add_locked(sids[k], surf);
			if (e == NULL) {
				continue;
			}
		}
		use_take_locked(e);
		e->seq = seq;
	}
	use_reap_locked();
	pthread_mutex_unlock(&use_lock);
	if ((s & (s - 1)) == 0 && s >= 64u) {
		use_log();
	}
}

/* sweetdrm: the import's handle is gone (Mesa freed the texture) */
static void
use_closed_hook(uint32_t sid)
{
	pthread_mutex_lock(&use_lock);
	struct use_entry *e = use_find_locked(sid);
	if (e) {
		if (e->held) {
			IOSurfaceDecrementUseCount(e->surf);
		}
		CFRelease(e->surf);
		*e = use_track[--use_n];
		use_closed++;
	}
	pthread_mutex_unlock(&use_lock);
}

/*
 * sweetdrm, its lock held, before a submit's clean (G7v): the seed of a
 * tracked surface. Only use_lock is taken here, and nothing under use_lock
 * takes sweetdrm's lock (sweetdrm_seq_retired takes none).
 */
static int
use_seed_hook(uint32_t sid, uint32_t *seed)
{
	pthread_mutex_lock(&use_lock);
	struct use_entry *e = use_find_locked(sid);
	if (e) {
		*seed = IOSurfaceGetSeed(e->surf);
	}
	pthread_mutex_unlock(&use_lock);
	return e != NULL;
}

static const struct sweetdrm_surface_hooks use_hooks = { use_submitted, use_closed_hook, use_seed_hook };

/* Before the first submit */
static void
use_install(void)
{
	if (use_enabled()) {
		sweetdrm_set_surface_hooks(&use_hooks);
	}
	sweetdrm_say("7.7 eagl: IOSurface use counts per submit: %s", use_enabled() ? "on" : "off (SWEETEAGL_USE=0)");
}

void
sweeteagl_after_flush(void)
{
	uint32_t seq = sweetdrm_last_submitted();
	pthread_mutex_lock(&use_lock);
	for (unsigned i = 0; i < use_n; i++) {
		struct use_entry *e = &use_track[i];
		if (e->pending) {
			/* the newest submit covers everything CA queued before this flush */
			e->pending = false;
			if ((int32_t)(seq - e->seq) > 0) {
				e->seq = seq;
			}
		}
	}
	use_reap_locked();
	pthread_mutex_unlock(&use_lock);
}

static void
use_reap(void)
{
	pthread_mutex_lock(&use_lock);
	use_reap_locked();
	pthread_mutex_unlock(&use_lock);
}

/*
 * ---- G7v stripe check ----
 * Small details (the Clock icon's hands: 19x46 and 19x67 RGBA surfaces with
 * 128-byte rows, two cache lines each) show as hatched stripes. Are the stripes
 * in memory, or only in what the GPU reads? Some seconds after a small surface
 * is bound, at CA's next glBindTexture of that texture, its rows are read by
 * the CPU (vm_read_overwrite: these can wrap client memory that goes away, G7j)
 * and by the GPU (glReadPixels through a read framebuffer), and compared.
 * Twice per surface (>= 3 s and >= 30 s after the bind). Everything here runs
 * under the GL lock. SWEETEAGL_STRIPE=0 turns it off.
 */
#define STRIPE_SURFACES         16u
#define STRIPE_MAX_DIM          256u
#define STRIPE_CHECKS           2u
#define STRIPE_ROWS_PER_LINE    96u

struct stripe_entry {
	struct sweetgl_context *gl;     /* NULL = free */
	unsigned tex;
	IOSurfaceRef surf;              /* retained */
	uint32_t sid;
	unsigned w, h, checks, probes;
	uint64_t bound_ns;
};
static struct stripe_entry stripe[STRIPE_SURFACES];
static unsigned stripe_live;
static int stripe_on = -1;

/* G7w draw probe: the next draw after such a texture is bound */
#define PROBE_PER_SURFACE       1u
#define PROBE_MAX               16u
#define PROBE_MAX_DIM           512
#define PROBE_CELLS             64
#define PROBE_BOX               96      /* G7x: <= the log line's room */
static struct stripe_entry *probe_next;
static GLint probe_unit;
static unsigned probe_total;

static bool
stripe_enabled(void)
{
	if (stripe_on < 0) {
		const char *v = getenv("SWEETEAGL_STRIPE");
		stripe_on = !(v && v[0] == '0');
		sweetdrm_say("7.7 stripe: compare small surfaces as CPU and GPU read them: %s",
		    stripe_on ? "on" : "off (SWEETEAGL_STRIPE=0)");
	}
	return stripe_on;
}

static struct stripe_entry *
stripe_find(struct sweetgl_context *gl, unsigned tex)
{
	for (unsigned k = 0; k < STRIPE_SURFACES; k++) {
		if (stripe[k].gl == gl && stripe[k].tex == tex && gl != NULL) {
			return &stripe[k];
		}
	}
	return NULL;
}

static void
stripe_drop(struct stripe_entry *e)
{
	CFRelease(e->surf);
	if (probe_next == e) {
		probe_next = NULL;
	}
	*e = (struct stripe_entry){ 0 };
	stripe_live--;
}

/*
 * G8b: the kernel rounds an IOSurface's bytes per row up to 64 (xnu 0046), but
 * whatever fills the small CPU-drawn ones still writes rows at width * bpe
 * rounded up to 32 (G8a: a 19 px surface's rows land 96 bytes apart in its
 * 128-byte stride). texImageIOSurface copies such a surface into the texture at
 * the writer's stride instead of importing it, and copies it again on a later
 * bind when its seed or its bytes changed. Surfaces this process renders into
 * (attachImage) keep the real stride. SWEETEAGL_ROWFIX=0 turns this off.
 */
#define ROWFIX_SURFACES         64
#define ROWFIX_MAX_BYTES        (4u << 20)
#define ROWFIX_COMPARE          (64u << 10)     /* up to this size the bytes are compared too, not only the seed */
#define ROWFIX_COMPARE_NS       100000000ull
#define ROWFIX_RENDERED         64
#define ROWFIX_LOG_MAX          40u

struct rowfix_entry {
	struct sweetgl_context *gl;     /* NULL = free */
	unsigned tex;
	IOSurfaceRef surf;              /* retained */
	uint32_t sid, seed;
	unsigned w, h, ws, bpe;
	GLenum fmt;
	uint8_t *copy, *fresh;          /* the bytes last uploaded; a second buffer to compare (small ones only) */
	size_t bytes;
	uint64_t compared_ns;
};
static struct rowfix_entry rowfix[ROWFIX_SURFACES];
static unsigned rowfix_live;
static int rowfix_on = -1;
static uint32_t rowfix_rendered[ROWFIX_RENDERED];      /* surface ids seen in attachImage, a ring */
static unsigned rowfix_rendered_next;
static uint64_t rowfix_reuploads[2];                    /* seed changed, bytes changed */

static bool
rowfix_enabled(void)
{
	if (rowfix_on < 0) {
		const char *v = getenv("SWEETEAGL_ROWFIX");
		rowfix_on = !(v && v[0] == '0');
		sweetdrm_say("7.7 rowfix: copy texImageIOSurface surfaces whose rows the writer packs at 32 bytes: %s",
		    rowfix_on ? "on" : "off (SWEETEAGL_ROWFIX=0)");
	}
	return rowfix_on;
}

static void
rowfix_note_rendered(uint32_t sid)
{
	for (unsigned k = 0; k < ROWFIX_RENDERED; k++) {
		if (rowfix_rendered[k] == sid) {
			return;
		}
	}
	rowfix_rendered[rowfix_rendered_next++ % ROWFIX_RENDERED] = sid;
}

static bool
rowfix_is_rendered(uint32_t sid)
{
	for (unsigned k = 0; k < ROWFIX_RENDERED; k++) {
		if (rowfix_rendered[k] == sid) {
			return true;
		}
	}
	return false;
}

static struct rowfix_entry *
rowfix_find(struct sweetgl_context *gl, unsigned tex)
{
	for (unsigned k = 0; k < ROWFIX_SURFACES && rowfix_live; k++) {
		if (rowfix[k].gl == gl && rowfix[k].tex == tex && gl != NULL) {
			return &rowfix[k];
		}
	}
	return NULL;
}

static void
rowfix_drop(struct rowfix_entry *e)
{
	CFRelease(e->surf);
	free(e->copy);
	free(e->fresh);
	*e = (struct rowfix_entry){ 0 };
	rowfix_live--;
}

static GLenum
rowfix_format(uint32_t fourcc, size_t bpe)
{
	if ((fourcc == SWEETGL_FOURCC_BGRA || fourcc == SWEETGL_FOURCC_RGBA) && bpe == 4) {
		return fourcc == SWEETGL_FOURCC_BGRA ? GL_BGRA_EXT : GL_RGBA;
	}
	if (fourcc == SWEETGL_FOURCC_LA08 && bpe == 2) {
		return GL_LUMINANCE_ALPHA;
	}
	if (fourcc == SWEETGL_FOURCC_L008 && bpe == 1) {
		return GL_LUMINANCE;
	}
	if (fourcc == SWEETGL_FOURCC_A008 && bpe == 1) {
		return GL_ALPHA;
	}
	return 0;
}

/* small surfaces can wrap client memory freed while they live (G7j): read, never touch */
static bool
rowfix_read(IOSurfaceRef surf, void *dst, size_t n)
{
	const void *base = IOSurfaceGetBaseAddress(surf);
	vm_size_t got = 0;
	return base != NULL && vm_read_overwrite(mach_task_self(), (vm_address_t)base, n, (vm_address_t)dst, &got) ==
	    KERN_SUCCESS && got == n;
}

/* into the texture bound to GL_TEXTURE_2D, with the unpack state put back */
static GLenum
rowfix_upload(struct rowfix_entry *e, bool sub)
{
	GLint align = 4, rl = 0, sr = 0, sp = 0, unpack = 0;
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rl);
	glGetIntegerv(GL_UNPACK_SKIP_ROWS, &sr);
	glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &sp);
	glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(e->ws / e->bpe));
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	if (sub) {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)e->w, (GLsizei)e->h, e->fmt, GL_UNSIGNED_BYTE, e->copy);
	} else {
		glTexImage2D(GL_TEXTURE_2D, 0, (GLint)e->fmt, (GLsizei)e->w, (GLsizei)e->h, 0, e->fmt, GL_UNSIGNED_BYTE,
		    e->copy);
		/* CA reuses texture objects across formats: an import's L/A swizzle must not stay */
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, align);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, rl);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, sr);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, sp);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpack);
	return glGetError();
}

/* for the first ones: is the stride view's row padding full and the tail past the writer's rows empty? */
static void
rowfix_say_layout(struct rowfix_entry *e, unsigned stride, size_t sw)
{
	size_t all = (size_t)stride * e->h, used = sw * e->bpe, pad = 0, tail = 0;
	uint8_t *b = malloc(all);
	if (b == NULL || !rowfix_read(e->surf, b, all)) {
		free(b);
		return;
	}
	for (unsigned y = 0; y < e->h; y++) {
		for (size_t x = used; x < stride; x++) {
			pad += b[(size_t)y * stride + x] != 0;
		}
	}
	for (size_t i = e->bytes; i < all; i++) {
		tail += b[i] != 0;
	}
	free(b);
	sweetdrm_say("7.7 rowfix: surface %u: nonzero bytes in the row padding at stride %u: %zu of %zu; past %u rows of %u "
	    "bytes: %zu of %zu", e->sid, stride, pad, (stride - used) * e->h, e->h, e->ws, tail, all - e->bytes);
}

/* any import or copy into tex replaces what was there */
static void
rowfix_unbind(struct sweetgl_context *gl, unsigned tex)
{
	struct rowfix_entry *e = rowfix_find(gl, tex);
	if (e) {
		rowfix_drop(e);
	}
}

/* bind_iosurface, texImageIOSurface: true when the texture now holds a copy */
static bool
rowfix_bind(struct sweetgl_context *gl, unsigned tex, IOSurfaceRef surf, uint32_t sid, unsigned w, unsigned h,
    uint32_t fourcc, unsigned stride)
{
	rowfix_unbind(gl, tex);
	size_t bpe = IOSurfaceGetBytesPerElement(surf), sw = IOSurfaceGetWidth(surf);
	GLenum fmt = rowfix_format(fourcc, bpe);
	if (!rowfix_enabled() || fmt == 0 || gl == NULL || tex == 0 || w == 0 || h == 0 || w > sw ||
	    h > IOSurfaceGetHeight(surf) || rowfix_is_rendered(sid)) {
		return false;
	}
	unsigned ws = (unsigned)((sw * bpe + 31) & ~(size_t)31);
	size_t bytes = (size_t)ws * h;
	if (ws >= stride || bytes > ROWFIX_MAX_BYTES) {
		return false;
	}
	struct rowfix_entry *e = NULL;
	for (unsigned k = 0; k < ROWFIX_SURFACES && e == NULL; k++) {
		e = rowfix[k].gl == NULL ? &rowfix[k] : NULL;
	}
	static bool full_said;
	if (e == NULL) {
		if (!full_said) {
			full_said = true;
			sweetdrm_say("7.7 rowfix: all %u slots in use; surface %u is imported at stride %u", ROWFIX_SURFACES, sid,
			    stride);
		}
		return false;
	}
	uint8_t *copy = malloc(bytes), *fresh = bytes <= ROWFIX_COMPARE ? malloc(bytes) : NULL;
	if (copy == NULL || (bytes <= ROWFIX_COMPARE && fresh == NULL)) {
		free(copy);
		free(fresh);
		return false;
	}
	CFRetain(surf);
	*e = (struct rowfix_entry){ gl, tex, surf, sid, IOSurfaceGetSeed(surf), w, h, ws, (unsigned)bpe, fmt, copy, fresh,
	    bytes, sweetdrm_now_ns() };
	rowfix_live++;
	GLenum err = GL_NO_ERROR;
	bool read = rowfix_read(surf, copy, bytes);
	if (read) {
		err = rowfix_upload(e, false);
	}
	static unsigned logged;
	if (logged < ROWFIX_LOG_MAX) {
		logged++;
		sweetdrm_say("7.7 eagl: texImageIOSurface surface %u %ux%u (surface width %zu, bpe %zu) stride %u: copied at "
		    "writer stride %u (G8b) into tex %u: %s, err 0x%04x", sid, w, h, sw, bpe, stride, ws, tex,
		    read ? "ok" : "read FAILED", err);
		if (read && err == GL_NO_ERROR) {
			rowfix_say_layout(e, stride, sw);
		}
	}
	if (!read || err != GL_NO_ERROR) {
		rowfix_drop(e);
		return false;
	}
	return true;
}

/* glBindTexture: copy again what changed since */
static void
rowfix_check(unsigned tex)
{
	struct rowfix_entry *e = rowfix_find(sweetgl_get_current(), tex);
	if (e == NULL) {
		return;
	}
	uint32_t seed = IOSurfaceGetSeed(e->surf);
	unsigned why;
	if (seed != e->seed) {
		why = 0;
		if (!rowfix_read(e->surf, e->copy, e->bytes)) {
			return;
		}
	} else if (e->fresh && sweetdrm_now_ns() - e->compared_ns >= ROWFIX_COMPARE_NS) {
		e->compared_ns = sweetdrm_now_ns();
		if (!rowfix_read(e->surf, e->fresh, e->bytes) || memcmp(e->fresh, e->copy, e->bytes) == 0) {
			return;
		}
		why = 1;
		uint8_t *t = e->copy;
		e->copy = e->fresh;
		e->fresh = t;
	} else {
		return;
	}
	e->seed = seed;
	GLenum before = glGetError();
	GLenum err = rowfix_upload(e, true);
	uint64_t n = ++rowfix_reuploads[why];
	if (n <= 8 || (n & (n - 1)) == 0 || err != GL_NO_ERROR) {
		sweetdrm_say("7.7 rowfix: surface %u tex %u copied again (%s changed): err 0x%04x, before 0x%04x; so far seed "
		    "%llu bytes %llu", e->sid, tex, why ? "bytes" : "seed", err, before,
		    (unsigned long long)rowfix_reuploads[0], (unsigned long long)rowfix_reuploads[1]);
	}
}

/* bind_iosurface: tex now holds surf */
static void
stripe_record(unsigned tex, IOSurfaceRef surf, uint32_t sid, unsigned w, unsigned h)
{
	if (!stripe_enabled() || w > STRIPE_MAX_DIM || h > STRIPE_MAX_DIM || w == 0 || h == 0 ||
	    w > IOSurfaceGetWidth(surf) || h > IOSurfaceGetHeight(surf)) {
		return;
	}
	struct sweetgl_context *gl = sweetgl_get_current();
	struct stripe_entry *e = stripe_find(gl, tex);
	if (e) {
		stripe_drop(e);
	}
	for (unsigned k = 0; k < STRIPE_SURFACES; k++) {
		if (stripe[k].gl == NULL) {
			CFRetain(surf);
			stripe[k] = (struct stripe_entry){ gl, tex, surf, sid, w, h, 0, 0, sweetdrm_now_ns() };
			stripe_live++;
			return;
		}
	}
}

void
sweeteagl_stripe_forget(int n, const unsigned *tex)
{
	if ((stripe_live == 0 && rowfix_live == 0) || tex == NULL) {
		return;
	}
	struct sweetgl_context *gl = sweetgl_get_current();
	for (int i = 0; i < n && rowfix_live; i++) {
		rowfix_unbind(gl, tex[i]);
	}
	for (int i = 0; i < n; i++) {
		struct stripe_entry *e = stripe_find(gl, tex[i]);
		if (e) {
			stripe_drop(e);
		}
	}
}

static bool
row_zero(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i]) {
			return false;
		}
	}
	return true;
}

/* equal as stored, or with R and B swapped (BGRA surfaces read back as RGBA) */
static bool
row_same(const uint8_t *a, const uint8_t *b, unsigned w)
{
	if (memcmp(a, b, (size_t)w * 4) == 0) {
		return true;
	}
	for (unsigned x = 0; x < w; x++) {
		const uint8_t *p = a + x * 4, *q = b + x * 4;
		if (p[0] != q[2] || p[1] != q[1] || p[2] != q[0] || p[3] != q[3]) {
			return false;
		}
	}
	return true;
}

static void
stripe_say_rows(uint32_t sid, const char *what, const char *marks, unsigned h)
{
	for (unsigned y = 0; y < h; y += STRIPE_ROWS_PER_LINE) {
		unsigned n = h - y < STRIPE_ROWS_PER_LINE ? h - y : STRIPE_ROWS_PER_LINE;
		sweetdrm_say("7.7 stripe %u %s r%u: %.*s", sid, what, y, (int)n, marks + y);
	}
}

static void
stripe_compare(struct stripe_entry *e, double age_s)
{
	unsigned w = e->w, h = e->h;
	size_t row = (size_t)w * 4, bpr = IOSurfaceGetBytesPerRow(e->surf);
	const uint8_t *base = IOSurfaceGetBaseAddress(e->surf);
	uint8_t *cpu = calloc(h, row), *gpu = calloc(h, row);
	char *mc = calloc(3, h + 1u);
	if (cpu == NULL || gpu == NULL || mc == NULL || base == NULL) {
		free(cpu);
		free(gpu);
		free(mc);
		return;
	}
	char *mg = mc + h + 1, *md = mg + h + 1;
	/* the CPU's view; a row that cannot be read is '?' */
	for (unsigned y = 0; y < h; y++) {
		vm_size_t got = 0;
		kern_return_t kr = vm_read_overwrite(mach_task_self(), (vm_address_t)(base + (size_t)y * bpr), row,
		    (vm_address_t)(cpu + (size_t)y * row), &got);
		mc[y] = kr != KERN_SUCCESS || got != row ? '?' : row_zero(cpu + (size_t)y * row, row) ? '.' : '#';
	}
	/* the GPU's view, with every piece of state we touch put back */
	GLint old_read = 0, old_pack = 0, align = 4, rl = 0, sr = 0, sp = 0;
	GLenum before = glGetError();
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack);
	glGetIntegerv(GL_PACK_ALIGNMENT, &align);
	glGetIntegerv(GL_PACK_ROW_LENGTH, &rl);
	glGetIntegerv(GL_PACK_SKIP_ROWS, &sr);
	glGetIntegerv(GL_PACK_SKIP_PIXELS, &sp);
	GLuint fb = 0;
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e->tex, 0);
	GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
	bool read = status == GL_FRAMEBUFFER_COMPLETE;
	if (read) {
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		glPixelStorei(GL_PACK_SKIP_ROWS, 0);
		glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
		glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, gpu);
		glPixelStorei(GL_PACK_ALIGNMENT, align);
		glPixelStorei(GL_PACK_ROW_LENGTH, rl);
		glPixelStorei(GL_PACK_SKIP_ROWS, sr);
		glPixelStorei(GL_PACK_SKIP_PIXELS, sp);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)old_pack);
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old_read);
	glDeleteFramebuffers(1, &fb);
	GLenum err = glGetError();
	read = read && err == GL_NO_ERROR;
	unsigned differ = 0, cpu_rows = 0, gpu_rows = 0, first = h;
	for (unsigned y = 0; y < h; y++) {
		const uint8_t *c = cpu + (size_t)y * row, *g = gpu + (size_t)y * row;
		mg[y] = !read ? '!' : row_zero(g, row) ? '.' : '#';
		cpu_rows += mc[y] == '#';
		gpu_rows += mg[y] == '#';
		bool same = read && mc[y] != '?' && row_same(c, g, w);
		md[y] = mc[y] == '?' || !read ? '?' : same ? '=' : 'x';
		if (md[y] == 'x') {
			differ++;
			first = first < y ? first : y;
		}
	}
	sweetdrm_say("7.7 stripe: surface %u %ux%u stride %zu tex %u check %u at %.1f s: %u/%u rows differ; nonzero rows "
	    "cpu %u gpu %u; fb 0x%x err 0x%x before 0x%x", e->sid, w, h, bpr, e->tex, e->checks, age_s, differ, h, cpu_rows,
	    gpu_rows, status, err, before);
	if (first < h) {
		const uint8_t *c = cpu + (size_t)first * row, *g = gpu + (size_t)first * row;
		unsigned x = 0;
		while (x + 1 < w && row_same(c + x * 4, g + x * 4, 1)) {
			x++;
		}
		uint32_t cv, gv;
		memcpy(&cv, c + x * 4, 4);
		memcpy(&gv, g + x * 4, 4);
		sweetdrm_say("7.7 stripe %u: first differing row %u, pixel %u: cpu 0x%08x gpu 0x%08x", e->sid, first, x, cv, gv);
	}
	if (e->checks == 1 || differ) {
		stripe_say_rows(e->sid, "cpu ", mc, h);
		stripe_say_rows(e->sid, "gpu ", mg, h);
		stripe_say_rows(e->sid, "diff", md, h);
	}
	free(cpu);
	free(gpu);
	free(mc);
}

void
sweeteagl_stripe_check(unsigned tex)
{
	if (rowfix_live) {
		rowfix_check(tex);
	}
	if (stripe_live == 0) {
		return;
	}
	struct stripe_entry *e = stripe_find(sweetgl_get_current(), tex);
	if (e == NULL) {
		return;
	}
	if (e->probes < PROBE_PER_SURFACE && probe_total < PROBE_MAX) {
		probe_next = e;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &probe_unit);
	}
	uint64_t age = sweetdrm_now_ns() - e->bound_ns;
	if (e->checks >= STRIPE_CHECKS || age < (e->checks == 0 ? 3000000000ull : 30000000000ull)) {
		return;
	}
	e->checks++;
	stripe_compare(e, (double)age / 1e9);
	if (e->checks >= STRIPE_CHECKS && (e->probes >= PROBE_PER_SURFACE || probe_total >= PROBE_MAX)) {
		stripe_drop(e);
	}
}

/*
 * G7w: the stripe check found the clock hands' surfaces and their textures
 * equal row for row (G7v), so the stripes come from the draw that samples
 * them. For the first draw after such a texture is bound: log its sampler,
 * blend, target and first vertices, then read the target back (when it is
 * small) as a coarse picture. SWEETEAGL_STRIPE=0 turns this off too.
 */
struct probe {
	struct stripe_entry *e;
	GLint fb, vp[4], prog, count;
	bool box;                       /* G7x: lo/hi hold attrib 0's x,y range (raw and divided by w) */
	float lo[2], hi[2];
	bool proj;                      /* G8a: plo/phi hold the window range of vertex_matrix * attrib 0 */
	float plo[2], phi[2];
};

/* G7x: vertex v's index, or -1 when the indices are in a buffer */
static long
probe_index(GLint first, GLenum itype, const void *indices, GLint ebo, unsigned v)
{
	if (itype == 0) {
		return first + (long)v;
	}
	if (ebo || indices == NULL) {
		return -1;
	}
	return itype == GL_UNSIGNED_BYTE ? ((const uint8_t *)indices)[v] :
	    itype == GL_UNSIGNED_SHORT ? ((const uint16_t *)indices)[v] : (long)((const uint32_t *)indices)[v];
}

/* G7x: the screen range of every vertex of the draw (client-side float positions only) */
static void
probe_bbox(struct probe *p, GLint first, GLsizei count, GLenum itype, const void *indices, GLint ebo)
{
	GLint on = 0, size = 0, type = 0, stride = 0, buf = 0;
	glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &on);
	glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
	glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
	glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
	glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
	void *ptr = NULL;
	glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
	p->box = false;
	if (!on || buf || type != GL_FLOAT || ptr == NULL || size < 2) {
		return;
	}
	size_t step = stride ? (size_t)stride : (size_t)size * 4;
	for (unsigned v = 0; v < (unsigned)count && v < 1024; v++) {
		long i = probe_index(first, itype, indices, ebo, v);
		if (i < 0) {
			return;
		}
		const float *q = (const float *)((const uint8_t *)ptr + (size_t)i * step);
		float w = size == 4 && q[3] > 0.01f ? q[3] : 1.0f;
		float xs[2] = { q[0], q[0] / w }, ys[2] = { q[1], q[1] / w };
		for (int k = 0; k < 2; k++) {
			if (!p->box) {
				p->lo[0] = p->hi[0] = xs[k];
				p->lo[1] = p->hi[1] = ys[k];
				p->box = true;
			}
			p->lo[0] = xs[k] < p->lo[0] ? xs[k] : p->lo[0];
			p->hi[0] = xs[k] > p->hi[0] ? xs[k] : p->hi[0];
			p->lo[1] = ys[k] < p->lo[1] ? ys[k] : p->lo[1];
			p->hi[1] = ys[k] > p->hi[1] ? ys[k] : p->hi[1];
		}
	}
}

/* G7x: a program's shader sources, once per program, in pieces that fit a log line (device log only) */
static void
probe_shaders(GLint prog)
{
	static GLint said[8];
	static unsigned nsaid;
	for (unsigned k = 0; k < nsaid; k++) {
		if (said[k] == prog) {
			return;
		}
	}
	if (nsaid >= 8 || prog == 0) {
		return;
	}
	said[nsaid++] = prog;
	GLuint sh[4];
	GLsizei n = 0;
	glGetAttachedShaders((GLuint)prog, 4, &n, sh);
	for (GLsizei s = 0; s < n; s++) {
		GLint type = 0, len = 0;
		glGetShaderiv(sh[s], GL_SHADER_TYPE, &type);
		glGetShaderiv(sh[s], GL_SHADER_SOURCE_LENGTH, &len);
		char *src = len > 0 && len < 65536 ? malloc((size_t)len) : NULL;
		if (src == NULL) {
			sweetdrm_say("7.7 probe prog %d: shader %u type 0x%x source length %d not logged", prog, sh[s], type, len);
			continue;
		}
		glGetShaderSource(sh[s], len, NULL, src);
		const char *what = type == GL_VERTEX_SHADER ? "vs" : type == GL_FRAGMENT_SHADER ? "fs" : "??";
		unsigned line = 0;
		for (char *l = src; *l; line++) {
			char *nl = strchr(l, '\n');
			size_t n2 = nl ? (size_t)(nl - l) : strlen(l);
			for (size_t at = 0; at < n2 || at == 0; at += 150) {
				sweetdrm_say("7.7 probe prog %d %s %03u%s %.*s", prog, what, line, at ? "+" : ":",
				    (int)(n2 - at < 150 ? n2 - at : 150), l + at);
				if (n2 == 0) {
					break;
				}
			}
			l += n2 + (nl != NULL);
		}
		free(src);
	}
}

static void
probe_attribs(struct probe *pr, GLenum mode, GLint first, GLsizei count, GLenum itype, const void *indices)
{
	GLint ebo = 0;
	glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ebo);
	probe_bbox(pr, first, count, itype, indices, ebo);
	unsigned idx[4], nidx = 0;
	for (unsigned v = 0; v < 4 && (GLsizei)v < count; v++) {
		if (indices == NULL && itype == 0) {
			idx[nidx++] = (unsigned)(first + (GLint)v);
		} else if (ebo == 0 && indices) {
			idx[nidx++] = itype == GL_UNSIGNED_BYTE ? ((const uint8_t *)indices)[v] :
			    itype == GL_UNSIGNED_SHORT ? ((const uint16_t *)indices)[v] : ((const uint32_t *)indices)[v];
		}
	}
	sweetdrm_say("7.7 probe: mode 0x%x first %d count %d itype 0x%x ebo %d", mode, first, count, itype, ebo);
	for (GLuint a = 0; a < 4; a++) {
		GLint on = 0, size = 0, type = 0, stride = 0, buf = 0, norm = 0;
		glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &on);
		if (!on) {
			continue;
		}
		glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
		glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
		glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
		glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
		glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &norm);
		void *ptr = NULL;
		glGetVertexAttribPointerv(a, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
		char vals[256] = "";
		size_t at = 0;
		if (buf == 0 && type == GL_FLOAT && ptr && size > 0 && size <= 4) {
			size_t step = stride ? (size_t)stride : (size_t)size * 4;
			for (unsigned v = 0; v < nidx && at < sizeof vals - 48; v++) {
				const float *p = (const float *)((const uint8_t *)ptr + idx[v] * step);
				at += (size_t)snprintf(vals + at, sizeof vals - at, " [");
				for (GLint c = 0; c < size; c++) {
					at += (size_t)snprintf(vals + at, sizeof vals - at, c ? " %.4g" : "%.4g", (double)p[c]);
				}
				at += (size_t)snprintf(vals + at, sizeof vals - at, "]");
			}
		}
		sweetdrm_say("7.7 probe: attrib %u size %d type 0x%x norm %d stride %d vbo %d ptr %p:%s", a, size, type, norm,
		    stride, buf, ptr, buf ? " (in a buffer)" : vals);
	}
}

static float
probe_half(uint16_t h)
{
	int e = (h >> 10) & 0x1f, m = h & 0x3ff;
	float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(m | 0x400), e - 25);
	return (h & 0x8000) ? -v : v;
}

/*
 * G7z: G7y's readback of the clock hands looks like outlines with the middle
 * missing. Log what the mesh should draw: the uniforms its vertex shader uses
 * and every vertex's client-side float or half-float attribs, so the expected
 * picture can be drawn on the host and compared with the readback.
 * G8a: G7z's boxes (raw and /w positions) covered only part of the hand. Also
 * project every vertex as the GPU does (vertex_matrix, then the viewport), so
 * the readback box can be placed on the window range the draw really covers.
 */
#define PROBE_MESH_MAX          96

static void
probe_mesh(struct probe *p, GLint first, GLsizei count, GLenum itype, const void *indices)
{
	static const char *const names[] = { "vertex_matrix", "texmat0", "texmat1", "texmatD", "texmatW" };
	uint32_t sid = p->e->sid;
	GLint prog = p->prog, ebo = 0;
	float m[16];
	bool have_m = false;
	p->proj = false;
	glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ebo);
	for (unsigned n = 0; n < sizeof names / sizeof names[0]; n++) {
		GLint loc = glGetUniformLocation((GLuint)prog, names[n]);
		if (loc < 0) {
			continue;
		}
		float u[16];
		unsigned nu = n == 0 ? 16 : 4;
		glGetUniformfv((GLuint)prog, loc, u);
		if (n == 0) {
			memcpy(m, u, sizeof m);
			have_m = true;
		}
		for (unsigned at = 0; at < nu; at += 8) {
			char vals[128] = "";
			size_t len = 0;
			for (unsigned k = at; k < nu && k < at + 8; k++) {
				len += (size_t)snprintf(vals + len, sizeof vals - len, " %.6g", (double)u[k]);
			}
			sweetdrm_say("7.7 probe %u u %s[%u]:%s", sid, names[n], at, vals);
		}
	}
	for (unsigned v = 0; v < (unsigned)count && v < PROBE_MESH_MAX; v++) {
		long i = probe_index(first, itype, indices, ebo, v);
		if (i < 0) {
			sweetdrm_say("7.7 probe %u: indices in a buffer, vertices not logged", sid);
			return;
		}
		char vals[176] = "";
		size_t len = 0;
		float pos[4] = { 0, 0, 0, 1 };
		bool have_pos = false;
		for (GLuint a = 0; a < 4; a++) {
			GLint on = 0, size = 0, type = 0, stride = 0, buf = 0;
			glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &on);
			glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
			glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
			glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
			glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
			void *ptr = NULL;
			glGetVertexAttribPointerv(a, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
			if (!on || buf || ptr == NULL || size < 1 || size > 4 || (type != GL_FLOAT && type != GL_HALF_FLOAT)) {
				continue;
			}
			size_t es = type == GL_FLOAT ? 4 : 2, step = stride ? (size_t)stride : (size_t)size * es;
			const uint8_t *q = (const uint8_t *)ptr + (size_t)i * step;
			len += (size_t)snprintf(vals + len, sizeof vals - len, " a%u", a);
			for (GLint c = 0; c < size && len < sizeof vals - 12; c++) {
				float f = type == GL_FLOAT ? ((const float *)q)[c] : probe_half(((const uint16_t *)q)[c]);
				len += (size_t)snprintf(vals + len, sizeof vals - len, " %.5g", (double)f);
			}
			for (GLint c = 0; a == 0 && c < size; c++) {
				pos[c] = type == GL_FLOAT ? ((const float *)q)[c] : probe_half(((const uint16_t *)q)[c]);
				have_pos = true;
			}
		}
		sweetdrm_say("7.7 probe %u v%02u:%s", sid, v, vals);
		if (!have_m || !have_pos) {
			continue;
		}
		/* column-major, as glGetUniformfv returns it; window coordinates relative to the viewport */
		float clip[4];
		for (unsigned r = 0; r < 4; r++) {
			clip[r] = m[r] * pos[0] + m[r + 4] * pos[1] + m[r + 8] * pos[2] + m[r + 12] * pos[3];
		}
		if (!(clip[3] > 1e-6f)) {
			continue;
		}
		float wx = (clip[0] / clip[3] + 1) * 0.5f * (float)p->vp[2], wy = (clip[1] / clip[3] + 1) * 0.5f * (float)p->vp[3];
		if (!p->proj) {
			p->plo[0] = p->phi[0] = wx;
			p->plo[1] = p->phi[1] = wy;
			p->proj = true;
		}
		p->plo[0] = wx < p->plo[0] ? wx : p->plo[0];
		p->phi[0] = wx > p->phi[0] ? wx : p->phi[0];
		p->plo[1] = wy < p->plo[1] ? wy : p->plo[1];
		p->phi[1] = wy > p->phi[1] ? wy : p->phi[1];
	}
	if (p->proj) {
		sweetdrm_say("7.7 probe %u: projected window x %.1f..%.1f y %.1f..%.1f (GL, bottom up)", sid, (double)p->plo[0],
		    (double)p->phi[0], (double)p->plo[1], (double)p->phi[1]);
	}
}

void *
sweeteagl_probe_begin(unsigned mode, int first, int count, unsigned itype, const void *indices)
{
	struct stripe_entry *e = probe_next;
	if (e == NULL) {
		return NULL;
	}
	probe_next = NULL;
	if (e->gl != sweetgl_get_current() || probe_total >= PROBE_MAX) {
		return NULL;
	}
	GLint active = 0, bound = 0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glActiveTexture((GLenum)probe_unit);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	GLint minf = 0, magf = 0, ws = 0, wt = 0, base = 0, maxl = 0;
	if ((unsigned)bound == e->tex) {
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minf);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magf);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &ws);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wt);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxl);
	}
	glActiveTexture((GLenum)active);
	if ((unsigned)bound != e->tex) {
		return NULL;    /* rebound before the draw */
	}
	e->probes++;
	probe_total++;
	static struct probe p;
	p.e = e;
	p.count = count;
	p.proj = false;
	GLint prog = 0, sampler = 0, blend = 0, bs = 0, bd = 0, bsa = 0, bda = 0, sc = 0, box[4] = { 0 }, mask[4] = { 0 };
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &p.fb);
	glGetIntegerv(GL_VIEWPORT, p.vp);
	glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
	glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
	blend = glIsEnabled(GL_BLEND);
	glGetIntegerv(GL_BLEND_SRC_RGB, &bs);
	glGetIntegerv(GL_BLEND_DST_RGB, &bd);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsa);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &bda);
	sc = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_SCISSOR_BOX, box);
	glGetIntegerv(GL_COLOR_WRITEMASK, mask);
	GLint atype = 0, aname = 0, samples = 0;
	if (p.fb) {
		glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &atype);
		if (atype) {
			glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &aname);
		}
	}
	glGetIntegerv(GL_SAMPLES, &samples);
	sweetdrm_say("7.7 probe: surface %u %ux%u tex %u unit %d: min 0x%x mag 0x%x wrap 0x%x/0x%x levels %d-%d sampler %d; "
	    "prog %d", e->sid, e->w, e->h, e->tex, probe_unit - GL_TEXTURE0, minf, magf, ws, wt, base, maxl, sampler, prog);
	sweetdrm_say("7.7 probe: target fb %d att 0x%x %d samples %d viewport %d,%d %dx%d scissor %d %d,%d %dx%d blend %d "
	    "0x%x/0x%x 0x%x/0x%x mask %d%d%d%d", p.fb, atype, aname, samples, p.vp[0], p.vp[1], p.vp[2], p.vp[3], sc, box[0],
	    box[1], box[2], box[3], blend, bs, bd, bsa, bda, mask[0], mask[1], mask[2], mask[3]);
	p.prog = prog;
	probe_attribs(&p, mode, first, count, itype, indices);
	if (p.box) {
		sweetdrm_say("7.7 probe %u: positions x %.1f..%.1f y %.1f..%.1f (raw and /w)", e->sid, (double)p.lo[0],
		    (double)p.hi[0], (double)p.lo[1], (double)p.hi[1]);
	}
	if (count > 6) {
		probe_shaders(prog);
		probe_mesh(&p, first, count, itype, indices);
	}
	return &p;
}

/* a cell: transparent '_', else by luminance '@' dark, '+', '-', ' ' near white */
static char
probe_cell(const uint8_t *px)
{
	if (px[3] == 0) {
		return '_';
	}
	unsigned l = (px[0] * 3u + px[1] * 6u + px[2]) / 10u;
	return l < 64 ? '@' : l < 160 ? '+' : l < 240 ? '-' : ' ';
}

/*
 * Read x,y,w,h (GL window coordinates, bottom up) of fb and log it top row
 * first as cw x ch cells, one sample per cell. tag names the picture.
 */
static void
probe_picture(const struct probe *p, const char *tag, GLint x, GLint y, GLint w, GLint h, unsigned cw, unsigned ch)
{
	uint8_t *px = malloc((size_t)w * (size_t)h * 4);
	if (px == NULL) {
		return;
	}
	GLint old_read = 0, old_pack = 0, align = 4, rl = 0, sr = 0, sp = 0;
	GLenum before = glGetError();
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack);
	glGetIntegerv(GL_PACK_ALIGNMENT, &align);
	glGetIntegerv(GL_PACK_ROW_LENGTH, &rl);
	glGetIntegerv(GL_PACK_SKIP_ROWS, &sr);
	glGetIntegerv(GL_PACK_SKIP_PIXELS, &sp);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)p->fb);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_SKIP_ROWS, 0);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glPixelStorei(GL_PACK_ALIGNMENT, align);
	glPixelStorei(GL_PACK_ROW_LENGTH, rl);
	glPixelStorei(GL_PACK_SKIP_ROWS, sr);
	glPixelStorei(GL_PACK_SKIP_PIXELS, sp);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)old_pack);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old_read);
	GLenum err = glGetError();
	sweetdrm_say("7.7 probe %u %s: fb %d box %d,%d %dx%d read err 0x%x before 0x%x; %ux%u cells, top row first",
	    p->e->sid, tag, p->fb, x, y, w, h, err, before, cw, ch);
	if (err == GL_NO_ERROR) {
		char line[PROBE_BOX + 1];
		for (unsigned cy = 0; cy < ch; cy++) {
			unsigned yy = (unsigned)h - 1u - cy * (unsigned)h / ch;
			for (unsigned cx = 0; cx < cw; cx++) {
				unsigned xx = cx * (unsigned)w / cw;
				line[cx] = probe_cell(px + ((size_t)yy * (size_t)w + xx) * 4);
			}
			line[cw] = 0;
			sweetdrm_say("7.7 probe %u %s%02u: |%s|", p->e->sid, tag, cy, line);
		}
	}
	free(px);
}

/* G7x: a box of at most PROBE_BOX pixels around the draw's positions, clipped to the viewport */
static void
probe_box(const struct probe *p, const char *tag, bool flip)
{
	GLint vw = p->vp[2], vh = p->vp[3];
	float cx = (p->lo[0] + p->hi[0]) / 2, cy = (p->lo[1] + p->hi[1]) / 2;
	GLint bw = (GLint)(p->hi[0] - p->lo[0]) + 8, bh = (GLint)(p->hi[1] - p->lo[1]) + 8;
	bw = bw > PROBE_BOX ? PROBE_BOX : bw;
	bh = bh > PROBE_BOX ? PROBE_BOX : bh;
	if (flip) {
		cy = (float)vh - cy;
	}
	GLint x = (GLint)cx - bw / 2, y = (GLint)cy - bh / 2;
	x = x < 0 ? 0 : x + bw > vw ? vw - bw : x;
	y = y < 0 ? 0 : y + bh > vh ? vh - bh : y;
	if (bw <= 0 || bh <= 0 || x < 0 || y < 0) {
		return;
	}
	probe_picture(p, tag, p->vp[0] + x, p->vp[1] + y, bw, bh, (unsigned)bw, (unsigned)bh);
}

/*
 * G8a: the projected window range plus a 2 pixel margin, read in full and,
 * when wider or taller than PROBE_BOX, logged at PROBE_BOX cells.
 */
static void
probe_proj_box(const struct probe *p)
{
	GLint x0 = (GLint)floorf(p->plo[0]) - 2, y0 = (GLint)floorf(p->plo[1]) - 2;
	GLint x1 = (GLint)ceilf(p->phi[0]) + 2, y1 = (GLint)ceilf(p->phi[1]) + 2;
	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > p->vp[2] ? p->vp[2] : x1;
	y1 = y1 > p->vp[3] ? p->vp[3] : y1;
	GLint w = x1 - x0, h = y1 - y0;
	if (w <= 0 || h <= 0 || w > PROBE_MAX_DIM || h > PROBE_MAX_DIM) {
		sweetdrm_say("7.7 probe %u: projected box %d,%d %dx%d not read", p->e->sid, x0, y0, w, h);
		return;
	}
	probe_picture(p, "p", p->vp[0] + x0, p->vp[1] + y0, w, h, w < PROBE_BOX ? (unsigned)w : PROBE_BOX,
	    h < PROBE_BOX ? (unsigned)h : PROBE_BOX);
}

/*
 * G8a: the texels the mesh samples, as RGBA hex (bytes as glReadPixels gives
 * them), bottom row (t = 0) first, PROBE_TEXELS_PER_LINE to a line, so the host
 * can sample the texture as the GPU does. Small textures only.
 */
#define PROBE_TEXELS_MAX        4096u
#define PROBE_TEXELS_PER_LINE   12u

static void
probe_texels(const struct probe *p)
{
	const struct stripe_entry *e = p->e;
	unsigned w = e->w, h = e->h;
	if ((size_t)w * h > PROBE_TEXELS_MAX) {
		sweetdrm_say("7.7 probe %u t: texture %ux%u not dumped (over %u texels)", e->sid, w, h, PROBE_TEXELS_MAX);
		return;
	}
	uint8_t *px = calloc((size_t)w * h, 4);
	if (px == NULL) {
		return;
	}
	GLint old_read = 0, old_pack = 0, align = 4, rl = 0, sr = 0, sp = 0;
	GLenum before = glGetError();
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack);
	glGetIntegerv(GL_PACK_ALIGNMENT, &align);
	glGetIntegerv(GL_PACK_ROW_LENGTH, &rl);
	glGetIntegerv(GL_PACK_SKIP_ROWS, &sr);
	glGetIntegerv(GL_PACK_SKIP_PIXELS, &sp);
	GLuint fb = 0;
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e->tex, 0);
	GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
	if (status == GL_FRAMEBUFFER_COMPLETE) {
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		glPixelStorei(GL_PACK_SKIP_ROWS, 0);
		glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
		glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
		glPixelStorei(GL_PACK_ALIGNMENT, align);
		glPixelStorei(GL_PACK_ROW_LENGTH, rl);
		glPixelStorei(GL_PACK_SKIP_ROWS, sr);
		glPixelStorei(GL_PACK_SKIP_PIXELS, sp);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)old_pack);
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old_read);
	glDeleteFramebuffers(1, &fb);
	GLenum err = glGetError();
	sweetdrm_say("7.7 probe %u t: texture %u %ux%u fb 0x%x err 0x%x before 0x%x; RGBA hex, row 0 (t = 0) first",
	    e->sid, e->tex, w, h, status, err, before);
	if (status == GL_FRAMEBUFFER_COMPLETE && err == GL_NO_ERROR) {
		for (unsigned y = 0; y < h; y++) {
			for (unsigned x = 0; x < w; x += PROBE_TEXELS_PER_LINE) {
				char line[PROBE_TEXELS_PER_LINE * 9 + 1];
				size_t len = 0;
				for (unsigned k = x; k < w && k < x + PROBE_TEXELS_PER_LINE; k++) {
					const uint8_t *q = px + ((size_t)y * w + k) * 4;
					len += (size_t)snprintf(line + len, sizeof line - len, " %02x%02x%02x%02x", q[0], q[1], q[2], q[3]);
				}
				sweetdrm_say("7.7 probe %u t%02u.%02u:%s", e->sid, y, x, line);
			}
		}
	}
	free(px);
}

void
sweeteagl_probe_end(void *token)
{
	struct probe *p = token;
	if (p == NULL) {
		return;
	}
	GLint w = p->vp[2], h = p->vp[3];
	if (p->fb == 0 || w <= 0 || h <= 0) {
		sweetdrm_say("7.7 probe %u: target not read back (fb %d %dx%d)", p->e->sid, p->fb, w, h);
	} else if (w <= PROBE_MAX_DIM && h <= PROBE_MAX_DIM) {
		probe_picture(p, "r", p->vp[0], p->vp[1], w, h, (unsigned)w < PROBE_CELLS ? (unsigned)w : PROBE_CELLS,
		    (unsigned)h < PROBE_CELLS ? (unsigned)h : PROBE_CELLS);
	} else if (p->proj && p->count > 6) {
		/* G8a: the meshes (clock hands) on a full-screen target, where the GPU put them */
		probe_proj_box(p);
		probe_texels(p);
	} else if (p->box && p->count > 6) {
		/* G7x: the meshes (clock hands) on a full-screen target; which way y runs is not known */
		probe_box(p, "b", false);
		probe_box(p, "f", true);
	} else {
		sweetdrm_say("7.7 probe %u: target not read back (fb %d %dx%d)", p->e->sid, p->fb, w, h);
	}
	struct stripe_entry *e = p->e;
	if (e->gl && e->checks >= STRIPE_CHECKS && (e->probes >= PROBE_PER_SURFACE || probe_total >= PROBE_MAX)) {
		stripe_drop(e);
	}
}
/* ---- EAGLContext's methods ---- */

static id
super_init(id self)
{
	struct objc_super sup = { self, class_getSuperclass(eagl) };
	return ((id (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, @selector(init));
}

static id
init_common(id self, NSUInteger api_arg, id sharegroup)
{
	/* EAGLRenderingAPI is an NSUInteger enum; look at the low 32 bits only, as for the GL enums */
	uint32_t api = (uint32_t)api_arg;
	if (api != 2 && api != 3) {
		/* ES 1 (kEAGLRenderingAPIOpenGLES1) has no Mesa path here */
		sweetdrm_say("7.5 eagl: API %lu refused", (unsigned long)api);
		[self release];
		return nil;
	}
	pthread_once(&display_once, open_display);
	if (display == NULL || (self = super_init(self)) == nil) {
		[self release];
		return nil;
	}
	if (sharegroup != nil) {
		/* CA makes no sharegroups (EAGL_NOTES); one would need a share context here */
		sweetdrm_say("7.5 eagl: sharegroup ignored, the context shares nothing");
	}
	struct sweeteagl_priv *p = calloc(1, sizeof(*p));
	if (p == NULL) {
		[self release];
		return nil;
	}
	/* Mesa gives ES 3.2 for either; the API reported is the one asked for */
	SWEETEAGL_ENTER(NULL);
	p->gl = sweetgl_context_create(display, (int)api, 0, NULL);
	if (p->gl == NULL) {
		sweetdrm_say("7.5 eagl: no GLES %lu context", (unsigned long)api);
		free(p);
		[self release];
		return nil;
	}
	p->object = self;
	p->api = api;
	p->gli_ctx = p;
	sweeteagl_fill_table(p->fn);
	*(struct sweeteagl_priv **)((char *)self + private_offset) = p;
	pthread_once(&diag_once, diag_start);
	static unsigned logged;
	if (__atomic_fetch_add(&logged, 1u, __ATOMIC_RELAXED) < CONTEXT_LOG_MAX) {
		sweetdrm_say("7.7 eagl: context %p API %u, footprint %u MB", (void *)self, api, footprint_mb());
	}
	return self;
}

static id
m_initWithAPI(id self, SEL _cmd, NSUInteger api)
{
	(void)_cmd;
	return init_common(self, api, nil);
}

static id
m_initWithAPI_properties(id self, SEL _cmd, NSUInteger api, NSDictionary *properties)
{
	(void)_cmd;
	(void)properties;       /* accelerated, client retain/release: nothing to change */
	return init_common(self, api, nil);
}

static id
m_initWithAPI_sharegroup(id self, SEL _cmd, NSUInteger api, id sharegroup)
{
	(void)_cmd;
	return init_common(self, api, sharegroup);
}

static void
m_dealloc(id self, SEL _cmd)
{
	struct sweeteagl_priv *p = priv_of(self);
	if (p) {
		SWEETEAGL_ENTER(NULL);
		if (current_priv() == p) {
			/* only reached if the client released the context it had current */
			tsd()[TSD_EAGL_SLOT] = NULL;
			sweetgl_make_current(NULL);
		}
		/* Mesa makes it current while it frees, then restores the thread's context */
		sweetgl_context_destroy(p->gl);
		free(p);
		*(void **)((char *)self + private_offset) = NULL;
	}
	struct objc_super sup = { self, class_getSuperclass(eagl) };
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}

static BOOL
m_setCurrentContext(id cls, SEL _cmd, id ctx)
{
	(void)cls;
	(void)_cmd;
	struct sweeteagl_priv *old = current_priv(), *p = ctx ? priv_of(ctx) : NULL;
	if (ctx && p == NULL) {
		return NO;
	}
	if (p == old && sweetgl_get_current() == (p ? p->gl : NULL)) {
		return YES;
	}
	SWEETEAGL_ENTER(NULL);
	if (!sweetgl_make_current(p ? p->gl : NULL)) {
		sweetdrm_say("7.5 eagl: make current failed");
		return NO;
	}
	tsd()[TSD_EAGL_SLOT] = p;
	/* the thread holds a reference to its current context, as with Apple's */
	[ctx retain];
	if (old) {
		[(id)old->object release];
	}
	return YES;
}

static id
m_currentContext(id cls, SEL _cmd)
{
	(void)cls;
	(void)_cmd;
	struct sweeteagl_priv *p = current_priv();
	return p ? (id)p->object : nil;
}

static NSUInteger
m_API(id self, SEL _cmd)
{
	(void)_cmd;
	struct sweeteagl_priv *p = priv_of(self);
	return p ? (NSUInteger)p->api : 0;
}

static void *
m_getMacroContextPrivate(id self, SEL _cmd)
{
	(void)_cmd;
	struct sweeteagl_priv *p = priv_of(self);
	return p ? &p->gli_ctx : NULL;
}

/* CGL-style integer parameters: any pname is accepted; 0x139 (multithreaded) reads back */
static int
m_setParameter_to(id self, SEL _cmd, int pname, const int *value)
{
	(void)_cmd;
	struct sweeteagl_priv *p = priv_of(self);
	if (p && value && pname == PNAME_MULTITHREADED) {
		p->multithreaded = *value;
	}
	return 0;
}

static int
m_getParameter_to(id self, SEL _cmd, int pname, int *value)
{
	(void)_cmd;
	struct sweeteagl_priv *p = priv_of(self);
	if (value) {
		*value = p && pname == PNAME_MULTITHREADED ? p->multithreaded : 0;
	}
	return 0;
}

/*
 * The swizzle that makes the image's R8G8/R8 read as fourcc's L/A (sweetgl.h);
 * identity otherwise, since CA reuses texture objects across formats.
 */
static void
set_swizzle(uint32_t fourcc)
{
	GLint s[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
	if (fourcc == SWEETGL_FOURCC_LA08) {
		s[0] = s[1] = s[2] = GL_RED;
		s[3] = GL_GREEN;
	} else if (fourcc == SWEETGL_FOURCC_L008) {
		s[0] = s[1] = s[2] = GL_RED;
		s[3] = GL_ONE;
	} else if (fourcc == SWEETGL_FOURCC_A008) {
		s[0] = s[1] = s[2] = GL_ZERO;
		s[3] = GL_RED;
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, s[0]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, s[1]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, s[2]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, s[3]);
}

/*
 * Give the texture bound to target on self the surface's pages, width x height
 * pixels of format fourcc from its base address. self is made current for the
 * call if it is not.
 */
static BOOL
bind_iosurface(id self, GLenum target, IOSurfaceRef surf, unsigned width, unsigned height, uint32_t fourcc,
    const char *how)
{
	struct sweeteagl_priv *p = priv_of(self);
	if (p == NULL || surf == NULL || target != GL_TEXTURE_2D) {
		sweetdrm_say("7.5 eagl: %s: target 0x%x or surface %p refused", how, target, (void *)surf);
		return NO;
	}
	/* Mesa's own current context decides, not the TSD slot: CA can set that one alone (G6r) */
	SWEETEAGL_ENTER(NULL);
	uint64_t t0 = sweetdrm_now_ns();
	struct sweetgl_context *old = sweetgl_get_current();
	if (old != p->gl) {
		sweetgl_make_current(p->gl);
	}
	BOOL ok = NO;
	uint32_t sid = IOSurfaceGetID(surf);
	unsigned stride = (unsigned)IOSurfaceGetBytesPerRow(surf);
	/* G8b: a surface whose writer packs its rows tighter than the stride is copied, not imported */
	GLint bound = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	bool copied = false;
	if (strcmp(how, "texImageIOSurface") == 0) {
		GLenum pending = glGetError();
		copied = rowfix_bind(p->gl, (unsigned)bound, surf, sid, width, height, fourcc, stride);
		if (pending != GL_NO_ERROR) {
			sweetdrm_say("7.5 eagl: %s: GL error 0x%04x pending before it (dropped)", how, pending);
		}
	} else {
		rowfix_note_rendered(sid);
		rowfix_unbind(p->gl, (unsigned)bound);
	}
	if (copied) {
		ok = YES;
		use_hold(surf);
		if (width >= 512 && height >= 512) {
			diag_record_surface(surf, sid);
		}
	}
	struct sweetgl_image *img = copied ? NULL : sweetgl_image_from_iosurface(display, sid,
	    IOSurfaceGetBaseAddress(surf), width, height, stride, fourcc);
	if (img) {
		/* an error pending from the client's own calls would be mistaken for ours: log it, drop it */
		GLenum before = glGetError(), e;
		egl_image_target_texture_2d(GL_TEXTURE_2D, (GLeglImageOES)img);
		set_swizzle(fourcc);
		e = glGetError();
		/* the texture now holds the resource */
		sweetgl_image_destroy(img);
		ok = e == GL_NO_ERROR;
		if (ok) {
			use_hold(surf);
			if (fourcc == SWEETGL_FOURCC_BGRA || fourcc == SWEETGL_FOURCC_RGBA) {
				GLint tex = 0;
				glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
				stripe_record((unsigned)tex, surf, sid, width, height);
			}
			/* G7j: backboardd died in the diag thread sampling a small surface later on; those can wrap
			 * client memory (G7a) that is freed while the surface lives. Watch only the display ones. */
			if (width >= 512 && height >= 512) {
				diag_record_surface(surf, sid);
			}
			/* G6w: icon-sized RGBA surfaces bind fine but the icons stay invisible; is there anything in them? */
			static unsigned sampled;
			if ((fourcc == SWEETGL_FOURCC_BGRA || fourcc == SWEETGL_FOURCC_RGBA) && width < 512 && height < 512 &&
			    __atomic_fetch_add(&sampled, 1u, __ATOMIC_RELAXED) < BIND_SAMPLE_MAX) {
				uint32_t centre;
				unsigned nz = diag_sample(surf, &centre);
				sweetdrm_say("7.7 eagl: %s surface %u %ux%u at bind: %u/%u samples nonzero, centre 0x%08x", how, sid,
				    width, height, nz, DIAG_GRID * DIAG_GRID, centre);
			}
			static unsigned traced;
			if (width == 128 && height == 128 && traced < TRACE_BINDS) {
				traced++;
				GLint tex = -1;
				glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
				sweetdrm_say("7.7 trace: %s surface %u -> texture %d; next %u calls follow", how, sid, tex,
				    TRACE_CALLS);
				trace_left = TRACE_CALLS;
			}
		} else {
			sweetdrm_say("7.5 eagl: %s: surface %u: GL error 0x%04x", how, sid, e);
		}
		if (before != GL_NO_ERROR) {
			sweetdrm_say("7.5 eagl: %s: GL error 0x%04x pending before it (dropped)", how, before);
		}
	}
	if (old != p->gl) {
		sweetgl_make_current(old);
	}
	sweetdrm_time_add(SWEETDRM_T_BIND, t0);
	/* 7.7: which surfaces CA hands us, without a line per frame */
	static unsigned logged;
	if (__atomic_fetch_add(&logged, 1u, __ATOMIC_RELAXED) < BIND_LOG_MAX) {
		sweetdrm_say("7.7 eagl: %s surface %u %ux%u stride %u fmt 0x%08x -> %s, footprint %u MB", how, sid, width,
		    height, stride, fourcc, ok ? copied ? "ok, copied" : "ok" : "FAILED", footprint_mb());
	}
	return ok;
}

static BOOL
m_attachImage(id self, SEL _cmd, NSUInteger target, IOSurfaceRef surf, BOOL inverted)
{
	(void)_cmd;
	if (inverted) {
		sweetdrm_say("7.5 eagl: attachImage: invertedRender YES is drawn upright");
	}
	/* CA's own small render targets carry no pixel format (G6t: 24x28, 4 bytes per element) */
	uint32_t fourcc = IOSurfaceGetPixelFormat(surf);
	if (fourcc == 0 && IOSurfaceGetBytesPerElement(surf) == 4) {
		fourcc = SWEETGL_FOURCC_BGRA;
	}
	return bind_iosurface(self, (GLenum)target, surf, (unsigned)IOSurfaceGetWidth(surf),
	    (unsigned)IOSurfaceGetHeight(surf), fourcc, "attachImage");
}

/*
 * The fourcc for texImageIOSurface: the surface's own if it has one, else what
 * the GL format describes at the surface's bytes per element; 0 if neither fits.
 */
static uint32_t
teximage_fourcc(IOSurfaceRef surf, GLenum ifmt, GLenum fmt, GLenum ty)
{
	uint32_t own = IOSurfaceGetPixelFormat(surf);
	size_t bpe = IOSurfaceGetBytesPerElement(surf);
	if (ty != GL_UNSIGNED_BYTE) {
		return 0;
	}
	if ((fmt == GL_BGRA_EXT || fmt == GL_RGBA) && (ifmt == GL_RGBA || ifmt == GL_BGRA_EXT || ifmt == GL_RGBA8)) {
		if (own != 0) {
			return own;
		}
		return bpe == 4 ? (fmt == GL_BGRA_EXT ? SWEETGL_FOURCC_BGRA : SWEETGL_FOURCC_RGBA) : 0;
	}
	if (fmt != ifmt) {
		return 0;
	}
	if (fmt == GL_LUMINANCE_ALPHA && bpe == 2) {
		return SWEETGL_FOURCC_LA08;
	}
	if (fmt == GL_LUMINANCE && bpe == 1) {
		return SWEETGL_FOURCC_L008;
	}
	if (fmt == GL_ALPHA && bpe == 1) {
		return SWEETGL_FOURCC_A008;
	}
	return 0;
}

/*
 * The GL enums arrive as NSUInteger in the public header; a caller may pass
 * 32-bit values, whose register's upper half is then undefined, so only the
 * low 32 bits are looked at.
 */
static BOOL
m_texImageIOSurface(id self, SEL _cmd, IOSurfaceRef surf, NSUInteger target, NSUInteger internal_format,
    uint32_t width, uint32_t height, NSUInteger format, NSUInteger type, uint32_t plane)
{
	(void)_cmd;
	GLenum ifmt = (GLenum)internal_format, fmt = (GLenum)format, ty = (GLenum)type;
	uint32_t fourcc = plane == 0 ? teximage_fourcc(surf, ifmt, fmt, ty) : 0;
	if (fourcc == 0) {
		sweetdrm_say("7.5 eagl: texImageIOSurface: plane %u, formats 0x%x/0x%x/0x%x, %zu bytes per element refused",
		    plane, ifmt, fmt, ty, IOSurfaceGetBytesPerElement(surf));
		return NO;
	}
	return bind_iosurface(self, (GLenum)target, surf, width, height, fourcc, "texImageIOSurface");
}

static BOOL
m_texImageIOSurface_invert(id self, SEL _cmd, IOSurfaceRef surf, NSUInteger target, NSUInteger internal_format,
    uint32_t width, uint32_t height, NSUInteger format, NSUInteger type, uint32_t plane, BOOL invert)
{
	if (invert) {
		sweetdrm_say("7.5 eagl: texImageIOSurface: invert YES is sampled upright");
	}
	return m_texImageIOSurface(self, _cmd, surf, target, internal_format, width, height, format, type, plane);
}

static BOOL
m_renderbufferStorage(id self, SEL _cmd, NSUInteger target, id drawable)
{
	(void)self; (void)_cmd; (void)target; (void)drawable;
	sweetdrm_say("7.5 eagl: renderbufferStorage:fromDrawable: is not supported");
	return NO;
}

static BOOL
m_presentRenderbuffer(id self, SEL _cmd, NSUInteger target)
{
	(void)self; (void)_cmd; (void)target;
	sweetdrm_say("7.5 eagl: presentRenderbuffer: is not supported");
	return NO;
}

/* ---- installing ---- */

static unsigned replaced;

static void
replace(Class cls, const char *name, IMP imp, const char *types)
{
	SEL sel = sel_registerName(name);
	Method m = class_getInstanceMethod(cls, sel);
	/* keep Apple's type encoding where the method exists; add it where it does not */
	if (m) {
		method_setImplementation(m, imp);
	} else {
		class_addMethod(cls, sel, imp, types);
		sweetdrm_say("7.5 eagl: %s%s was not there, added", class_isMetaClass(cls) ? "+" : "-", name);
	}
	++replaced;
}

static pthread_mutex_t install_lock = PTHREAD_MUTEX_INITIALIZER;

/* Returns 0, or -1 if EAGLContext is not (yet) in the process */
SWEETEAGL_EXPORT int
sweeteagl_install(void)
{
	pthread_mutex_lock(&install_lock);
	if (eagl) {
		pthread_mutex_unlock(&install_lock);
		return 0;
	}
	Class cls = objc_getClass("EAGLContext");
	Ivar iv = cls ? class_getInstanceVariable(cls, "_private") : NULL;
	if (cls == NULL || iv == NULL) {
		pthread_mutex_unlock(&install_lock);
		return -1;
	}
	private_offset = ivar_getOffset(iv);
	egl_image_target_texture_2d = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)sweetgl_get_proc_address(
	    "glEGLImageTargetTexture2DOES");
	Class meta = object_getClass(cls);
	replace(cls, "initWithAPI:", (IMP)m_initWithAPI, "@24@0:8Q16");
	replace(cls, "initWithAPI:properties:", (IMP)m_initWithAPI_properties, "@32@0:8Q16@24");
	replace(cls, "initWithAPI:sharegroup:", (IMP)m_initWithAPI_sharegroup, "@32@0:8Q16@24");
	replace(cls, "dealloc", (IMP)m_dealloc, "v16@0:8");
	replace(meta, "setCurrentContext:", (IMP)m_setCurrentContext, "B24@0:8@16");
	replace(meta, "currentContext", (IMP)m_currentContext, "@16@0:8");
	replace(cls, "API", (IMP)m_API, "Q16@0:8");
	replace(cls, "getMacroContextPrivate", (IMP)m_getMacroContextPrivate, "^v16@0:8");
	replace(cls, "setParameter:to:", (IMP)m_setParameter_to, "i28@0:8i16^i20");
	replace(cls, "getParameter:to:", (IMP)m_getParameter_to, "i28@0:8i16^i20");
	replace(cls, "attachImage:toCoreSurface:invertedRender:", (IMP)m_attachImage, "B36@0:8Q16^{__IOSurface=}24B32");
	replace(cls, "texImageIOSurface:target:internalFormat:width:height:format:type:plane:",
	    (IMP)m_texImageIOSurface, "B68@0:8^{__IOSurface=}16Q24Q32I40I44Q48Q56I64");
	replace(cls, "texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:",
	    (IMP)m_texImageIOSurface_invert, "B72@0:8^{__IOSurface=}16Q24Q32I40I44Q48Q56I64B68");
	replace(cls, "renderbufferStorage:fromDrawable:", (IMP)m_renderbufferStorage, "B32@0:8Q16@24");
	replace(cls, "presentRenderbuffer:", (IMP)m_presentRenderbuffer, "B24@0:8Q16");
	eagl = cls;
	pthread_mutex_unlock(&install_lock);
	sweetdrm_say("7.5 eagl: installed on EAGLContext (_private at +%td, %u methods, glEGLImageTargetTexture2DOES %s)",
	    private_offset, replaced, egl_image_target_texture_2d ? "found" : "MISSING");
	return 0;
}

/* Loaded with DYLD_INSERT_LIBRARIES (7.7): OpenGLES is mapped by then if the program links it */
__attribute__((constructor)) static void
sweeteagl_load(void)
{
	sweeteagl_install();
}
