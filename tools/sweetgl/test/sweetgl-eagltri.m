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
 */
#include <dlfcn.h>
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

#include "xf86drm.h"
#include "sweetdrm.h"

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

int sweeteagl_install(void);

#define W               64
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
	/* no GetShaderiv slot in CA's list: ask Mesa directly, as a check only */
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		sweetdrm_say("7.5 %s shader: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
	}
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
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		sweetdrm_say("7.5 link: %s", log);
		CALL(591, void, (void *, GLuint), p);
		return 0;
	}
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
#define ASK             "sweetgleagltri=1"

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

static int
run(void)
{
	void *gles = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW);
	if (gles == NULL || sweeteagl_install() != 0) {
		sweetdrm_say("7.5: OpenGLES %s, shim not installed", gles ? "loaded" : dlerror());
		return 1;
	}
	Class eagl = objc_getClass("EAGLContext");
	NSDictionary *props = [NSDictionary dictionary];
	id ctx = MSG(id, NSUInteger, id)(MSG(id)((id)eagl, @selector(alloc)), @selector(initWithAPI:properties:), 3,
	    props);
	if (ctx == nil) {
		sweetdrm_say("7.5: initWithAPI:3 properties: gave nil");
		return 1;
	}
	unsigned fails = 0;
	int zero = 0;
	NSUInteger api = MSG(NSUInteger)(ctx, @selector(API));
	int setp = MSG(int, int, int *)(ctx, @selector(setParameter:to:), 0x260, &zero);
	BOOL cur = MSG(BOOL, id)((id)eagl, @selector(setCurrentContext:), ctx);
	id back = MSG(id)((id)eagl, @selector(currentContext));
	sweetdrm_say("7.5 context: API %lu, setParameter 0x260 -> %d, setCurrentContext %s, currentContext %s",
	    (unsigned long)api, setp, cur ? "YES" : "NO", back == ctx ? "same" : "DIFFERENT");
	if (api != 3 || !cur || back != ctx) {
		sweetdrm_say("7.5 sweetgl-eagltri: FAIL");
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
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	sweetdrm_say("7.5 display buffer: attachImage %s, fbo status 0x%04x%s", att ? "YES" : "NO", status,
	    status == GL_FRAMEBUFFER_COMPLETE ? " (complete)" : "");
	if (!att || status != GL_FRAMEBUFFER_COMPLETE || gl_errors("display buffer")) {
		sweetdrm_say("7.5 sweetgl-eagltri: FAIL");
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
	sweetdrm_say("7.5 sweetgl-eagltri: %s, footprint %u MB", fails ? "FAIL" : "pass", footprint_mb());
	return fails ? 1 : 0;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!asked_for()) {
		printf("sweetgl-eagltri: no " ASK " in the boot-args, nothing to do\n");
		return 0;
	}
	int fd = -1;
	unsigned waited = 0;
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
	sweetdrm_say("7.5 sweetgl-eagltri: SweetGPU open after %u s", waited);
	raise_memlimit();
	@autoreleasepool {
		return run();
	}
}
