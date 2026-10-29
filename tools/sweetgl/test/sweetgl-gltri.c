/*
 * sweetgl-gltri (ios-sweet plan Stage 7.3): the 7.2 test redone through GL.
 * Through the sweetgl frontend it makes a surfaceless OpenGL ES 3.0 context,
 * renders into a 64x64 RGBA8 renderbuffer FBO, clears it to blue and checks
 * every pixel with glReadPixels, then draws one red triangle with GLSL ES
 * shaders and checks that about half the pixels turned red, on the right side
 * of the diagonal. Each result line also reaches the kernel log (sweetdrm_say).
 *
 * FD_MESA_DEBUG defaults to nogmem,noubwc as in 7.2, and GALLIUM_THREAD to 0
 * (no threaded gallium context, so a GPU fault shows on this thread); set
 * either in the environment to try others.
 * Exit status 0 only if every check passes.
 *
 * Run at boot by a launchd job, as sweetgl-tri is: it does nothing unless the
 * boot-args contain sweetglgltri=1, and then retries the open every 5 s for up
 * to 15 min, since SweetGPU only appears after the kernel's 6.7 hold.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <mach/mach.h>

#include <GLES3/gl3.h>

#include "xf86drm.h"
#include "sweetdrm.h"
#include "sweetgl.h"

#define W               64
#define H               64
#define BLUE            0xffff0000u     /* RGBA8 bytes read as a little-endian word: 0xAABBGGRR */
#define RED             0xff0000ffu

static uint32_t pixels[W * H];

/* every GL error since the last call, logged; returns how many */
static unsigned
gl_errors(const char *where)
{
	unsigned n = 0;
	for (GLenum e; (e = glGetError()) != GL_NO_ERROR && n < 8u; ++n) {
		sweetdrm_say("7.3 %s: GL error 0x%04x", where, e);
	}
	return n;
}

/*
 * Read the FBO back and count its pixels: those equal to want_a in the region
 * where x + y < W - 1 (below the triangle's diagonal; row 0 is the bottom) and
 * want_b elsewhere, reporting the first few that are neither. Returns the bad count.
 */
static unsigned
check(const char *what, uint32_t want_a, uint32_t want_b, unsigned *count_a)
{
	memset(pixels, 0x5a, sizeof(pixels));
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	if (gl_errors(what)) {
		return W * H;
	}
	unsigned bad = 0, a = 0;
	for (unsigned y = 0; y < H; ++y) {
		for (unsigned x = 0; x < W; ++x) {
			uint32_t p = pixels[y * W + x];
			uint32_t want = x + y < W - 1u ? want_a : want_b;
			/* pixels on the diagonal may go either way */
			bool edge = x + y == W - 1u || x + y == W - 2u || x + y == W;
			a += p == want_a;
			if (p == want || (edge && (p == want_a || p == want_b))) {
				continue;
			}
			if (bad++ < 4u) {
				sweetdrm_say("7.3 %s: pixel (%u,%u) 0x%08x, want 0x%08x", what, x, y, p, want);
			}
		}
	}
	sweetdrm_say("7.3 %s: %u of %u pixels wrong, %u are 0x%08x", what, bad, W * H, a, want_a);
	if (count_a) {
		*count_a = a;
	}
	return bad;
}

static GLuint
shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		sweetdrm_say("7.3 %s shader: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
	}
	return s;
}

static const char vs_src[] =
    "#version 300 es\n"
    "layout(location = 0) in vec4 pos;\n"
    "void main() { gl_Position = pos; }\n";
static const char fs_src[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "out vec4 color;\n"
    "void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }\n";

/* returns 0 when the program linked and the draw was issued */
static int
draw_triangle(void)
{
	GLuint vs = shader(GL_VERTEX_SHADER, vs_src), fs = shader(GL_FRAGMENT_SHADER, fs_src);
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		glGetProgramInfoLog(prog, sizeof(log), NULL, log);
		sweetdrm_say("7.3 link: %s", log);
		return 1;
	}
	glUseProgram(prog);

	/* the triangle below the x + y = 0 diagonal in NDC, as in 7.2 */
	static const float verts[3][4] = {
		{ -1, -1, 0, 1 },
		{ 1, -1, 0, 1 },
		{ -1, 1, 0, 1 },
	};
	GLuint vao, vbo;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();

	glDeleteBuffers(1, &vbo);
	glDeleteVertexArrays(1, &vao);
	glDeleteProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);
	return gl_errors("triangle") != 0;
}

/*
 * Jetsam: launchd gives the job the small default daemon limit whatever its
 * JetsamProperties say, and Mesa goes over it (7.2, G6l). Raise our own limit,
 * as root may (bsd/sys/kern_memorystatus.h, private, so declared here).
 */
#define MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES        7
#define MEMORYSTATUS_MEMLIMIT_ATTR_FATAL                0x1
#define MEMLIMIT_MB                                     2048
typedef struct {
	int32_t memlimit_active;        /* MB */
	uint32_t memlimit_active_attr;
	int32_t memlimit_inactive;      /* MB */
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
	sweetdrm_say("7.3 memory: set jetsam limit %d MB -> %d", MEMLIMIT_MB, r);
}

#define WAIT_STEP_S     5u
#define WAIT_MAX_S      900u

/* the boot-args hold " sweetglgltri=1" (or start with it) */
static int
asked_for(void)
{
	char args[1024];
	size_t len = sizeof(args) - 1;
	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return 0;
	}
	args[len] = '\0';
	for (const char *p = args; (p = strstr(p, "sweetglgltri=1")) != NULL; p++) {
		char end = p[strlen("sweetglgltri=1")];
		if ((p == args || p[-1] == ' ') && (end == '\0' || end == ' ')) {
			return 1;
		}
	}
	return 0;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!asked_for()) {
		printf("sweetgl-gltri: no sweetglgltri=1 in the boot-args, nothing to do\n");
		return 0;
	}
	setenv("FD_MESA_DEBUG", "nogmem,noubwc", 0);
	setenv("GALLIUM_THREAD", "0", 0);
	int fd = -1;
	unsigned waited = 0;
	while ((fd = drmOpenWithType("msm", NULL, 0)) < 0 && waited < WAIT_MAX_S) {
		sleep(WAIT_STEP_S);
		waited += WAIT_STEP_S;
	}
	if (fd < 0) {
		fprintf(stderr, "sweetgl-gltri: no sweetdrm device after %u s (%d)\n", waited, fd);
		return 1;
	}
	sweetdrm_say("7.3 sweetgl-gltri: SweetGPU open after %u s", waited);
	raise_memlimit();
	sweetdrm_say("7.3 sweetgl-gltri: FD_MESA_DEBUG=%s GALLIUM_THREAD=%s", getenv("FD_MESA_DEBUG"),
	    getenv("GALLIUM_THREAD"));

	struct sweetgl_display *d = sweetgl_display_open(fd);
	if (d == NULL) {
		sweetdrm_say("7.3: no display");
		return 1;
	}
	sweetdrm_say("7.3: display \"%s\", footprint %u MB", sweetgl_display_name(d), footprint_mb());
	struct sweetgl_context *c = sweetgl_context_create(d, 3, 0, NULL);
	if (c == NULL) {
		sweetdrm_say("7.3: no GLES 3.0 context");
		return 1;
	}
	if (!sweetgl_make_current(c)) {
		sweetdrm_say("7.3: make current failed");
		return 1;
	}
	sweetdrm_say("7.3: GL_VERSION \"%s\", GL_RENDERER \"%s\", GLSL \"%s\", footprint %u MB",
	    (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
	    (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION), footprint_mb());

	unsigned fails = gl_errors("context") != 0;
	GLuint rb, fbo;
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	sweetdrm_say("7.3 fbo: status 0x%04x%s", status, status == GL_FRAMEBUFFER_COMPLETE ? " (complete)" : "");
	if (status != GL_FRAMEBUFFER_COMPLETE || gl_errors("fbo")) {
		sweetdrm_say("7.3 sweetgl-gltri: FAIL");
		return 1;
	}
	glViewport(0, 0, W, H);

	unsigned red = 0;
	glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	fails += check("clear", BLUE, BLUE, NULL) != 0;

	if (draw_triangle() != 0) {
		++fails;
	}
	fails += check("triangle", RED, BLUE, &red) != 0;
	/* half the target, give or take the diagonal */
	if (red < W * H / 2u - W || red > W * H / 2u + W) {
		sweetdrm_say("7.3 triangle: %u red pixels, want about %u", red, W * H / 2u);
		++fails;
	}

	glDeleteFramebuffers(1, &fbo);
	glDeleteRenderbuffers(1, &rb);
	sweetdrm_say("7.3 sweetgl-gltri: %s, footprint %u MB", fails ? "FAIL" : "pass", footprint_mb());
	sweetgl_context_destroy(c);
	sweetgl_display_close(d);
	return fails ? 1 : 0;
}
