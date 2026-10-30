/*
 * sweeteagl (Stage 7.5): the EAGL macro table. Core Animation calls slot i as
 * fn[i](gli_ctx, args...) and OpenGLES's gl* exports do the same, so each
 * entry here drops the context argument and calls Mesa's entry point (which
 * dispatches to the context current on this thread). The slot numbers are
 * the ones docs/EAGL_NOTES.md lists as used by CA::OGL; every other slot gets
 * a stub that logs its number once.
 */
#include <stdatomic.h>

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

static int
fail_logged(void)
{
	static atomic_uint n;
	return atomic_fetch_add_explicit(&n, 1u, memory_order_relaxed) < FAIL_LOG_MAX;
}

static void s_BindTexture(CTX, GLenum t, GLuint n) { sweeteagl_ensure_current(ctx); glBindTexture(t, n); }
static void s_BlendFunc(CTX, GLenum s, GLenum d) { sweeteagl_ensure_current(ctx); glBlendFunc(s, d); }
static void s_Clear(CTX, GLbitfield m) { sweeteagl_ensure_current(ctx); count(&sweeteagl_diag.clears, ctx); glClear(m); }
static void s_ClearColor(CTX, GLfloat r, GLfloat g, GLfloat b, GLfloat a) { sweeteagl_ensure_current(ctx); glClearColor(r, g, b, a); }
static void s_CopyTexSubImage2D(CTX, GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{
	sweeteagl_ensure_current(ctx);
	glCopyTexSubImage2D(t, l, xo, yo, x, y, w, h);
}
static void s_DeleteTextures(CTX, GLsizei n, const GLuint *t) { sweeteagl_ensure_current(ctx); glDeleteTextures(n, t); }
static void s_Disable(CTX, GLenum c) { sweeteagl_ensure_current(ctx); glDisable(c); }
static void s_DrawArrays(CTX, GLenum m, GLint f, GLsizei n) { sweeteagl_ensure_current(ctx); count(&sweeteagl_diag.draws, ctx); glDrawArrays(m, f, n); }
static void s_DrawElements(CTX, GLenum m, GLsizei n, GLenum t, const void *i)
{
	sweeteagl_ensure_current(ctx);
	count(&sweeteagl_diag.draws, ctx);
	glDrawElements(m, n, t, i);
}
static void s_Enable(CTX, GLenum c) { sweeteagl_ensure_current(ctx); glEnable(c); }
static void s_Finish(CTX) { sweeteagl_ensure_current(ctx); count(&sweeteagl_diag.finishes, ctx); glFinish(); }
static void s_Flush(CTX) { sweeteagl_ensure_current(ctx); count(&sweeteagl_diag.flushes, ctx); glFlush(); }
static void s_GenTextures(CTX, GLsizei n, GLuint *t) { sweeteagl_ensure_current(ctx); glGenTextures(n, t); }
static GLenum s_GetError(CTX)
{
	sweeteagl_ensure_current(ctx);
	GLenum e = glGetError();
	if (e != GL_NO_ERROR) {
		atomic_fetch_add_explicit(&sweeteagl_diag.gl_errors, 1u, memory_order_relaxed);
		atomic_store_explicit(&sweeteagl_diag.last_gl_error, e, memory_order_relaxed);
	}
	return e;
}
static void s_GetIntegerv(CTX, GLenum p, GLint *v) { sweeteagl_ensure_current(ctx); glGetIntegerv(p, v); }
static const GLubyte *s_GetString(CTX, GLenum n) { sweeteagl_ensure_current(ctx); return glGetString(n); }
static void s_PixelStorei(CTX, GLenum p, GLint v) { sweeteagl_ensure_current(ctx); glPixelStorei(p, v); }
static void s_Scissor(CTX, GLint x, GLint y, GLsizei w, GLsizei h) { sweeteagl_ensure_current(ctx); glScissor(x, y, w, h); }
static void s_TexImage2D(CTX, GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty,
    const void *p)
{
	sweeteagl_ensure_current(ctx);
	glTexImage2D(t, l, ifmt, w, h, b, f, ty, p);
}
static void s_TexParameterf(CTX, GLenum t, GLenum p, GLfloat v) { sweeteagl_ensure_current(ctx); glTexParameterf(t, p, v); }
static void s_TexParameteri(CTX, GLenum t, GLenum p, GLint v) { sweeteagl_ensure_current(ctx); glTexParameteri(t, p, v); }
static void s_TexSubImage2D(CTX, GLenum t, GLint l, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum f, GLenum ty,
    const void *p)
{
	sweeteagl_ensure_current(ctx);
	glTexSubImage2D(t, l, xo, yo, w, h, f, ty, p);
}
static void s_Viewport(CTX, GLint x, GLint y, GLsizei w, GLsizei h) { sweeteagl_ensure_current(ctx); glViewport(x, y, w, h); }
static void s_BlendFuncSeparate(CTX, GLenum a, GLenum b, GLenum c, GLenum d) { sweeteagl_ensure_current(ctx); glBlendFuncSeparate(a, b, c, d); }
static void s_BlendEquation(CTX, GLenum m) { sweeteagl_ensure_current(ctx); glBlendEquation(m); }
static void s_ActiveTexture(CTX, GLenum t) { sweeteagl_ensure_current(ctx); glActiveTexture(t); }
static void s_CompressedTexImage2D(CTX, GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei n,
    const void *d)
{
	sweeteagl_ensure_current(ctx);
	glCompressedTexImage2D(t, l, ifmt, w, h, b, n, d);
}
/* GL_APPLE_fence's wait for one object: a full finish does that too */
static void s_FinishObjectAPPLE(CTX, GLenum o, GLint n)
{
	sweeteagl_ensure_current(ctx);
	(void)o;
	(void)n;
	count(&sweeteagl_diag.finishes, ctx);
	glFinish();
}
static void s_VertexAttribPointer(CTX, GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const void *p)
{
	sweeteagl_ensure_current(ctx);
	glVertexAttribPointer(i, s, t, n, st, p);
}
static void s_EnableVertexAttribArray(CTX, GLuint i) { sweeteagl_ensure_current(ctx); glEnableVertexAttribArray(i); }
static void s_DisableVertexAttribArray(CTX, GLuint i) { sweeteagl_ensure_current(ctx); glDisableVertexAttribArray(i); }
/* both glDeleteShader and glDeleteProgram land here; Mesa's names share one space */
static void s_DeleteShaderOrProgram(CTX, GLuint n)
{
	sweeteagl_ensure_current(ctx);
	if (glIsProgram(n)) {
		glDeleteProgram(n);
	} else {
		glDeleteShader(n);
	}
}
static GLuint s_CreateShader(CTX, GLenum t) { sweeteagl_ensure_current(ctx); return glCreateShader(t); }
static void s_ShaderSource(CTX, GLuint s, GLsizei n, const GLchar *const *str, const GLint *len)
{
	sweeteagl_ensure_current(ctx);
	glShaderSource(s, n, str, len);
}
static void s_CompileShader(CTX, GLuint s)
{
	sweeteagl_ensure_current(ctx);
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
static GLuint s_CreateProgram(CTX) { sweeteagl_ensure_current(ctx); return glCreateProgram(); }
static void s_AttachShader(CTX, GLuint p, GLuint s) { sweeteagl_ensure_current(ctx); glAttachShader(p, s); }
static void s_LinkProgram(CTX, GLuint p)
{
	sweeteagl_ensure_current(ctx);
	glLinkProgram(p);
	GLint ok = GL_FALSE;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	atomic_fetch_add_explicit(ok ? &sweeteagl_diag.links : &sweeteagl_diag.link_fails, 1u, memory_order_relaxed);
	if (!ok && fail_logged()) {
		char log[INFO_LOG_CHARS + 1] = "";
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		sweetdrm_say("7.7 eagl: program %u does not link: %s", p, log);
	}
}
static void s_UseProgram(CTX, GLuint p) { sweeteagl_ensure_current(ctx); count(&sweeteagl_diag.programs, ctx); glUseProgram(p); }
static void s_Uniform1f(CTX, GLint l, GLfloat a) { sweeteagl_ensure_current(ctx); glUniform1f(l, a); }
static void s_Uniform4f(CTX, GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { sweeteagl_ensure_current(ctx); glUniform4f(l, a, b, c, d); }
static void s_Uniform1i(CTX, GLint l, GLint a) { sweeteagl_ensure_current(ctx); glUniform1i(l, a); }
static void s_Uniform3fv(CTX, GLint l, GLsizei n, const GLfloat *v) { sweeteagl_ensure_current(ctx); glUniform3fv(l, n, v); }
static void s_Uniform4fv(CTX, GLint l, GLsizei n, const GLfloat *v) { sweeteagl_ensure_current(ctx); glUniform4fv(l, n, v); }
static void s_UniformMatrix3fv(CTX, GLint l, GLsizei n, GLboolean t, const GLfloat *v) { sweeteagl_ensure_current(ctx); glUniformMatrix3fv(l, n, t, v); }
static void s_UniformMatrix4fv(CTX, GLint l, GLsizei n, GLboolean t, const GLfloat *v) { sweeteagl_ensure_current(ctx); glUniformMatrix4fv(l, n, t, v); }
static GLint s_GetUniformLocation(CTX, GLuint p, const GLchar *n) { sweeteagl_ensure_current(ctx); return glGetUniformLocation(p, n); }
static void s_BindAttribLocation(CTX, GLuint p, GLuint i, const GLchar *n) { sweeteagl_ensure_current(ctx); glBindAttribLocation(p, i, n); }
static void s_BindFramebuffer(CTX, GLenum t, GLuint f) { sweeteagl_ensure_current(ctx); count(&sweeteagl_diag.fb_binds, ctx); glBindFramebuffer(t, f); }
static void s_DeleteFramebuffers(CTX, GLsizei n, const GLuint *f) { sweeteagl_ensure_current(ctx); glDeleteFramebuffers(n, f); }
static void s_GenFramebuffers(CTX, GLsizei n, GLuint *f) { sweeteagl_ensure_current(ctx); glGenFramebuffers(n, f); }
static void s_FramebufferTexture2D(CTX, GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)
{
	sweeteagl_ensure_current(ctx);
	glFramebufferTexture2D(t, a, tt, tex, l);
	GLenum st = glCheckFramebufferStatus(t);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		atomic_fetch_add_explicit(&sweeteagl_diag.fb_incomplete, 1u, memory_order_relaxed);
		if (fail_logged()) {
			sweetdrm_say("7.7 eagl: framebuffer incomplete 0x%04x after texture %u at 0x%04x", st, tex, a);
		}
	}
}
static void s_GenerateMipmap(CTX, GLenum t) { sweeteagl_ensure_current(ctx); glGenerateMipmap(t); }
static GLsync s_FenceSync(CTX, GLenum c, GLbitfield f) { sweeteagl_ensure_current(ctx); return glFenceSync(c, f); }
static void s_DeleteSync(CTX, GLsync s) { sweeteagl_ensure_current(ctx); glDeleteSync(s); }
static GLenum s_ClientWaitSync(CTX, GLsync s, GLbitfield f, GLuint64 t)
{
	sweeteagl_ensure_current(ctx);
	GLenum r = glClientWaitSync(s, f, t);
	if (r == GL_TIMEOUT_EXPIRED || r == GL_WAIT_FAILED) {
		atomic_fetch_add_explicit(&sweeteagl_diag.wait_fails, 1u, memory_order_relaxed);
	}
	return r;
}
/* debug markers: nothing to do */
static void s_InsertEventMarkerEXT(CTX, GLsizei n, const GLchar *m) { sweeteagl_ensure_current(ctx); (void)n; (void)m; }
static void s_PushGroupMarkerEXT(CTX, GLsizei n, const GLchar *m) { sweeteagl_ensure_current(ctx); (void)n; (void)m; }
static void s_PopGroupMarkerEXT(CTX) { sweeteagl_ensure_current(ctx); }

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
