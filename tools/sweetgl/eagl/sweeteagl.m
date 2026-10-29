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
#include <stdlib.h>
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

#define TSD_EAGL_SLOT           30      /* OpenGLES's thread-specific slot for the current priv */
#define PNAME_MULTITHREADED     0x139
#define SWEETEAGL_WAIT_S        300u    /* for SweetGPU; G6o published it ~260 s after launchd */
#define BIND_LOG_MAX            16u     /* IOSurface binds reported */
#define CONTEXT_LOG_MAX         4u      /* contexts reported */

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
	if (p == old) {
		return YES;
	}
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
 * Give the texture bound to target on self the surface's pages, width x height
 * pixels from its base address. self is made current for the call if it is not.
 */
static BOOL
bind_iosurface(id self, GLenum target, IOSurfaceRef surf, unsigned width, unsigned height, const char *how)
{
	struct sweeteagl_priv *p = priv_of(self), *old = current_priv();
	if (p == NULL || surf == NULL || target != GL_TEXTURE_2D) {
		sweetdrm_say("7.5 eagl: %s: target 0x%x or surface %p refused", how, target, (void *)surf);
		return NO;
	}
	if (old != p) {
		sweetgl_make_current(p->gl);
	}
	BOOL ok = NO;
	uint32_t sid = IOSurfaceGetID(surf), fourcc = IOSurfaceGetPixelFormat(surf);
	unsigned stride = (unsigned)IOSurfaceGetBytesPerRow(surf);
	struct sweetgl_image *img = sweetgl_image_from_iosurface(display, sid, IOSurfaceGetBaseAddress(surf), width,
	    height, stride, fourcc);
	if (img) {
		/* an error pending from the client's own calls would be mistaken for ours: log it, drop it */
		GLenum before = glGetError(), e;
		egl_image_target_texture_2d(GL_TEXTURE_2D, (GLeglImageOES)img);
		e = glGetError();
		/* the texture now holds the resource */
		sweetgl_image_destroy(img);
		ok = e == GL_NO_ERROR;
		if (!ok) {
			sweetdrm_say("7.5 eagl: %s: surface %u: GL error 0x%04x", how, sid, e);
		}
		if (before != GL_NO_ERROR) {
			sweetdrm_say("7.5 eagl: %s: GL error 0x%04x pending before it (dropped)", how, before);
		}
	}
	if (old != p) {
		sweetgl_make_current(old ? old->gl : NULL);
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
	return bind_iosurface(self, (GLenum)target, surf, (unsigned)IOSurfaceGetWidth(surf),
	    (unsigned)IOSurfaceGetHeight(surf), "attachImage");
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
	/* the surface's own format decides; the GL ones must describe 32-bit BGRA or RGBA */
	if (plane != 0 || ty != GL_UNSIGNED_BYTE || (fmt != GL_BGRA_EXT && fmt != GL_RGBA) ||
	    (ifmt != GL_RGBA && ifmt != GL_BGRA_EXT && ifmt != GL_RGBA8)) {
		sweetdrm_say("7.5 eagl: texImageIOSurface: plane %u, formats 0x%x/0x%x/0x%x refused", plane, ifmt, fmt, ty);
		return NO;
	}
	return bind_iosurface(self, (GLenum)target, surf, width, height, "texImageIOSurface");
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
