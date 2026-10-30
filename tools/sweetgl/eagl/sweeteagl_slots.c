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

static void s_BindTexture(CTX, GLenum t, GLuint n) { glBindTexture(t, n); }
static void s_BlendFunc(CTX, GLenum s, GLenum d) { glBlendFunc(s, d); }
static void s_Clear(CTX, GLbitfield m) { glClear(m); }
static void s_ClearColor(CTX, GLfloat r, GLfloat g, GLfloat b, GLfloat a) { glClearColor(r, g, b, a); }
static void s_CopyTexSubImage2D(CTX, GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{
	glCopyTexSubImage2D(t, l, xo, yo, x, y, w, h);
}
static void s_DeleteTextures(CTX, GLsizei n, const GLuint *t) { glDeleteTextures(n, t); }
static void s_Disable(CTX, GLenum c) { glDisable(c); }
static void s_DrawArrays(CTX, GLenum m, GLint f, GLsizei n) { glDrawArrays(m, f, n); }
static void s_DrawElements(CTX, GLenum m, GLsizei n, GLenum t, const void *i) { glDrawElements(m, n, t, i); }
static void s_Enable(CTX, GLenum c) { glEnable(c); }
static void s_Finish(CTX) { glFinish(); }
static void s_Flush(CTX) { glFlush(); }
static void s_GenTextures(CTX, GLsizei n, GLuint *t) { glGenTextures(n, t); }
static GLenum s_GetError(CTX) { return glGetError(); }
static void s_GetIntegerv(CTX, GLenum p, GLint *v) { glGetIntegerv(p, v); }
static const GLubyte *s_GetString(CTX, GLenum n) { return glGetString(n); }
static void s_PixelStorei(CTX, GLenum p, GLint v) { glPixelStorei(p, v); }
static void s_Scissor(CTX, GLint x, GLint y, GLsizei w, GLsizei h) { glScissor(x, y, w, h); }
static void s_TexImage2D(CTX, GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty,
    const void *p)
{
	glTexImage2D(t, l, ifmt, w, h, b, f, ty, p);
}
static void s_TexParameterf(CTX, GLenum t, GLenum p, GLfloat v) { glTexParameterf(t, p, v); }
static void s_TexParameteri(CTX, GLenum t, GLenum p, GLint v) { glTexParameteri(t, p, v); }
static void s_TexSubImage2D(CTX, GLenum t, GLint l, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum f, GLenum ty,
    const void *p)
{
	glTexSubImage2D(t, l, xo, yo, w, h, f, ty, p);
}
static void s_Viewport(CTX, GLint x, GLint y, GLsizei w, GLsizei h) { glViewport(x, y, w, h); }
static void s_BlendFuncSeparate(CTX, GLenum a, GLenum b, GLenum c, GLenum d) { glBlendFuncSeparate(a, b, c, d); }
static void s_BlendEquation(CTX, GLenum m) { glBlendEquation(m); }
static void s_ActiveTexture(CTX, GLenum t) { glActiveTexture(t); }
static void s_CompressedTexImage2D(CTX, GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei n,
    const void *d)
{
	glCompressedTexImage2D(t, l, ifmt, w, h, b, n, d);
}
/* GL_APPLE_fence's wait for one object: a full finish does that too */
static void s_FinishObjectAPPLE(CTX, GLenum o, GLint n) { (void)o; (void)n; glFinish(); }
static void s_VertexAttribPointer(CTX, GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const void *p)
{
	glVertexAttribPointer(i, s, t, n, st, p);
}
static void s_EnableVertexAttribArray(CTX, GLuint i) { glEnableVertexAttribArray(i); }
static void s_DisableVertexAttribArray(CTX, GLuint i) { glDisableVertexAttribArray(i); }
/* both glDeleteShader and glDeleteProgram land here; Mesa's names share one space */
static void s_DeleteShaderOrProgram(CTX, GLuint n)
{
	if (glIsProgram(n)) {
		glDeleteProgram(n);
	} else {
		glDeleteShader(n);
	}
}
static GLuint s_CreateShader(CTX, GLenum t) { return glCreateShader(t); }
static void s_ShaderSource(CTX, GLuint s, GLsizei n, const GLchar *const *str, const GLint *len)
{
	glShaderSource(s, n, str, len);
}
static void s_CompileShader(CTX, GLuint s) { glCompileShader(s); }
static GLuint s_CreateProgram(CTX) { return glCreateProgram(); }
static void s_AttachShader(CTX, GLuint p, GLuint s) { glAttachShader(p, s); }
static void s_LinkProgram(CTX, GLuint p) { glLinkProgram(p); }
static void s_UseProgram(CTX, GLuint p) { glUseProgram(p); }
static void s_Uniform1f(CTX, GLint l, GLfloat a) { glUniform1f(l, a); }
static void s_Uniform4f(CTX, GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { glUniform4f(l, a, b, c, d); }
static void s_Uniform1i(CTX, GLint l, GLint a) { glUniform1i(l, a); }
static void s_Uniform3fv(CTX, GLint l, GLsizei n, const GLfloat *v) { glUniform3fv(l, n, v); }
static void s_Uniform4fv(CTX, GLint l, GLsizei n, const GLfloat *v) { glUniform4fv(l, n, v); }
static void s_UniformMatrix3fv(CTX, GLint l, GLsizei n, GLboolean t, const GLfloat *v) { glUniformMatrix3fv(l, n, t, v); }
static void s_UniformMatrix4fv(CTX, GLint l, GLsizei n, GLboolean t, const GLfloat *v) { glUniformMatrix4fv(l, n, t, v); }
static GLint s_GetUniformLocation(CTX, GLuint p, const GLchar *n) { return glGetUniformLocation(p, n); }
static void s_BindAttribLocation(CTX, GLuint p, GLuint i, const GLchar *n) { glBindAttribLocation(p, i, n); }
static void s_BindFramebuffer(CTX, GLenum t, GLuint f) { glBindFramebuffer(t, f); }
static void s_DeleteFramebuffers(CTX, GLsizei n, const GLuint *f) { glDeleteFramebuffers(n, f); }
static void s_GenFramebuffers(CTX, GLsizei n, GLuint *f) { glGenFramebuffers(n, f); }
static void s_FramebufferTexture2D(CTX, GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)
{
	glFramebufferTexture2D(t, a, tt, tex, l);
}
static void s_GenerateMipmap(CTX, GLenum t) { glGenerateMipmap(t); }
static GLsync s_FenceSync(CTX, GLenum c, GLbitfield f) { return glFenceSync(c, f); }
static void s_DeleteSync(CTX, GLsync s) { glDeleteSync(s); }
static GLenum s_ClientWaitSync(CTX, GLsync s, GLbitfield f, GLuint64 t) { return glClientWaitSync(s, f, t); }
/* debug markers: nothing to do */
static void s_InsertEventMarkerEXT(CTX, GLsizei n, const GLchar *m) { (void)n; (void)m; }
static void s_PushGroupMarkerEXT(CTX, GLsizei n, const GLchar *m) { (void)n; (void)m; }
static void s_PopGroupMarkerEXT(CTX) { }

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
	if (first_use(slot)) {
		sweetdrm_say("7.5 eagl: slot %u called, which the shim does not implement (returns 0)", slot);
	}
	return 0;
}

void
sweeteagl_gen_used(unsigned slot, const char *name)
{
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
