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
 * nogmem,noubwc and GALLIUM_THREAD to 0, the configuration 7.3 passed with.
 * In backboardd (7.7) that first context can come before the kernel has
 * published SweetGPU, so the open waits for it, up to SWEETEAGL_WAIT_S.
 */
#include <errno.h>
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

#define TSD_EAGL_SLOT           30      /* OpenGLES's thread-specific slot for the current priv */
#define PNAME_MULTITHREADED     0x139
#define SWEETEAGL_WAIT_S        300u    /* for SweetGPU; G6o published it ~260 s after launchd */
#define BIND_LOG_MAX            16u     /* IOSurface binds reported */
#define CONTEXT_LOG_MAX         4u      /* contexts reported */
#define DIAG_SURFACES           4u      /* bound surfaces whose pixels the diag thread samples */
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

int
sweeteagl_enter(void *p)
{
	if (pthread_mutex_trylock(&gl_lock) != 0) {
		static unsigned logged;
		atomic_fetch_add_explicit(&sweeteagl_diag.contended, 1u, memory_order_relaxed);
		if (__atomic_fetch_add(&logged, 1u, __ATOMIC_RELAXED) < CONTENTION_LOG_MAX) {
			uint64_t tid = 0;
			pthread_threadid_np(NULL, &tid);
			sweetdrm_say("7.7 eagl: thread %llu waits for the GL lock: another thread is in Mesa",
			    (unsigned long long)tid);
		}
		pthread_mutex_lock(&gl_lock);
	}
	sweeteagl_ensure_current(p);
	return 1;
}

void
sweeteagl_leave(int *entered)
{
	(void)entered;
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

static void
open_display(void)
{
	setenv("FD_MESA_DEBUG", "nogmem,noubwc", 0);
	setenv("GALLIUM_THREAD", "0", 0);
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

static void *
diag_thread(void *arg)
{
	(void)arg;
	unsigned prev[16] = { 0 }, prev_nz[DIAG_SURFACES], prev_c[DIAG_SURFACES];
	memset(prev_nz, 0xff, sizeof(prev_nz));
	memset(prev_c, 0xff, sizeof(prev_c));
	for (unsigned r = 1; r <= DIAG_REPORTS; r++) {
		sleep(DIAG_PERIOD_S);
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
	struct sweetgl_context *old = sweetgl_get_current();
	if (old != p->gl) {
		sweetgl_make_current(p->gl);
	}
	BOOL ok = NO;
	uint32_t sid = IOSurfaceGetID(surf);
	unsigned stride = (unsigned)IOSurfaceGetBytesPerRow(surf);
	struct sweetgl_image *img = sweetgl_image_from_iosurface(display, sid, IOSurfaceGetBaseAddress(surf), width,
	    height, stride, fourcc);
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
			diag_record_surface(surf, sid);
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
	/* 7.7: which surfaces CA hands us, without a line per frame */
	static unsigned logged;
	if (__atomic_fetch_add(&logged, 1u, __ATOMIC_RELAXED) < BIND_LOG_MAX) {
		sweetdrm_say("7.7 eagl: %s surface %u %ux%u stride %u fmt 0x%08x -> %s, footprint %u MB", how, sid, width,
		    height, stride, fourcc, ok ? "ok" : "FAILED", footprint_mb());
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
