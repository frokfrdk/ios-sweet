/*
 * sweeteagl (Stage 7.5): the EAGL macro table. Core Animation calls slot i as
 * fn[i](gli_ctx, args...) and OpenGLES's gl* exports do the same, so each
 * entry here drops the context argument and calls Mesa's entry point (which
 * dispatches to the context current on this thread). The slot numbers are
 * the ones docs/EAGL_NOTES.md lists as used by CA::OGL; every other slot gets
 * a stub that logs its number once.
 */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include "sweeteagl.h"
#include "sweetdrm.h"

#define CTX     void *ctx __attribute__((unused))

#define FAIL_LOG_MAX    8u      /* shader, program and framebuffer failures reported */
#define INFO_LOG_CHARS  240

/*
 * 7.7 diagnostics (G6q drew a black screen with no slot logged): what CA calls.
 * G6r showed every call arriving where the slot's context was not current in
 * Mesa (CA sets the current context without +setCurrentContext:), so every
 * slot now starts with sweeteagl_ensure_current, which counts those as
 * not_current and makes the context current.
 */
struct sweeteagl_diag sweeteagl_diag;

static inline void
count(atomic_uint *c, void *ctx)
{
	(void)ctx;
	atomic_fetch_add_explicit(c, 1u, memory_order_relaxed);
}

/* G7p: TIMED(kind) times the rest of the scope as one call of that kind (sweetdrm_time_report) */
struct timed {
	enum sweetdrm_time_kind kind;
	uint64_t t0;
};

static void
timed_end(struct timed *t)
{
	sweetdrm_time_add(t->kind, t->t0);
}

#define TIMED(k) \
	struct timed timed_ __attribute__((cleanup(timed_end), unused)) = { (k), sweetdrm_now_ns() }

static int
fail_logged(void)
{
	static atomic_uint n;
	return atomic_fetch_add_explicit(&n, 1u, memory_order_relaxed) < FAIL_LOG_MAX;
}

/*
 * G6v: an error (0x0500) was pending before attachImage, from a call nobody
 * checked, and the app icons stay invisible. So the texture and state calls
 * below check glGetError afterwards and log the first failures with their
 * arguments; the first uploads are logged too. A caught error is held for
 * CA's own glGetError.
 */
#define UPLOAD_LOG_MAX  24u
#define ERROR_LOG_MAX   16u

static _Thread_local GLenum held_error;
static atomic_uint logged_errors;

static int
under(atomic_uint *n, unsigned max)
{
	return atomic_fetch_add_explicit(n, 1u, memory_order_relaxed) < max;
}

/* before a checked call: hold an unchecked call's error, so it is not blamed on this one */
static void
drain(const char *call)
{
	GLenum err = glGetError();
	if (err == GL_NO_ERROR) {
		return;
	}
	if (held_error == GL_NO_ERROR) {
		held_error = err;
	}
	if (under(&logged_errors, ERROR_LOG_MAX)) {
		sweetdrm_say("7.7 eagl: GL error 0x%04x pending before %s, from an unchecked call", err, call);
	}
}

static void
checked(const char *call, unsigned a, unsigned b, unsigned c, unsigned d, unsigned e, unsigned f)
{
	GLenum err = glGetError();
	if (err == GL_NO_ERROR) {
		return;
	}
	if (held_error == GL_NO_ERROR) {
		held_error = err;
	}
	if (under(&logged_errors, ERROR_LOG_MAX)) {
		sweetdrm_say("7.7 eagl: %s(0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x) -> GL error 0x%04x", call, a, b, c, d, e, f, err);
	}
}

static void
upload_logged(const char *call, GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLenum f, GLenum ty)
{
	static atomic_uint logged_uploads;
	if (under(&logged_uploads, UPLOAD_LOG_MAX)) {
		sweetdrm_say("7.7 eagl: %s target 0x%x level %d ifmt 0x%x %dx%d format 0x%x type 0x%x", call, t, l, ifmt, w, h,
		    f, ty);
	}
}

static void s_BindTexture(CTX, GLenum t, GLuint n)
{
	SWEETEAGL_ENTER(ctx);
	drain("BindTexture");
	glBindTexture(t, n);
	checked("BindTexture", t, n, 0, 0, 0, 0);
	if (t == GL_TEXTURE_2D && n) {
		sweeteagl_stripe_check(n);
	}
}
static void s_BlendFunc(CTX, GLenum s, GLenum d) { SWEETEAGL_ENTER(ctx); glBlendFunc(s, d); }
static void s_Clear(CTX, GLbitfield m) { SWEETEAGL_ENTER(ctx); count(&sweeteagl_diag.clears, ctx); glClear(m); }
static void s_ClearColor(CTX, GLfloat r, GLfloat g, GLfloat b, GLfloat a) { SWEETEAGL_ENTER(ctx); glClearColor(r, g, b, a); }
static void s_CopyTexSubImage2D(CTX, GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{
	SWEETEAGL_ENTER(ctx);
	TIMED(SWEETDRM_T_UPLOAD);
	drain("CopyTexSubImage2D");
	upload_logged("CopyTexSubImage2D", t, l, 0, w, h, 0, 0);
	glCopyTexSubImage2D(t, l, xo, yo, x, y, w, h);
	checked("CopyTexSubImage2D", t, l, xo, yo, w, h);
}
static void s_DeleteTextures(CTX, GLsizei n, const GLuint *t)
{
	SWEETEAGL_ENTER(ctx);
	sweeteagl_stripe_forget(n, t);
	glDeleteTextures(n, t);
}
static void s_Disable(CTX, GLenum c)
{
	SWEETEAGL_ENTER(ctx);
	drain("Disable");
	glDisable(c);
	checked("Disable", c, 0, 0, 0, 0, 0);
}
/*
 * G7n: with GL_EXT_shader_framebuffer_fetch hidden the small flashing chunks
 * went, so they come from CA's shaders that read gl_LastFragData. Mesa keeps
 * those reads coherent only through the overlap flush of the draw itself, and
 * G7l (GMEM) made it worse. So before every draw whose program reads the
 * framebuffer, glFramebufferFetchBarrierEXT has freedreno wait for idle, flush
 * CCU and flush and invalidate the caches. Shader and program names are small
 * integers; one bit each, under the GL lock every slot holds.
 * SWEETEAGL_FBBARRIER=0 turns the barrier off (the draws are still counted).
 * Mesa's GLES public entry points leave glFramebufferFetchBarrierEXT out, so
 * this calls its implementation (main/barrier.c, on the current context).
 */
void GL_APIENTRY _mesa_FramebufferFetchBarrierEXT(void);

#define FB_NAMES        8192u

static uint64_t fb_shaders[FB_NAMES / 64u], fb_programs[FB_NAMES / 64u];
static unsigned fb_draws, fb_barriers, fb_untracked;

static int
fb_bit(const uint64_t *set, GLuint n)
{
	return n < FB_NAMES && (set[n / 64u] >> (n % 64u)) & 1u;
}

static void
fb_set(uint64_t *set, GLuint n, int on)
{
	if (n >= FB_NAMES) {
		fb_untracked += on;
		return;
	}
	uint64_t bit = 1ull << (n % 64u);
	set[n / 64u] = on ? set[n / 64u] | bit : set[n / 64u] & ~bit;
}

static int
fb_barrier_enabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		const char *e = getenv("SWEETEAGL_FBBARRIER");
		enabled = !(e && e[0] == '0');
		sweetdrm_say("7.7 eagl: framebuffer-fetch barrier before each such draw: %s", enabled ? "on" : "off");
	}
	return enabled;
}

static void
fb_before_draw(void)
{
	GLint p = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &p);
	if (!fb_bit(fb_programs, (GLuint)p)) {
		return;
	}
	if (fb_barrier_enabled()) {
		TIMED(SWEETDRM_T_FBBARRIER);
		_mesa_FramebufferFetchBarrierEXT();
		fb_barriers++;
	}
	if (++fb_draws >= 64u && (fb_draws & (fb_draws - 1u)) == 0) {
		unsigned progs = 0;
		for (unsigned k = 0; k < FB_NAMES / 64u; k++) {
			progs += (unsigned)__builtin_popcountll(fb_programs[k]);
		}
		sweetdrm_say("7.7 eagl: framebuffer-fetch draws: %u, %u barriers; %u programs read the framebuffer, "
		    "%u names untracked", fb_draws, fb_barriers, progs, fb_untracked);
	}
}

static void s_DrawArrays(CTX, GLenum m, GLint f, GLsizei n)
{
	SWEETEAGL_ENTER(ctx);
	TIMED(SWEETDRM_T_DRAW);
	count(&sweeteagl_diag.draws, ctx);
	fb_before_draw();
	glDrawArrays(m, f, n);
}
static void s_DrawElements(CTX, GLenum m, GLsizei n, GLenum t, const void *i)
{
	SWEETEAGL_ENTER(ctx);
	TIMED(SWEETDRM_T_DRAW);
	count(&sweeteagl_diag.draws, ctx);
	fb_before_draw();
	glDrawElements(m, n, t, i);
}
static void s_Enable(CTX, GLenum c)
{
	SWEETEAGL_ENTER(ctx);
	drain("Enable");
	glEnable(c);
	checked("Enable", c, 0, 0, 0, 0, 0);
}
static void s_Finish(CTX)
{
	SWEETEAGL_ENTER(ctx);
	TIMED(SWEETDRM_T_FINISH);
	count(&sweeteagl_diag.finishes, ctx);
	glFinish();
	sweeteagl_after_flush();
}
static void s_Flush(CTX)
{
	SWEETEAGL_ENTER(ctx);
	TIMED(SWEETDRM_T_FLUSH);
	count(&sweeteagl_diag.flushes, ctx);
	glFlush();
	sweeteagl_after_flush();
}
static void s_GenTextures(CTX, GLsizei n, GLuint *t) { SWEETEAGL_ENTER(ctx); glGenTextures(n, t); }
static GLenum s_GetError(CTX)
{
	SWEETEAGL_ENTER(ctx);
	GLenum e = held_error;
	held_error = GL_NO_ERROR;
	if (e == GL_NO_ERROR) {
		e = glGetError();
	}
	if (e != GL_NO_ERROR) {
		atomic_fetch_add_explicit(&sweeteagl_diag.gl_errors, 1u, memory_order_relaxed);
		atomic_store_explicit(&sweeteagl_diag.last_gl_error, e, memory_order_relaxed);
	}
	return e;
}
static void s_GetIntegerv(CTX, GLenum p, GLint *v) { SWEETEAGL_ENTER(ctx); glGetIntegerv(p, v); }
static const GLubyte *s_GetString(CTX, GLenum n) { SWEETEAGL_ENTER(ctx); return glGetString(n); }
static void s_PixelStorei(CTX, GLenum p, GLint v)
{
	SWEETEAGL_ENTER(ctx);
	drain("PixelStorei");
	glPixelStorei(p, v);
	checked("PixelStorei", p, (unsigned)v, 0, 0, 0, 0);
}
static void s_Scissor(CTX, GLint x, GLint y, GLsizei w, GLsizei h) { SWEETEAGL_ENTER(ctx); glScissor(x, y, w, h); }
static void s_TexImage2D(CTX, GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty,
    const void *p)
{
	SWEETEAGL_ENTER(ctx);
	TIMED(SWEETDRM_T_UPLOAD);
	drain("TexImage2D");
	upload_logged("TexImage2D", t, l, ifmt, w, h, f, ty);
	glTexImage2D(t, l, ifmt, w, h, b, f, ty, p);
	checked("TexImage2D", t, (unsigned)ifmt, (unsigned)w, (unsigned)h, f, ty);
}
static void s_TexParameterf(CTX, GLenum t, GLenum p, GLfloat v)
{
	SWEETEAGL_ENTER(ctx);
	drain("TexParameterf");
	glTexParameterf(t, p, v);
	checked("TexParameterf", t, p, (unsigned)(v * 1000.0f), 0, 0, 0);
}
static void s_TexParameteri(CTX, GLenum t, GLenum p, GLint v)
{
	SWEETEAGL_ENTER(ctx);
	drain("TexParameteri");
	glTexParameteri(t, p, v);
	checked("TexParameteri", t, p, (unsigned)v, 0, 0, 0);
}
static void s_TexSubImage2D(CTX, GLenum t, GLint l, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum f, GLenum ty,
    const void *p)
{
	SWEETEAGL_ENTER(ctx);
	TIMED(SWEETDRM_T_UPLOAD);
	drain("TexSubImage2D");
	upload_logged("TexSubImage2D", t, l, 0, w, h, f, ty);
	glTexSubImage2D(t, l, xo, yo, w, h, f, ty, p);
	checked("TexSubImage2D", t, (unsigned)l, (unsigned)w, (unsigned)h, f, ty);
}
static void s_Viewport(CTX, GLint x, GLint y, GLsizei w, GLsizei h) { SWEETEAGL_ENTER(ctx); glViewport(x, y, w, h); }
static void s_BlendFuncSeparate(CTX, GLenum a, GLenum b, GLenum c, GLenum d) { SWEETEAGL_ENTER(ctx); glBlendFuncSeparate(a, b, c, d); }
static void s_BlendEquation(CTX, GLenum m) { SWEETEAGL_ENTER(ctx); glBlendEquation(m); }
static void s_ActiveTexture(CTX, GLenum t) { SWEETEAGL_ENTER(ctx); glActiveTexture(t); }
static void s_CompressedTexImage2D(CTX, GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei n,
    const void *d)
{
	SWEETEAGL_ENTER(ctx);
	TIMED(SWEETDRM_T_UPLOAD);
	drain("CompressedTexImage2D");
	upload_logged("CompressedTexImage2D", t, l, (GLint)ifmt, w, h, 0, (GLenum)n);
	glCompressedTexImage2D(t, l, ifmt, w, h, b, n, d);
	checked("CompressedTexImage2D", t, ifmt, (unsigned)w, (unsigned)h, (unsigned)n, 0);
}
/* GL_APPLE_fence's wait for one object: a full finish does that too */
static void s_FinishObjectAPPLE(CTX, GLenum o, GLint n)
{
	SWEETEAGL_ENTER(ctx);
	TIMED(SWEETDRM_T_FINISH);
	(void)o;
	(void)n;
	count(&sweeteagl_diag.finishes, ctx);
	glFinish();
	sweeteagl_after_flush();
}
static void s_VertexAttribPointer(CTX, GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const void *p)
{
	SWEETEAGL_ENTER(ctx);
	glVertexAttribPointer(i, s, t, n, st, p);
}
static void s_EnableVertexAttribArray(CTX, GLuint i) { SWEETEAGL_ENTER(ctx); glEnableVertexAttribArray(i); }
static void s_DisableVertexAttribArray(CTX, GLuint i) { SWEETEAGL_ENTER(ctx); glDisableVertexAttribArray(i); }
/* both glDeleteShader and glDeleteProgram land here; Mesa's names share one space */
static void s_DeleteShaderOrProgram(CTX, GLuint n)
{
	SWEETEAGL_ENTER(ctx);
	if (glIsProgram(n)) {
		glDeleteProgram(n);
	} else {
		glDeleteShader(n);
	}
}
static GLuint s_CreateShader(CTX, GLenum t) { SWEETEAGL_ENTER(ctx); return glCreateShader(t); }
static void s_ShaderSource(CTX, GLuint s, GLsizei n, const GLchar *const *str, const GLint *len)
{
	SWEETEAGL_ENTER(ctx);
	glShaderSource(s, n, str, len);
	int reads_fb = 0;
	for (GLsizei k = 0; k < n && str && !reads_fb; k++) {
		if (!str[k]) {
			continue;
		}
		size_t l = len && len[k] >= 0 ? (size_t)len[k] : strlen(str[k]);
		reads_fb = memmem(str[k], l, "gl_LastFragData", 15) || memmem(str[k], l, "framebuffer_fetch", 17);
	}
	fb_set(fb_shaders, s, reads_fb);
}
static void s_CompileShader(CTX, GLuint s)
{
	SWEETEAGL_ENTER(ctx);
	glCompileShader(s);
	GLint ok = GL_FALSE;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	atomic_fetch_add_explicit(ok ? &sweeteagl_diag.compiles : &sweeteagl_diag.compile_fails, 1u,
	    memory_order_relaxed);
	if (!ok && fail_logged()) {
		char log[INFO_LOG_CHARS + 1] = "", src[INFO_LOG_CHARS + 1] = "";
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		glGetShaderSource(s, sizeof(src), NULL, src);
		sweetdrm_say("7.7 eagl: shader %u does not compile: %s", s, log);
		sweetdrm_say("7.7 eagl: shader %u source starts: %s", s, src);
	}
}
static GLuint s_CreateProgram(CTX) { SWEETEAGL_ENTER(ctx); return glCreateProgram(); }
static void s_AttachShader(CTX, GLuint p, GLuint s) { SWEETEAGL_ENTER(ctx); glAttachShader(p, s); }
static void s_LinkProgram(CTX, GLuint p)
{
	SWEETEAGL_ENTER(ctx);
	glLinkProgram(p);
	GLint ok = GL_FALSE;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	atomic_fetch_add_explicit(ok ? &sweeteagl_diag.links : &sweeteagl_diag.link_fails, 1u, memory_order_relaxed);
	GLuint sh[8];
	GLsizei nsh = 0;
	int reads_fb = 0;
	glGetAttachedShaders(p, 8, &nsh, sh);
	for (GLsizei k = 0; k < nsh; k++) {
		reads_fb |= fb_bit(fb_shaders, sh[k]);
	}
	fb_set(fb_programs, p, ok && reads_fb);
	if (!ok && fail_logged()) {
		char log[INFO_LOG_CHARS + 1] = "";
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		sweetdrm_say("7.7 eagl: program %u does not link: %s", p, log);
	}
}
static void s_UseProgram(CTX, GLuint p) { SWEETEAGL_ENTER(ctx); count(&sweeteagl_diag.programs, ctx); glUseProgram(p); }
static void s_Uniform1f(CTX, GLint l, GLfloat a) { SWEETEAGL_ENTER(ctx); glUniform1f(l, a); }
static void s_Uniform4f(CTX, GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { SWEETEAGL_ENTER(ctx); glUniform4f(l, a, b, c, d); }
static void s_Uniform1i(CTX, GLint l, GLint a) { SWEETEAGL_ENTER(ctx); glUniform1i(l, a); }
static void s_Uniform3fv(CTX, GLint l, GLsizei n, const GLfloat *v) { SWEETEAGL_ENTER(ctx); glUniform3fv(l, n, v); }
static void s_Uniform4fv(CTX, GLint l, GLsizei n, const GLfloat *v) { SWEETEAGL_ENTER(ctx); glUniform4fv(l, n, v); }
static void s_UniformMatrix3fv(CTX, GLint l, GLsizei n, GLboolean t, const GLfloat *v) { SWEETEAGL_ENTER(ctx); glUniformMatrix3fv(l, n, t, v); }
static void s_UniformMatrix4fv(CTX, GLint l, GLsizei n, GLboolean t, const GLfloat *v) { SWEETEAGL_ENTER(ctx); glUniformMatrix4fv(l, n, t, v); }
static GLint s_GetUniformLocation(CTX, GLuint p, const GLchar *n) { SWEETEAGL_ENTER(ctx); return glGetUniformLocation(p, n); }
static void s_BindAttribLocation(CTX, GLuint p, GLuint i, const GLchar *n) { SWEETEAGL_ENTER(ctx); glBindAttribLocation(p, i, n); }
static void s_BindFramebuffer(CTX, GLenum t, GLuint f) { SWEETEAGL_ENTER(ctx); count(&sweeteagl_diag.fb_binds, ctx); glBindFramebuffer(t, f); }
static void s_DeleteFramebuffers(CTX, GLsizei n, const GLuint *f) { SWEETEAGL_ENTER(ctx); glDeleteFramebuffers(n, f); }
static void s_GenFramebuffers(CTX, GLsizei n, GLuint *f) { SWEETEAGL_ENTER(ctx); glGenFramebuffers(n, f); }
static void s_FramebufferTexture2D(CTX, GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)
{
	SWEETEAGL_ENTER(ctx);
	glFramebufferTexture2D(t, a, tt, tex, l);
	GLenum st = glCheckFramebufferStatus(t);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		atomic_fetch_add_explicit(&sweeteagl_diag.fb_incomplete, 1u, memory_order_relaxed);
		if (fail_logged()) {
			sweetdrm_say("7.7 eagl: framebuffer incomplete 0x%04x after texture %u at 0x%04x", st, tex, a);
		}
	}
}
static void s_GenerateMipmap(CTX, GLenum t) { SWEETEAGL_ENTER(ctx); glGenerateMipmap(t); }
static GLsync s_FenceSync(CTX, GLenum c, GLbitfield f) { SWEETEAGL_ENTER(ctx); return glFenceSync(c, f); }
static void s_DeleteSync(CTX, GLsync s) { SWEETEAGL_ENTER(ctx); glDeleteSync(s); }
static GLenum s_ClientWaitSync(CTX, GLsync s, GLbitfield f, GLuint64 t)
{
	SWEETEAGL_ENTER(ctx);
	TIMED(SWEETDRM_T_FINISH);
	GLenum r = glClientWaitSync(s, f, t);
	if (r == GL_TIMEOUT_EXPIRED || r == GL_WAIT_FAILED) {
		atomic_fetch_add_explicit(&sweeteagl_diag.wait_fails, 1u, memory_order_relaxed);
	}
	return r;
}
/* debug markers: nothing to do */
static void s_InsertEventMarkerEXT(CTX, GLsizei n, const GLchar *m) { SWEETEAGL_ENTER(ctx); (void)n; (void)m; }
static void s_PushGroupMarkerEXT(CTX, GLsizei n, const GLchar *m) { SWEETEAGL_ENTER(ctx); (void)n; (void)m; }
static void s_PopGroupMarkerEXT(CTX) { SWEETEAGL_ENTER(ctx); }

static const struct {
	uint16_t slot;
	void *fn;
} known[] = {
	{ 5, s_BindTexture },           { 7, s_BlendFunc },             { 10, s_Clear },
	{ 12, s_ClearColor },           { 56, s_CopyTexSubImage2D },    { 59, s_DeleteTextures },
	{ 63, s_Disable },              { 65, s_DrawArrays },           { 67, s_DrawElements },
	{ 72, s_Enable },               { 89, s_Finish },               { 90, s_Flush },
	{ 98, s_GenTextures },          { 102, s_GetError },            { 104, s_GetIntegerv },
	{ 117, s_GetString },           { 195, s_PixelStorei },         { 251, s_Scissor },
	{ 301, s_TexImage2D },          { 302, s_TexParameterf },       { 304, s_TexParameteri },
	{ 307, s_TexSubImage2D },       { 335, s_Viewport },            { 336, s_BlendFuncSeparate },
	{ 338, s_BlendEquation },       { 342, s_ActiveTexture },       { 380, s_CompressedTexImage2D },
	{ 470, s_FinishObjectAPPLE },   { 511, s_VertexAttribPointer }, { 512, s_EnableVertexAttribArray },
	{ 513, s_DisableVertexAttribArray },                            { 591, s_DeleteShaderOrProgram },
	{ 594, s_CreateShader },        { 595, s_ShaderSource },        { 596, s_CompileShader },
	{ 597, s_CreateProgram },       { 598, s_AttachShader },        { 599, s_LinkProgram },
	{ 600, s_UseProgram },          { 602, s_Uniform1f },           { 605, s_Uniform4f },
	{ 606, s_Uniform1i },           { 612, s_Uniform3fv },          { 613, s_Uniform4fv },
	{ 619, s_UniformMatrix3fv },    { 620, s_UniformMatrix4fv },    { 625, s_GetUniformLocation },
	{ 630, s_BindAttribLocation },  { 672, s_BindFramebuffer },     { 673, s_DeleteFramebuffers },
	{ 674, s_GenFramebuffers },     { 677, s_FramebufferTexture2D }, { 681, s_GenerateMipmap },
	{ 779, s_FenceSync },           { 781, s_DeleteSync },          { 782, s_ClientWaitSync },
	{ 816, s_InsertEventMarkerEXT }, { 817, s_PushGroupMarkerEXT }, { 818, s_PopGroupMarkerEXT },
	/* 1024 FramebufferParameteriAPPLE stays a stub: GL_APPLE_tile_order_rot is not advertised */
};

static atomic_uint_least64_t logged[(SWEETEAGL_SLOTS + 63u) / 64u];

/* True the first time a slot is seen (one bit per slot, shared by both loggers) */
static int
first_use(unsigned slot)
{
	if (slot >= SWEETEAGL_SLOTS) {
		return 0;
	}
	uint64_t bit = 1ull << (slot % 64u);
	return (atomic_fetch_or(&logged[slot / 64u], bit) & bit) == 0;
}

long
sweeteagl_unknown_slot(unsigned slot)
{
	atomic_fetch_add_explicit(&sweeteagl_diag.unknown_calls, 1u, memory_order_relaxed);
	if (first_use(slot)) {
		sweetdrm_say("7.5 eagl: slot %u called, which the shim does not implement (returns 0)", slot);
	}
	return 0;
}

void
sweeteagl_gen_used(void *ctx, unsigned slot, const char *name)
{
	sweeteagl_ensure_current(ctx);
	atomic_fetch_add_explicit(&sweeteagl_diag.gen_calls, 1u, memory_order_relaxed);
	if (first_use(slot)) {
		sweetdrm_say("7.7 eagl: slot %u %s first used (generated wrapper)", slot, name);
	}
}

void
sweeteagl_fill_table(void *fn[SWEETEAGL_SLOTS])
{
	for (unsigned i = 0; i < SWEETEAGL_SLOTS; i++) {
		fn[i] = (void *)&sweeteagl_unknown_stubs[2u * i];
	}
	for (unsigned k = 0; k < sweeteagl_gen_count; k++) {
		fn[sweeteagl_gen_slots[k].slot] = sweeteagl_gen_slots[k].fn;
	}
	for (unsigned k = 0; k < sizeof(known) / sizeof(known[0]); k++) {
		fn[known[k].slot] = known[k].fn;
	}
}
