/*
 * sweetgl-eagltri (ios-sweet plan Stage 7.5): the EAGL shim, driven the way
 * Core Animation drives it (docs/EAGL_NOTES.md). It loads OpenGLES.framework,
 * installs the shim, makes an EAGLContext with initWithAPI:3 properties:, and
 * from then on calls GL only through the macro table getMacroContextPrivate
 * returns, as CA::OGL::GLESContext does:
 *
 *   1. a 64x64 BGRA IOSurface becomes a texture (attachImage:toCoreSurface:
 *      invertedRender:) and an FBO's colour attachment, as CA's display buffer
 *   2. clear it to blue, glFinish, and check every pixel in the surface's own
 *      memory (no glReadPixels)
 *   3. a red triangle (GLSL ES 1.00, client-side vertex array, colour from a
 *      Uniform4f), waited for with FenceSync/ClientWaitSync; check the halves
 *   4. a second IOSurface filled green by the CPU, bound with texImageIOSurface:
 *      ...plane:0 invert:NO, sampled onto the whole target; check it is green
 *   5. an unimplemented slot returns 0 (and is logged once)
 *
 * Each result line also reaches the kernel log (sweetdrm_say). Exit status 0
 * only if every check passes. Run at boot by a launchd job like the other
 * sweetgl tests: nothing unless the boot-args contain sweetgleagltri=1, then it
 * waits up to 15 min for SweetGPU.
 *
 * Built with -DSWEETEAGL_INSERTED it is sweetgl-eagldy (Stage 7.6), the way
 * backboardd will get the shim in 7.7: linked with OpenGLES.framework and
 * neither Mesa nor the shim, which arrive only as /usr/lib/libsweeteagl.dylib
 * through the kernel's sweetenv= (DYLD_INSERT_LIBRARIES). Before the same
 * checks it confirms the variable, that dyld loaded the dylib, that its
 * constructor already put the shim on EAGLContext, and that sweetaccel=1
 * published an IOAcceleratorES service. Its lines go to the kernel log
 * through its own SweetGPU connection (the dylib has its own). Boot-arg
 * sweetgleagldy=1.
 */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <mach/mach.h>

#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <CoreFoundation/CoreFoundation.h>

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#if defined(SWEETEAGL_INSERTED)
#include <stdarg.h>

/* IOKit.framework, declared as sweetdrm_iokit.c does (no IOKit headers in the iOS SDK) */
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
#define SEL_REPORT      16u             /* SweetGPU's report selector, as sweetdrm_iokit.c */
#define REPORT_MAX      192u
#define SHIM_PATH       "/usr/lib/libsweeteagl.dylib"

static io_connect_t report_conn;

/* sweetdrm_say without sweetdrm: stderr, and the kernel log once SweetGPU is open */
static void
sweetdrm_say(const char *fmt, ...)
{
	char line[REPORT_MAX];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fprintf(stderr, "sweetgl: %s\n", line);
	if (report_conn) {
		IOConnectCallMethod(report_conn, SEL_REPORT, NULL, 0, line, strlen(line), NULL, NULL, NULL, NULL);
	}
}

static bool
have_service(const char *name)
{
	io_service_t s = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching(name));
	if (s) {
		IOObjectRelease(s);
	}
	return s != 0;
}
#else
#include "xf86drm.h"
#include "sweetdrm.h"
#endif

/* The public IOSurface.framework calls (the iPhoneOS SDK has no headers for them) */
typedef struct __IOSurface *IOSurfaceRef;
extern const CFStringRef kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfaceBytesPerRow,
    kIOSurfacePixelFormat;
IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
kern_return_t IOSurfaceLock(IOSurfaceRef buffer, uint32_t options, uint32_t *seed);
kern_return_t IOSurfaceUnlock(IOSurfaceRef buffer, uint32_t options, uint32_t *seed);
void *IOSurfaceGetBaseAddress(IOSurfaceRef buffer);
size_t IOSurfaceGetBytesPerRow(IOSurfaceRef buffer);
#define LOCK_READ_ONLY  1u

#if !defined(SWEETEAGL_INSERTED)
int sweeteagl_install(void);
#endif

#define W              64
#define H               64
#define STRIDE          256u            /* bytes per row: a618 wants linear pitches in 256s */
#define BLUE            0xff0000ffu     /* BGRA bytes read as a little-endian word: 0xAARRGGBB */
#define RED             0xffff0000u
#define GREEN           0xff00ff00u
#define POISON          0x5a5a5a5au

/* the macro table: slot i is called as fn[i](gli_ctx, ...) */
static void **fn;
static void *gc;
#define CALL(slot, ret, params, ...)    (((ret (*) params)fn[slot])(gc, __VA_ARGS__))
#define CALL0(slot, ret)                (((ret (*)(void *))fn[slot])(gc))

static unsigned
gl_errors(const char *where)
{
	unsigned n = 0;
	for (GLenum e; (e = CALL0(102, GLenum)) != GL_NO_ERROR && n < 8u; ++n) {
		sweetdrm_say("7.5 %s: GL error 0x%04x", where, e);
	}
	return n;
}

static IOSurfaceRef
surface(void)
{
	CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	const struct { CFStringRef key; int value; } kv[] = {
		{ kIOSurfaceWidth, W }, { kIOSurfaceHeight, H }, { kIOSurfaceBytesPerElement, 4 },
		{ kIOSurfaceBytesPerRow, (int)STRIDE }, { kIOSurfacePixelFormat, 0x42475241 },  /* 'BGRA' */
	};
	for (unsigned i = 0; i < sizeof(kv) / sizeof(kv[0]); i++) {
		CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &kv[i].value);
		CFDictionarySetValue(props, kv[i].key, n);
		CFRelease(n);
	}
	IOSurfaceRef s = IOSurfaceCreate(props);
	CFRelease(props);
	if (s && IOSurfaceGetBytesPerRow(s) != STRIDE) {
		sweetdrm_say("7.5: surface has %zu bytes per row, not %u", IOSurfaceGetBytesPerRow(s), STRIDE);
		CFRelease(s);
		return NULL;
	}
	return s;
}

static void
fill(IOSurfaceRef s, uint32_t v)
{
	IOSurfaceLock(s, 0, NULL);
	uint32_t *px = IOSurfaceGetBaseAddress(s);
	for (unsigned i = 0; i < H * STRIDE / 4u; i++) {
		px[i] = v;
	}
	IOSurfaceUnlock(s, 0, NULL);
}

/*
 * Count the surface's pixels: want_a where x + y < W - 1 (row 0 is GL's bottom
 * row, and the surface is not inverted), want_b elsewhere; the diagonal may
 * go either way. Returns the bad count.
 */
static unsigned
check(IOSurfaceRef s, const char *what, uint32_t want_a, uint32_t want_b, unsigned *count_a)
{
	IOSurfaceLock(s, LOCK_READ_ONLY, NULL);
	const volatile uint32_t *px = IOSurfaceGetBaseAddress(s);
	unsigned bad = 0, a = 0;
	for (unsigned y = 0; y < H; ++y) {
		for (unsigned x = 0; x < W; ++x) {
			uint32_t p = px[y * (STRIDE / 4u) + x];
			uint32_t want = x + y < W - 1u ? want_a : want_b;
			bool edge = x + y == W - 1u || x + y == W - 2u || x + y == W;
			a += p == want_a;
			if (p == want || (edge && (p == want_a || p == want_b))) {
				continue;
			}
			if (bad++ < 4u) {
				sweetdrm_say("7.5 %s: pixel (%u,%u) 0x%08x, want 0x%08x", what, x, y, p, want);
			}
		}
	}
	IOSurfaceUnlock(s, LOCK_READ_ONLY, NULL);
	sweetdrm_say("7.5 %s: %u of %u pixels wrong, %u are 0x%08x", what, bad, W * H, a, want_a);
	if (count_a) {
		*count_a = a;
	}
	return bad;
}

static GLuint
shader(GLenum type, const char *src)
{
	GLuint s = CALL(594, GLuint, (void *, GLenum), type);
	CALL(595, void, (void *, GLuint, GLsizei, const GLchar *const *, const GLint *), s, 1, &src, NULL);
	CALL(596, void, (void *, GLuint), s);
#if !defined(SWEETEAGL_INSERTED)
	/*
	 * No GetShaderiv slot in CA's list: ask Mesa directly, as a check only. The
	 * inserted build has no Mesa of its own (its gl* are OpenGLES's), and the
	 * pixel checks tell a bad shader anyway.
	 */
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		sweetdrm_say("7.5 %s shader: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
	}
#endif
	return s;
}

static GLuint
program(const char *vs_src, const char *fs_src)
{
	GLuint vs = shader(GL_VERTEX_SHADER, vs_src), fs = shader(GL_FRAGMENT_SHADER, fs_src);
	GLuint p = CALL0(597, GLuint);
	CALL(598, void, (void *, GLuint, GLuint), p, vs);
	CALL(598, void, (void *, GLuint, GLuint), p, fs);
	CALL(630, void, (void *, GLuint, GLuint, const GLchar *), p, 0, "pos");
	CALL(599, void, (void *, GLuint), p);
	/* slot 591 deletes shaders and programs alike; these stay attached until the program goes */
	CALL(591, void, (void *, GLuint), vs);
	CALL(591, void, (void *, GLuint), fs);
#if !defined(SWEETEAGL_INSERTED)
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		sweetdrm_say("7.5 link: %s", log);
		CALL(591, void, (void *, GLuint), p);
		return 0;
	}
#endif
	CALL(600, void, (void *, GLuint), p);
	return p;
}

static const char tri_vs[] =
    "attribute vec4 pos;\n"
    "void main() { gl_Position = pos; }\n";
static const char tri_fs[] =
    "precision mediump float;\n"
    "uniform vec4 color;\n"
    "void main() { gl_FragColor = color; }\n";
static const char tex_vs[] =
    "attribute vec4 pos;\n"
    "varying vec2 uv;\n"
    "void main() { gl_Position = pos; uv = pos.xy * 0.5 + 0.5; }\n";
static const char tex_fs[] =
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "varying vec2 uv;\n"
    "void main() { gl_FragColor = texture2D(tex, uv); }\n";

static const float tri_verts[3][4] = { { -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 } };
static const float quad_verts[4][4] = { { -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 } };

static void
draw(const float (*verts)[4], GLenum mode, GLsizei count)
{
	/* client-side arrays from VAO 0, as an ES 2 renderer would */
	CALL(511, void, (void *, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *), 0, 4, GL_FLOAT, GL_FALSE, 0,
	    verts);
	CALL(512, void, (void *, GLuint), 0);
	CALL(65, void, (void *, GLenum, GLint, GLsizei), mode, 0, count);
	CALL(513, void, (void *, GLuint), 0);
}

static void
texture_params(void)
{
	CALL(304, void, (void *, GLenum, GLenum, GLint), GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	CALL(304, void, (void *, GLenum, GLenum, GLint), GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	CALL(304, void, (void *, GLenum, GLenum, GLint), GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	CALL(304, void, (void *, GLenum, GLenum, GLint), GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* Jetsam: as sweetgl-gltri (7.3); launchd's default daemon limit is too small for Mesa */
#define MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES        7
#define MEMORYSTATUS_MEMLIMIT_ATTR_FATAL                0x1
#define MEMLIMIT_MB                                     2048
typedef struct {
	int32_t memlimit_active;
	uint32_t memlimit_active_attr;
	int32_t memlimit_inactive;
	uint32_t memlimit_inactive_attr;
} memlimit_props;
extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t buffersize);

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
raise_memlimit(void)
{
	memlimit_props p = {
		.memlimit_active = MEMLIMIT_MB, .memlimit_active_attr = MEMORYSTATUS_MEMLIMIT_ATTR_FATAL,
		.memlimit_inactive = MEMLIMIT_MB, .memlimit_inactive_attr = MEMORYSTATUS_MEMLIMIT_ATTR_FATAL,
	};
	int r = memorystatus_control(MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES, getpid(), 0, &p, sizeof(p));
	sweetdrm_say("7.5 memory: set jetsam limit %d MB -> %d", MEMLIMIT_MB, r);
}

#define WAIT_STEP_S     5u
#define WAIT_MAX_S      900u
#if defined(SWEETEAGL_INSERTED)
#define ASK             "sweetgleagldy=1"
#define NAME            "7.6 sweetgl-eagldy"
#else
#define ASK             "sweetgleagltri=1"
#define NAME            "7.5 sweetgl-eagltri"
#endif

static int
asked_for(void)
{
	char args[1024];
	size_t len = sizeof(args) - 1;
	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return 0;
	}
	args[len] = '\0';
	for (const char *p = args; (p = strstr(p, ASK)) != NULL; p++) {
		char end = p[strlen(ASK)];
		if ((p == args || p[-1] == ' ') && (end == '\0' || end == ' ')) {
			return 1;
		}
	}
	return 0;
}

#define MSG(ret, ...)   ((ret (*)(id, SEL, ##__VA_ARGS__))objc_msgSend)

#if defined(SWEETEAGL_INSERTED)
/*
 * 7.6's own checks: the kernel added the variable, dyld honoured it, the
 * dylib's constructor installed the shim before main, and IOAcceleratorES is
 * there. A failure is counted; the GL checks still run if the shim is on.
 */
static unsigned
inserted_checks(void)
{
	unsigned fails = 0;
	const char *env = getenv("DYLD_INSERT_LIBRARIES");
	sweetdrm_say("7.6 env: DYLD_INSERT_LIBRARIES=%s", env ? env : "(not set)");
	fails += env == NULL || strcmp(env, SHIM_PATH) != 0;

	bool loaded = false;
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *n = _dyld_get_image_name(i);
		loaded |= n != NULL && strcmp(n, SHIM_PATH) == 0;
	}
	void *install = dlsym(RTLD_DEFAULT, "sweeteagl_install");
	sweetdrm_say("7.6 dyld: %s %s, sweeteagl_install %s", SHIM_PATH, loaded ? "loaded" : "NOT loaded",
	    install ? "found" : "missing");
	fails += !loaded || install == NULL;

	/* the constructor ran if EAGLContext's initWithAPI: is already the dylib's */
	Class eagl = objc_getClass("EAGLContext");
	Method m = eagl ? class_getInstanceMethod(eagl, @selector(initWithAPI:)) : NULL;
	Dl_info di = { 0 };
	bool ours = m && dladdr((const void *)method_getImplementation(m), &di) && di.dli_fname &&
	    strcmp(di.dli_fname, SHIM_PATH) == 0;
	sweetdrm_say("7.6 shim: EAGLContext %s, initWithAPI: from %s", eagl ? "present" : "MISSING",
	    di.dli_fname ? di.dli_fname : "?");
	if (!ours) {
		++fails;
		if (install) {
			int r = ((int (*)(void))install)();
			sweetdrm_say("7.6 shim: not installed by its constructor; sweeteagl_install() now -> %d", r);
		}
	}

	bool accel = have_service("IOAcceleratorES");
	sweetdrm_say("7.6 accel: IOAcceleratorES %s", accel ? "found" : "NOT found (sweetaccel=1?)");
	fails += !accel;
	return fails;
}
#endif

static int
run(void)
{
	unsigned fails = 0;
#if defined(SWEETEAGL_INSERTED)
	fails += inserted_checks();
#else
	void *gles = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW);
	if (gles == NULL || sweeteagl_install() != 0) {
		sweetdrm_say("7.5: OpenGLES %s, shim not installed", gles ? "loaded" : dlerror());
		return 1;
	}
#endif
	Class eagl = objc_getClass("EAGLContext");
	NSDictionary *props = [NSDictionary dictionary];
	id ctx = MSG(id, NSUInteger, id)(MSG(id)((id)eagl, @selector(alloc)), @selector(initWithAPI:properties:), 3,
	    props);
	if (ctx == nil) {
		sweetdrm_say("7.5: initWithAPI:3 properties: gave nil");
		sweetdrm_say(NAME ": FAIL");
		return 1;
	}
	int zero = 0;
	NSUInteger api = MSG(NSUInteger)(ctx, @selector(API));
	int setp = MSG(int, int, int *)(ctx, @selector(setParameter:to:), 0x260, &zero);
	BOOL cur = MSG(BOOL, id)((id)eagl, @selector(setCurrentContext:), ctx);
	id back = MSG(id)((id)eagl, @selector(currentContext));
	sweetdrm_say("7.5 context: API %lu, setParameter 0x260 -> %d, setCurrentContext %s, currentContext %s",
	    (unsigned long)api, setp, cur ? "YES" : "NO", back == ctx ? "same" : "DIFFERENT");
	if (api != 3 || !cur || back != ctx) {
		sweetdrm_say(NAME ": FAIL");
		return 1;
	}
	void **table = MSG(void *)(ctx, @selector(getMacroContextPrivate));
	gc = table[0];
	fn = table + 1;
	sweetdrm_say("7.5: GL_VERSION \"%s\" through slot 117, footprint %u MB",
	    (const char *)CALL(117, const GLubyte *, (void *, GLenum), GL_VERSION), footprint_mb());

	/* 1. the display buffer, as GLESContext::make_buffer_current builds it */
	IOSurfaceRef target = surface(), source = surface();
	if (target == NULL || source == NULL) {
		sweetdrm_say("7.5: IOSurfaceCreate failed");
		return 1;
	}
	fill(target, POISON);
	GLuint tex[2], fbo;
	CALL(98, void, (void *, GLsizei, GLuint *), 2, tex);
	CALL(5, void, (void *, GLenum, GLuint), GL_TEXTURE_2D, tex[0]);
	texture_params();
	BOOL att = MSG(BOOL, NSUInteger, IOSurfaceRef, BOOL)(ctx, @selector(attachImage:toCoreSurface:invertedRender:),
	    GL_TEXTURE_2D, target, NO);
	CALL(674, void, (void *, GLsizei, GLuint *), 1, &fbo);
	CALL(672, void, (void *, GLenum, GLuint), GL_FRAMEBUFFER, fbo);
	CALL(677, void, (void *, GLenum, GLenum, GLenum, GLuint, GLint), GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_TEXTURE_2D, tex[0], 0);
#if defined(SWEETEAGL_INSERTED)
	/* no slot for it in CA's list and no Mesa here to ask; the clear check below shows it */
	GLenum status = GL_FRAMEBUFFER_COMPLETE;
#else
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
#endif
	sweetdrm_say("7.5 display buffer: attachImage %s, fbo status 0x%04x%s", att ? "YES" : "NO", status,
#if defined(SWEETEAGL_INSERTED)
	    " (not checked)");
#else
	    status == GL_FRAMEBUFFER_COMPLETE ? " (complete)" : "");
#endif
	if (!att || status != GL_FRAMEBUFFER_COMPLETE || gl_errors("display buffer")) {
		sweetdrm_say(NAME ": FAIL");
		return 1;
	}
	CALL(335, void, (void *, GLint, GLint, GLsizei, GLsizei), 0, 0, W, H);

	/* 2. clear */
	CALL(12, void, (void *, GLfloat, GLfloat, GLfloat, GLfloat), 0.0f, 0.0f, 1.0f, 1.0f);
	CALL(10, void, (void *, GLbitfield), GL_COLOR_BUFFER_BIT);
	CALL0(89, void);
	fails += gl_errors("clear") != 0;
	fails += check(target, "clear", BLUE, BLUE, NULL) != 0;

	/* 3. triangle, colour through Uniform4f (float arguments through the table) */
	GLuint p = program(tri_vs, tri_fs);
	if (p == 0) {
		++fails;
	} else {
		GLint loc = CALL(625, GLint, (void *, GLuint, const GLchar *), p, "color");
		CALL(605, void, (void *, GLint, GLfloat, GLfloat, GLfloat, GLfloat), loc, 1.0f, 0.0f, 0.0f, 1.0f);
		draw(tri_verts, GL_TRIANGLES, 3);
		GLsync sync = CALL(779, GLsync, (void *, GLenum, GLbitfield), GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		GLenum w = CALL(782, GLenum, (void *, GLsync, GLbitfield, GLuint64), sync, GL_SYNC_FLUSH_COMMANDS_BIT,
		    2000000000ull);
		CALL(781, void, (void *, GLsync), sync);
		sweetdrm_say("7.5 triangle: ClientWaitSync 0x%04x%s", w,
		    w == GL_ALREADY_SIGNALED || w == GL_CONDITION_SATISFIED ? " (signalled)" : "");
		fails += w != GL_ALREADY_SIGNALED && w != GL_CONDITION_SATISFIED;
		fails += gl_errors("triangle") != 0;
		unsigned red = 0;
		fails += check(target, "triangle", RED, BLUE, &red) != 0;
		if (red < W * H / 2u - W || red > W * H / 2u + W) {
			sweetdrm_say("7.5 triangle: %u red pixels, want about %u", red, W * H / 2u);
			++fails;
		}
		CALL(591, void, (void *, GLuint), p);
	}

	/* 4. an IOSurface texture, as create_surface_from_iosurface binds it, sampled over the target */
	fill(source, GREEN);
	CALL(342, void, (void *, GLenum), GL_TEXTURE0);
	CALL(5, void, (void *, GLenum, GLuint), GL_TEXTURE_2D, tex[1]);
	texture_params();
	BOOL ti = MSG(BOOL, IOSurfaceRef, NSUInteger, NSUInteger, uint32_t, uint32_t, NSUInteger, NSUInteger, uint32_t,
	    BOOL)(ctx, @selector(texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:), source,
	    GL_TEXTURE_2D, GL_RGBA, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, 0, NO);
	sweetdrm_say("7.5 texture: texImageIOSurface %s", ti ? "YES" : "NO");
	p = ti ? program(tex_vs, tex_fs) : 0;
	if (p == 0) {
		++fails;
	} else {
		CALL(606, void, (void *, GLint, GLint), CALL(625, GLint, (void *, GLuint, const GLchar *), p, "tex"), 0);
		draw(quad_verts, GL_TRIANGLE_STRIP, 4);
		/* CA's wait for one texture */
		CALL(470, void, (void *, GLenum, GLint), GL_TEXTURE, (GLint)tex[0]);
		fails += gl_errors("texture") != 0;
		fails += check(target, "texture", GREEN, GREEN, NULL) != 0;
		CALL(591, void, (void *, GLuint), p);
	}

	/* 5. a slot the shim leaves to its stub (1 is not in CA's list) */
	long r = CALL0(1, long);
	sweetdrm_say("7.5 stub: slot 1 returned %ld", r);
	fails += r != 0;

	CALL(673, void, (void *, GLsizei, const GLuint *), 1, &fbo);
	CALL(59, void, (void *, GLsizei, const GLuint *), 2, tex);
	fails += gl_errors("teardown") != 0;
	MSG(BOOL, id)((id)eagl, @selector(setCurrentContext:), nil);
	[ctx release];
	CFRelease(source);
	CFRelease(target);
	sweetdrm_say(NAME ": %s, footprint %u MB", fails ? "FAIL" : "pass", footprint_mb());
	return fails ? 1 : 0;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!asked_for()) {
		printf("%s: no " ASK " in the boot-args, nothing to do\n", getprogname());
		return 0;
	}
	unsigned waited = 0;
#if defined(SWEETEAGL_INSERTED)
	/* SweetGPU's user-client service appears once the GPU service is ready, as IOAcceleratorES does */
	io_service_t svc = 0;
	while ((svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("SweetGPU"))) == 0 &&
	    waited < WAIT_MAX_S) {
		sleep(WAIT_STEP_S);
		waited += WAIT_STEP_S;
	}
	kern_return_t kr = svc ? IOServiceOpen(svc, mach_task_self(), 0, &report_conn) : KERN_FAILURE;
	if (svc) {
		IOObjectRelease(svc);
	}
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "sweetgl-eagldy: no SweetGPU connection after %u s (0x%x)\n", waited, kr);
		return 1;
	}
#else
	int fd = -1;
	while ((fd = drmOpenWithType("msm", NULL, 0)) < 0 && waited < WAIT_MAX_S) {
		sleep(WAIT_STEP_S);
		waited += WAIT_STEP_S;
	}
	if (fd < 0) {
		fprintf(stderr, "sweetgl-eagltri: no sweetdrm device after %u s (%d)\n", waited, fd);
		return 1;
	}
	/* the shim opens its own on the first context */
	close(fd);
#endif
	sweetdrm_say(NAME ": SweetGPU open after %u s", waited);
	raise_memlimit();
	@autoreleasepool {
		return run();
	}
}
