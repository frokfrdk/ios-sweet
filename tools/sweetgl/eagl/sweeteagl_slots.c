/*
 * sweeteagl (Stage 7.5): the EAGL macro table. Core Animation calls slot i as
 * fn[i](gli_ctx, args...) and OpenGLES's gl* exports do the same, so each
 * entry here drops the context argument and calls Mesa's entry point (which
 * dispatches to the context current on this thread). The slot numbers are
 * the ones docs/EAGL_NOTES.md lists as used by CA::OGL; every other slot gets
 * a stub that logs its number once.
 */
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES3/gl32.h>
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

/*
 * G8c: G8b still had a GL_INVALID_ENUM from an unchecked call, and the App
 * Library's title band draws black. Every outermost slot or shim method now
 * ends with glGetError (sweeteagl_leave); an error is held for CA and logged
 * the first time per function and error, then at powers of two.
 * SWEETEAGL_ERRBLAME=0 turns this off.
 */
#define BLAME_NAMES     48
#define BLAME_LOG_MAX   96u

void
sweeteagl_blame_error(const char *fn)
{
	static int on = -1;
	static struct {
		const char *fn;
		GLenum err;
		unsigned n;
	} seen[BLAME_NAMES];
	static unsigned logged;
	if (on < 0) {
		const char *v = getenv("SWEETEAGL_ERRBLAME");
		on = !(v && v[0] == '0');
		sweetdrm_say("7.7 eagl: GL errors named by the call that raised them: %s", on ? "on" : "off (SWEETEAGL_ERRBLAME=0)");
	}
	if (!on || strcmp(fn, "s_GetError") == 0) {
		return;
	}
	GLenum err = glGetError();
	if (err == GL_NO_ERROR) {
		return;
	}
	if (held_error == GL_NO_ERROR) {
		held_error = err;
	}
	unsigned k = 0;
	while (k < BLAME_NAMES && seen[k].fn != NULL && !(seen[k].err == err && strcmp(seen[k].fn, fn) == 0)) {
		k++;
	}
	unsigned n = 1;
	if (k < BLAME_NAMES) {
		if (seen[k].fn == NULL) {
			seen[k].fn = fn;
			seen[k].err = err;
		}
		n = ++seen[k].n;
	}
	if ((n & (n - 1)) == 0 && logged < BLAME_LOG_MAX) {
		logged++;
		sweetdrm_say("7.7 eagl: GL error 0x%04x raised in %s (G8c), %u times", err, fn, n);
	}
}

/*
 * G8c: where do the black band and the missing glyphs come from? When CA
 * leaves a framebuffer it drew into, read its viewport back (at most every
 * OFFSCREEN_EVERY_NS per texture and size) and log how much of it is
 * transparent, opaque black, dark or other, with a 16x16 picture of the
 * majority class per cell (top row first). SWEETEAGL_OFFSCREEN=0 turns it off.
 */
#define OFFSCREEN_KEYS          96
#define OFFSCREEN_EVERY_NS      15000000000ull
#define OFFSCREEN_LOG_MAX       1000u   /* G8e: 400 ran out before the App Library opened */
#define OFFSCREEN_MAX_PIXELS    (2u << 20)
#define OFFSCREEN_GRID          16

static unsigned os_draws;               /* draws into the bound draw framebuffer since it was bound */
static GLuint cur_prog, fb_prog;        /* the program in use; the one of the last draw */
static void *use_ctx;                   /* G8f: the slot context of the last glUseProgram */
static struct sweetgl_context *use_gl;  /* and the Mesa context it ran on */

static void
offscreen_read(GLint fbo, GLint tex, const GLint vp[4], unsigned draws)
{
	static uint64_t t_start;
	uint64_t now = sweetdrm_now_ns();
	if (t_start == 0) {
		t_start = now;
	}
	size_t w = (size_t)vp[2], h = (size_t)vp[3];
	uint8_t *px = w * h <= OFFSCREEN_MAX_PIXELS ? malloc(w * h * 4) : NULL;
	if (px == NULL) {
		sweetdrm_say("7.7 offscreen fb %d tex %d viewport %zux%zu: not read", fbo, tex, w, h);
		return;
	}
	GLint old_read = 0, old_pack = 0, align = 4, rl = 0, sr = 0, sp = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack);
	glGetIntegerv(GL_PACK_ALIGNMENT, &align);
	glGetIntegerv(GL_PACK_ROW_LENGTH, &rl);
	glGetIntegerv(GL_PACK_SKIP_ROWS, &sr);
	glGetIntegerv(GL_PACK_SKIP_PIXELS, &sp);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)fbo);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_SKIP_ROWS, 0);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	glReadPixels(vp[0], vp[1], (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	GLenum err = glGetError();
	glPixelStorei(GL_PACK_ALIGNMENT, align);
	glPixelStorei(GL_PACK_ROW_LENGTH, rl);
	glPixelStorei(GL_PACK_SKIP_ROWS, sr);
	glPixelStorei(GL_PACK_SKIP_PIXELS, sp);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)old_pack);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old_read);
	double at = (double)(now - t_start) / 1e9;
	if (err != GL_NO_ERROR) {
		sweetdrm_say("7.7 offscreen fb %d tex %d viewport %zux%zu at %.1f s: read error 0x%04x", fbo, tex, w, h, at, err);
		free(px);
		return;
	}
	/* classes: 0 transparent, 1 opaque black, 2 dark, 3 other */
	static const char mark[4] = { '.', '#', '+', 'o' };
	unsigned cells[OFFSCREEN_GRID * OFFSCREEN_GRID][4];
	uint64_t total[4] = { 0 }, sum[4] = { 0 };
	memset(cells, 0, sizeof(cells));
	for (size_t y = 0; y < h; y++) {
		for (size_t x = 0; x < w; x++) {
			const uint8_t *p = px + (y * w + x) * 4;
			unsigned m = p[0] > p[1] ? p[0] : p[1];
			m = m > p[2] ? m : p[2];
			unsigned c = p[3] < 8 ? 0 : p[3] >= 248 && m < 16 ? 1 : m < 64 ? 2 : 3;
			total[c]++;
			for (unsigned i = 0; i < 4; i++) {
				sum[i] += p[i];
			}
			/* GL row 0 is the bottom; the picture's row 0 is the top */
			size_t cy = (OFFSCREEN_GRID - 1) - y * OFFSCREEN_GRID / h, cx = x * OFFSCREEN_GRID / w;
			cells[cy * OFFSCREEN_GRID + cx][c]++;
		}
	}
	free(px);
	uint64_t n = (uint64_t)w * h;
	sweetdrm_say("7.7 offscreen fb %d tex %d viewport %zux%zu at %d,%d after %u draws, prog %u, at %.1f s: transparent %u%% "
	    "black %u%% dark %u%% other %u%%; mean rgba %u %u %u %u (G8c)", fbo, tex, w, h, vp[0], vp[1], draws, fb_prog, at,
	    (unsigned)(total[0] * 100 / n), (unsigned)(total[1] * 100 / n), (unsigned)(total[2] * 100 / n),
	    (unsigned)(total[3] * 100 / n), (unsigned)(sum[0] / n), (unsigned)(sum[1] / n), (unsigned)(sum[2] / n),
	    (unsigned)(sum[3] / n));
	char line[4 * (OFFSCREEN_GRID + 1)];
	for (unsigned r0 = 0; r0 < OFFSCREEN_GRID; r0 += 4) {
		char *o = line;
		for (unsigned r = r0; r < r0 + 4; r++) {
			for (unsigned cx = 0; cx < OFFSCREEN_GRID; cx++) {
				const unsigned *k = cells[r * OFFSCREEN_GRID + cx];
				unsigned best = 0;
				for (unsigned c = 1; c < 4; c++) {
					best = k[c] > k[best] ? c : best;
				}
				*o++ = k[0] + k[1] + k[2] + k[3] == 0 ? ' ' : mark[best];
			}
			*o++ = r + 1 < r0 + 4 ? '|' : '\0';
		}
		sweetdrm_say("7.7 offscreen fb %d pic r%02u: %s", fbo, r0, line);
	}
}

/* glBindFramebuffer: the draw framebuffer CA leaves, when it drew into it */
static void
offscreen_done(GLenum target, GLuint next)
{
	static int on = -1;
	static struct {
		GLint tex, w, h;
		uint64_t last_ns;
	} seen[OFFSCREEN_KEYS];
	static unsigned logged;
	if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER) {
		return;
	}
	GLint cur = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &cur);
	if ((GLuint)cur == next) {
		return;
	}
	unsigned draws = os_draws;
	os_draws = 0;
	if (on < 0) {
		const char *v = getenv("SWEETEAGL_OFFSCREEN");
		on = !(v && v[0] == '0');
		sweetdrm_say("7.7 offscreen: read back framebuffers CA drew into, every %llu s per texture and size: %s",
		    OFFSCREEN_EVERY_NS / 1000000000ull, on ? "on" : "off (SWEETEAGL_OFFSCREEN=0)");
	}
	if (!on || cur == 0 || draws == 0 || logged >= OFFSCREEN_LOG_MAX) {
		return;
	}
	GLint type = 0, tex = 0, vp[4] = { 0 };
	glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
	if (type == GL_TEXTURE) {
		glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &tex);
	}
	glGetIntegerv(GL_VIEWPORT, vp);
	if (vp[2] <= 0 || vp[3] <= 0) {
		return;
	}
	uint64_t now = sweetdrm_now_ns();
	unsigned k = 0;
	while (k < OFFSCREEN_KEYS && seen[k].w != 0 && !(seen[k].tex == tex && seen[k].w == vp[2] && seen[k].h == vp[3])) {
		k++;
	}
	if (k == OFFSCREEN_KEYS) {
		return;
	}
	if (seen[k].w != 0 && now - seen[k].last_ns < OFFSCREEN_EVERY_NS) {
		return;
	}
	seen[k].tex = tex;
	seen[k].w = vp[2];
	seen[k].h = vp[3];
	seen[k].last_ns = now;
	logged++;
	offscreen_read(cur, tex, vp, draws);
}

/*
 * G8d: G8c's App Library header blur read back transparent after its passes
 * and the screen black where it lands; an incomplete texture samples as
 * (0,0,0,1). Before a draw, look at what each 2D sampler of the program Mesa
 * really has bound sees: texture, size and format of its base level, filters,
 * wrap, levels and sampler object, and flag the ones GLES calls incomplete.
 * Logged for the first TEXCHECK_PER_FB draws into each framebuffer every
 * TEXCHECK_EVERY_NS, and for every draw with a flagged texture (a few per
 * texture). SWEETEAGL_TEXCHECK=0 turns this off.
 */
#define TEXCHECK_PROGS          128
#define TEXCHECK_UNITS          4
#define TEXCHECK_FBS            256
#define TEXCHECK_PER_FB         8u
#define TEXCHECK_EVERY_NS       15000000000ull
#define TEXCHECK_LOG_MAX        1200u
#define TEXCHECK_BAD_PER_TEX    4u
#define TEXCHECK_LINE           200

struct tc_prog {
	GLuint prog;                    /* 0 = free */
	unsigned n;
	GLint loc[TEXCHECK_UNITS];
};
static struct tc_prog tc_progs[TEXCHECK_PROGS];
static struct {
	uint64_t since;
	unsigned n;
} tc_fb[TEXCHECK_FBS];
static unsigned char tc_bad[256];       /* flagged lines per texture name, modulo 256 */
static unsigned tc_logged;
static int texcheck_on = -1;
static bool tc_said;                    /* this draw's texcheck lines were logged (G8e logs its state after them) */

/* the 2D sampler uniforms of prog (names are not kept); NULL when the table is full */
static struct tc_prog *
tc_samplers(GLuint prog)
{
	unsigned h = prog % TEXCHECK_PROGS;
	for (unsigned i = 0; i < TEXCHECK_PROGS; i++) {
		struct tc_prog *e = &tc_progs[(h + i) % TEXCHECK_PROGS];
		if (e->prog == prog) {
			return e;
		}
		if (e->prog != 0) {
			continue;
		}
		e->prog = prog;
		e->n = 0;
		GLint count = 0;
		glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &count);
		for (GLint u = 0; u < count && e->n < TEXCHECK_UNITS; u++) {
			char name[64];
			GLsizei len = 0;
			GLint size = 0;
			GLenum type = 0;
			glGetActiveUniform(prog, (GLuint)u, sizeof(name), &len, &size, &type, name);
			if (type == GL_SAMPLER_2D) {
				e->loc[e->n++] = glGetUniformLocation(prog, name);
			}
		}
		return e;
	}
	return NULL;
}

static bool
tc_mipmap(GLint f)
{
	return f == GL_NEAREST_MIPMAP_NEAREST || f == GL_LINEAR_MIPMAP_NEAREST || f == GL_NEAREST_MIPMAP_LINEAR ||
	    f == GL_LINEAR_MIPMAP_LINEAR;
}

static bool
tc_linear(GLint minf, GLint magf)
{
	return magf == GL_LINEAR || minf == GL_LINEAR || minf == GL_LINEAR_MIPMAP_NEAREST ||
	    minf == GL_NEAREST_MIPMAP_LINEAR || minf == GL_LINEAR_MIPMAP_LINEAR;
}

/* one sampler: its description in out; the reason it is incomplete, or NULL */
static const char *
tc_unit(GLuint prog, GLint loc, char *out, size_t n, GLint *tex_out)
{
	GLint unit = 0, tex = 0, smp = 0, minf = 0, magf = 0, ws = 0, wt = 0, base = 0, maxl = 0, w0 = 0, h0 = 0, f0 = 0,
	    w1 = 0;
	glGetUniformiv(prog, loc, &unit);
	glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
	glGetIntegerv(GL_SAMPLER_BINDING, &smp);
	if (smp) {
		glGetSamplerParameteriv((GLuint)smp, GL_TEXTURE_MIN_FILTER, &minf);
		glGetSamplerParameteriv((GLuint)smp, GL_TEXTURE_MAG_FILTER, &magf);
		glGetSamplerParameteriv((GLuint)smp, GL_TEXTURE_WRAP_S, &ws);
		glGetSamplerParameteriv((GLuint)smp, GL_TEXTURE_WRAP_T, &wt);
	} else {
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minf);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magf);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &ws);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wt);
	}
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxl);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, base, GL_TEXTURE_WIDTH, &w0);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, base, GL_TEXTURE_HEIGHT, &h0);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, base, GL_TEXTURE_INTERNAL_FORMAT, &f0);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, base + 1, GL_TEXTURE_WIDTH, &w1);
	GLenum err = glGetError();
	const char *bad = NULL;
	if (tex == 0) {
		bad = "no texture bound";
	} else if (err != GL_NO_ERROR) {
		bad = NULL;     /* levels unknown: said in the line */
	} else if (w0 == 0 || h0 == 0) {
		bad = "no image at the base level";
	} else if (tc_mipmap(minf) && maxl > base && (w0 > 1 || h0 > 1) && w1 == 0) {
		bad = "mipmap filter, no level base+1";
	} else if ((f0 == GL_RGBA32F || f0 == GL_RGB32F || f0 == GL_RG32F || f0 == GL_R32F) && tc_linear(minf, magf)) {
		bad = "32-bit float with linear filtering";
	}
	snprintf(out, n, "u%d tex %d %dx%d fmt 0x%x min 0x%x mag 0x%x wrap 0x%x/0x%x levels %d..%d next w %d smp %d "
	    "err 0x%x%s%s", unit, tex, w0, h0, f0, minf, magf, ws, wt, base, maxl, w1, smp, err, bad ? ": INCOMPLETE, " : "",
	    bad ? bad : "");
	*tex_out = tex;
	return bad;
}

/* before a draw (GL lock held): sets fb_prog to the program Mesa has bound */
/*
 * G8f: G8e's App Library blur passes (512x176, 497x164) ended with draws that
 * had no program bound in Mesa, and read back fully transparent, while CA had
 * called glUseProgram and every link succeeded. For each such draw (the first
 * NOPROG_LOG_MAX, then at powers of two) say what CA's last glUseProgram was:
 * its program, whether that name is a linked program here, and whether it ran
 * on this slot context and this Mesa context. The draw's state follows (G8e).
 */
#define NOPROG_LOG_MAX          300u
#define NOPROG_TRACE_FULL       6u      /* G8g: full-screen no-program draws whose recent calls are logged */
#define NOPROG_TRACE_SMALL      40u     /* and smaller ones (the blur passes) */

static void
noprog_draw(void *ctx, GLint fb)
{
	static unsigned n, logged;
	n++;
	if (logged >= NOPROG_LOG_MAX && (n & (n - 1)) != 0) {
		return;
	}
	logged++;
	GLint vp[4] = { 0 }, linked = -1;
	glGetIntegerv(GL_VIEWPORT, vp);
	GLboolean is = cur_prog ? glIsProgram(cur_prog) : GL_FALSE;
	if (is) {
		glGetProgramiv(cur_prog, GL_LINK_STATUS, &linked);
	}
	sweetdrm_say("7.7 noprog fb %d viewport %dx%d: draw %u with no program; last UseProgram %u (%s, linked %d) "
	    "on %s slot context, %s Mesa context (G8f)", fb, vp[2], vp[3], n, cur_prog, is ? "a program here" :
	    "not a program here", linked, use_ctx == ctx ? "this" : "another", use_gl == sweetgl_get_current() ? "this" :
	    "another");
	tc_said = true;
	/* G8f found CA itself passed 0 to its last glUseProgram: G8g says what it called since */
	static unsigned traced_small, traced_full;
	bool full = vp[2] == 828 && vp[3] == 1792;
	if (full ? traced_full++ < NOPROG_TRACE_FULL : traced_small++ < NOPROG_TRACE_SMALL) {
		sweeteagl_recent_calls("noprog");
	}
}

static void
texcheck_draw(void *ctx)
{
	tc_said = false;
	if (texcheck_on < 0) {
		const char *v = getenv("SWEETEAGL_TEXCHECK");
		texcheck_on = !(v && v[0] == '0');
		sweetdrm_say("7.7 texcheck: sampled texture state before draws: %s", texcheck_on ? "on" :
		    "off (SWEETEAGL_TEXCHECK=0)");
	}
	if (!texcheck_on) {
		fb_prog = cur_prog;
		return;
	}
	GLint prog = 0, fb = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
	fb_prog = (GLuint)prog;
	if (prog == 0) {
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
		noprog_draw(ctx, fb);
		return;
	}
	if (tc_logged >= TEXCHECK_LOG_MAX) {
		return;
	}
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
	struct tc_prog *pe = tc_samplers((GLuint)prog);
	if (pe == NULL) {
		return;
	}
	uint64_t now = sweetdrm_now_ns();
	bool window = false;
	if (fb >= 0 && fb < TEXCHECK_FBS) {
		if (now - tc_fb[fb].since >= TEXCHECK_EVERY_NS) {
			tc_fb[fb].since = now;
			tc_fb[fb].n = 0;
		}
		window = tc_fb[fb].n < TEXCHECK_PER_FB;
	}
	GLint active = GL_TEXTURE0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	char desc[TEXCHECK_UNITS][TEXCHECK_LINE];
	bool flagged = false, wanted = window;
	for (unsigned k = 0; k < pe->n; k++) {
		GLint tex = 0;
		if (tc_unit((GLuint)prog, pe->loc[k], desc[k], sizeof(desc[k]), &tex) != NULL) {
			flagged = true;
			if (tc_bad[(unsigned)tex & 255u] < TEXCHECK_BAD_PER_TEX) {
				tc_bad[(unsigned)tex & 255u]++;
				wanted = true;
			}
		}
	}
	glActiveTexture((GLenum)active);
	if (!wanted) {
		return;
	}
	if (window) {
		tc_fb[fb].n++;
	}
	GLint vp[4] = { 0 };
	glGetIntegerv(GL_VIEWPORT, vp);
	tc_logged++;
	tc_said = true;
	sweetdrm_say("7.7 texcheck fb %d viewport %dx%d prog %d: %u samplers%s (G8d)", fb, vp[2], vp[3], prog, pe->n,
	    flagged ? ", INCOMPLETE" : "");
	for (unsigned k = 0; k < pe->n; k++) {
		sweetdrm_say("7.7 texcheck fb %d   %s", fb, desc[k]);
	}
}

/*
 * G8e: every sampler of the App Library blur passes is complete (G8d), yet
 * G8c read them back transparent. So after each draw texcheck logs, log the
 * state that can keep a draw from writing: scissor, colour mask, blend,
 * depth, stencil, cull and rasterizer discard; the program's first mat4
 * uniform; and for each enabled float attribute array, the range of every
 * component over the vertices the draw uses (client memory or a mapped
 * buffer). SWEETEAGL_DRAWSTATE=0 turns it off.
 */
#define DRAWSTATE_ATTRIBS       8u
#define DRAWSTATE_MAX_VERTS     4096u
#define DRAWSTATE_LOG_MAX       1500u

/* maps len bytes of buffer buf at off for reading through target; *old is the binding to give back */
static const uint8_t *
ds_map(GLenum target, GLenum binding, GLint buf, intptr_t off, size_t len, GLint *old)
{
	glGetIntegerv(binding, old);
	glBindBuffer(target, (GLuint)buf);
	const void *p = glMapBufferRange(target, off, (GLsizeiptr)len, GL_MAP_READ_BIT);
	if (p == NULL) {
		glGetError();
		glBindBuffer(target, (GLuint)*old);
	}
	return p;
}

static void
ds_unmap(GLenum target, GLint old)
{
	glUnmapBuffer(target);
	glBindBuffer(target, (GLuint)old);
}

static const char *
ds_on(GLenum cap)
{
	return glIsEnabled(cap) ? "on" : "off";
}

static void
drawstate(GLenum mode, GLint first, GLsizei count, GLenum itype, const void *indices)
{
	static int on = -1;
	static unsigned logged;
	if (on < 0) {
		const char *v = getenv("SWEETEAGL_DRAWSTATE");
		on = !(v && v[0] == '0');
		sweetdrm_say("7.7 drawstate: write state and vertex ranges of the draws texcheck logs: %s",
		    on ? "on" : "off (SWEETEAGL_DRAWSTATE=0)");
	}
	if (!on || !tc_said || logged >= DRAWSTATE_LOG_MAX || count <= 0) {
		return;
	}
	logged++;
	/* the queries below clear errors: hold a pending one for CA's glGetError */
	GLenum pend = glGetError();
	if (pend != GL_NO_ERROR && held_error == GL_NO_ERROR) {
		held_error = pend;
	}
	GLint fb = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
	GLint vp[4] = { 0 }, sc[4] = { 0 }, bs = 0, bd = 0, bsa = 0, bda = 0, eq = 0, eqa = 0, df = 0, sf = 0, sref = 0,
	    smask = 0, sfail = 0, szf = 0, szp = 0, swm = 0, cullf = 0;
	GLboolean cm[4] = { 0 }, dw = 0;
	glGetIntegerv(GL_VIEWPORT, vp);
	glGetIntegerv(GL_SCISSOR_BOX, sc);
	glGetBooleanv(GL_COLOR_WRITEMASK, cm);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &dw);
	glGetIntegerv(GL_BLEND_SRC_RGB, &bs);
	glGetIntegerv(GL_BLEND_DST_RGB, &bd);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsa);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &bda);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &eq);
	glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqa);
	glGetIntegerv(GL_DEPTH_FUNC, &df);
	glGetIntegerv(GL_STENCIL_FUNC, &sf);
	glGetIntegerv(GL_STENCIL_REF, &sref);
	glGetIntegerv(GL_STENCIL_VALUE_MASK, &smask);
	glGetIntegerv(GL_STENCIL_FAIL, &sfail);
	glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &szf);
	glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &szp);
	glGetIntegerv(GL_STENCIL_WRITEMASK, &swm);
	glGetIntegerv(GL_CULL_FACE_MODE, &cullf);
	sweetdrm_say("7.7 drawstate fb %d mode 0x%x first %d count %d itype 0x%x: viewport %d,%d %dx%d scissor %s %d,%d %dx%d "
	    "mask %d%d%d%d blend %s 0x%x/0x%x 0x%x/0x%x eq 0x%x/0x%x depth %s func 0x%x write %d stencil %s func 0x%x ref %d "
	    "mask 0x%x ops 0x%x/0x%x/0x%x wmask 0x%x cull %s 0x%x discard %s (G8e)", fb, mode, first, count, itype, vp[0],
	    vp[1], vp[2], vp[3], ds_on(GL_SCISSOR_TEST), sc[0], sc[1], sc[2], sc[3], cm[0], cm[1], cm[2], cm[3],
	    ds_on(GL_BLEND), bs, bd, bsa, bda, eq, eqa, ds_on(GL_DEPTH_TEST), df, dw, ds_on(GL_STENCIL_TEST), sf, sref,
	    (unsigned)smask, sfail, szf, szp, (unsigned)swm, ds_on(GL_CULL_FACE), cullf, ds_on(GL_RASTERIZER_DISCARD));

	/* the program's first mat4 uniform */
	if (fb_prog != 0) {
		GLint nu = 0;
		glGetProgramiv(fb_prog, GL_ACTIVE_UNIFORMS, &nu);
		for (GLint u = 0; u < nu; u++) {
			char name[64];
			GLsizei len = 0;
			GLint size = 0;
			GLenum type = 0;
			glGetActiveUniform(fb_prog, (GLuint)u, sizeof(name), &len, &size, &type, name);
			if (type != GL_FLOAT_MAT4) {
				continue;
			}
			GLfloat m[16] = { 0 };
			glGetUniformfv(fb_prog, glGetUniformLocation(fb_prog, name), m);
			sweetdrm_say("7.7 drawstate fb %d   mat4 %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
			    "%.3f %.3f %.3f %.3f", fb, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
			    m[12], m[13], m[14], m[15]);
			break;
		}
	}

	/* the vertices the draw uses: [lo, hi] */
	GLint lo = first, hi = first + count - 1;
	if (itype != 0) {
		size_t isz = itype == GL_UNSIGNED_BYTE ? 1 : itype == GL_UNSIGNED_SHORT ? 2 : 4;
		GLint ebuf = 0, old = 0;
		glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ebuf);
		const uint8_t *ix = indices;
		if (ebuf != 0) {
			ix = ds_map(GL_ELEMENT_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER_BINDING, ebuf, (intptr_t)indices,
			    (size_t)count * isz, &old);
		}
		if (ix == NULL) {
			sweetdrm_say("7.7 drawstate fb %d   indices not read (buffer %d)", fb, ebuf);
			return;
		}
		lo = INT32_MAX;
		hi = 0;
		for (GLsizei i = 0; i < count; i++) {
			GLint v = isz == 1 ? ix[i] : isz == 2 ? ((const uint16_t *)ix)[i] : (GLint)((const uint32_t *)ix)[i];
			lo = v < lo ? v : lo;
			hi = v > hi ? v : hi;
		}
		if (ebuf != 0) {
			ds_unmap(GL_ELEMENT_ARRAY_BUFFER, old);
		}
	}
	if (hi - lo + 1 > (GLint)DRAWSTATE_MAX_VERTS) {
		sweetdrm_say("7.7 drawstate fb %d   vertices %d..%d: too many to read", fb, lo, hi);
		return;
	}
	for (GLuint a = 0; a < DRAWSTATE_ATTRIBS; a++) {
		GLint en = 0, size = 0, type = 0, stride = 0, buf = 0, old = 0;
		void *ptr = NULL;
		glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
		if (glGetError() != GL_NO_ERROR || !en) {
			continue;
		}
		glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
		glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
		glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
		glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
		glGetVertexAttribPointerv(a, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
		if (type != GL_FLOAT || size < 1 || size > 4) {
			sweetdrm_say("7.7 drawstate fb %d   a%u size %d type 0x%x stride %d buf %d off %p: not float", fb, a, size,
			    type, stride, buf, ptr);
			continue;
		}
		size_t step = stride ? (size_t)stride : (size_t)size * 4;
		size_t span = (size_t)(hi - lo) * step + (size_t)size * 4;
		const uint8_t *base = (const uint8_t *)ptr + (size_t)lo * step;
		if (buf != 0) {
			base = ds_map(GL_ARRAY_BUFFER, GL_ARRAY_BUFFER_BINDING, buf, (intptr_t)base, span, &old);
		}
		if (base == NULL) {
			sweetdrm_say("7.7 drawstate fb %d   a%u buf %d off %p: not read", fb, a, buf, ptr);
			continue;
		}
		float mn[4] = { 0 }, mx[4] = { 0 };
		for (GLint v = 0; v <= hi - lo; v++) {
			const float *f = (const float *)(base + (size_t)v * step);
			for (GLint c = 0; c < size; c++) {
				mn[c] = v == 0 || f[c] < mn[c] ? f[c] : mn[c];
				mx[c] = v == 0 || f[c] > mx[c] ? f[c] : mx[c];
			}
		}
		if (buf != 0) {
			ds_unmap(GL_ARRAY_BUFFER, old);
		}
		sweetdrm_say("7.7 drawstate fb %d   a%u size %d stride %d buf %d vertices %d..%d: %.3f..%.3f %.3f..%.3f "
		    "%.3f..%.3f %.3f..%.3f", fb, a, size, stride, buf, lo, hi, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], mn[3],
		    mx[3]);
	}
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
	os_draws++;
	texcheck_draw(ctx);
	drawstate(m, f, n, 0, NULL);
	fb_before_draw();
	void *probe = sweeteagl_probe_begin(m, f, n, 0, NULL);
	glDrawArrays(m, f, n);
	sweeteagl_probe_end(probe);
}
static void s_DrawElements(CTX, GLenum m, GLsizei n, GLenum t, const void *i)
{
	SWEETEAGL_ENTER(ctx);
	TIMED(SWEETDRM_T_DRAW);
	count(&sweeteagl_diag.draws, ctx);
	os_draws++;
	texcheck_draw(ctx);
	drawstate(m, 0, n, t, i);
	fb_before_draw();
	void *probe = sweeteagl_probe_begin(m, 0, n, t, i);
	glDrawElements(m, n, t, i);
	sweeteagl_probe_end(probe);
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
static const GLubyte *s_GetString(CTX, GLenum n)
{
	SWEETEAGL_ENTER(ctx);
	const GLubyte *s = glGetString(n);
	static unsigned said;
	if (s == NULL && said++ < 4) {
		sweetdrm_say("7.7 eagl: glGetString(0x%04x) returned NULL (G8h)", n);      /* the 0x0500 G8c blamed */
	}
	return s;
}
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
/* G8h: an address as image+offset (nearest symbol+offset), for matching against the local binaries */
static void
say_frame(char *out, size_t n, const void *pc)
{
	Dl_info di;
	if (pc != NULL && dladdr(pc, &di) && di.dli_fname != NULL) {
		const char *img = strrchr(di.dli_fname, '/');
		snprintf(out, n, "%s+0x%lx (%s+0x%lx)", img ? img + 1 : di.dli_fname,
		    (unsigned long)((const char *)pc - (const char *)di.dli_fbase), di.dli_sname ? di.dli_sname : "?",
		    di.dli_saddr ? (unsigned long)((const char *)pc - (const char *)di.dli_saddr) : 0ul);
	} else {
		snprintf(out, n, "%p", pc);
	}
}

/*
 * G8g showed every no-program draw follows CA's glUseProgram(0) with no
 * program creation in between, from the very first draw. G8h logs the first
 * USE0_SITES distinct call chains (3 frames, by frame pointer) of those calls.
 */
#define USE0_SITES      12u
#define USE0_FRAMES     3u
static const void *use0_site[USE0_SITES][USE0_FRAMES];
static unsigned use0_sites, use0_calls;

/*
 * G8h placed both sites in CA's GL shader builder: one binds 0 after a
 * program is built, the other binds 0 first and is followed by no GL call,
 * the path that returns no program when CA's GL renderer has no fragment
 * source for the shader asked for. G8i logs that shader's key: the builder
 * keeps its argument (CA's shader description) in x21, which slot 600's entry
 * stub below stashes. The site offset is of the iOS 16 QuartzCore on the phone.
 */
#define CREATE_SHADER_USE0      0x22f134ul
#define SHADER_KEYS             32u
uint64_t sweeteagl_use_x21;

static void
shader_note(const void *ra)
{
	static unsigned keys;
	static uint32_t seen[SHADER_KEYS];
	const unsigned char *sh = (const unsigned char *)sweeteagl_use_x21;
	Dl_info di;
	if (keys >= SHADER_KEYS || ra == NULL || sh == NULL || !dladdr(ra, &di) || di.dli_fname == NULL ||
	    strstr(di.dli_fname, "/QuartzCore") == NULL ||
	    (uintptr_t)ra - (uintptr_t)di.dli_fbase != CREATE_SHADER_USE0) {
		return;
	}
	uint16_t target;
	memcpy(&target, sh + 8, sizeof(target));
	uint32_t key = (uint32_t)sh[0] | (uint32_t)target << 8 | (uint32_t)sh[0x5e] << 24;
	for (unsigned i = 0; i < keys; i++) {
		if (seen[i] == key) {
			return;
		}
	}
	seen[keys++] = key;
	char hex[2 * 0x30 + 1];
	for (unsigned i = 0; i < 0x30; i++) {
		snprintf(hex + 2 * i, 3, "%02x", sh[i]);
	}
	sweetdrm_say("7.7 shader key %u: id %u target 0x%04x byte5e %u; first 0x30 bytes %s (G8i)", keys - 1, sh[0],
	    target, sh[0x5e], hex);
}

static void
use0_note(void **fp)            /* s_UseProgram's frame record: { caller's record, return address } */
{
	const void *pc[USE0_FRAMES] = { NULL };
	for (unsigned i = 0; i < USE0_FRAMES && fp != NULL; i++) {
		pc[i] = __builtin_extract_return_addr(fp[1]);
		fp = fp[0];
	}
	shader_note(pc[0]);
	use0_calls++;
	for (unsigned i = 0; i < use0_sites; i++) {
		if (memcmp(use0_site[i], pc, sizeof(pc)) == 0) {
			return;
		}
	}
	if (use0_sites >= USE0_SITES) {
		return;
	}
	memcpy(use0_site[use0_sites], pc, sizeof(pc));
	char f[USE0_FRAMES][160];
	for (unsigned i = 0; i < USE0_FRAMES; i++) {
		say_frame(f[i], sizeof(f[i]), pc[i]);
	}
	sweetdrm_say("7.7 useprog0 site %u (call %u): %s <- %s <- %s (G8h)", use0_sites++, use0_calls, f[0], f[1], f[2]);
}

static void s_UseProgram(CTX, GLuint p) __asm__("_sweeteagl_s_UseProgram") __attribute__((used));

/* G8i: slot 600's entry: stash the caller's x21 (callee-saved, so still CA's here), then run s_UseProgram */
__attribute__((naked)) static void
s_UseProgram_entry(void)
{
	__asm__ volatile(
	    "adrp x16, _sweeteagl_use_x21@PAGE\n\t"
	    "str x21, [x16, _sweeteagl_use_x21@PAGEOFF]\n\t"
	    "b _sweeteagl_s_UseProgram");
}

static void s_UseProgram(CTX, GLuint p)
{
	SWEETEAGL_ENTER(ctx);
	count(&sweeteagl_diag.programs, ctx);
	if (p == 0) {
		use0_note(__builtin_frame_address(0));
	}
	cur_prog = p;
	use_ctx = ctx;
	use_gl = sweetgl_get_current();
	glUseProgram(p);
}
static void s_Uniform1f(CTX, GLint l, GLfloat a) { SWEETEAGL_ENTER(ctx); glUniform1f(l, a); }
static void s_Uniform4f(CTX, GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { SWEETEAGL_ENTER(ctx); glUniform4f(l, a, b, c, d); }
static void s_Uniform1i(CTX, GLint l, GLint a) { SWEETEAGL_ENTER(ctx); glUniform1i(l, a); }
static void s_Uniform3fv(CTX, GLint l, GLsizei n, const GLfloat *v) { SWEETEAGL_ENTER(ctx); glUniform3fv(l, n, v); }
static void s_Uniform4fv(CTX, GLint l, GLsizei n, const GLfloat *v) { SWEETEAGL_ENTER(ctx); glUniform4fv(l, n, v); }
static void s_UniformMatrix3fv(CTX, GLint l, GLsizei n, GLboolean t, const GLfloat *v) { SWEETEAGL_ENTER(ctx); glUniformMatrix3fv(l, n, t, v); }
static void s_UniformMatrix4fv(CTX, GLint l, GLsizei n, GLboolean t, const GLfloat *v) { SWEETEAGL_ENTER(ctx); glUniformMatrix4fv(l, n, t, v); }
static GLint s_GetUniformLocation(CTX, GLuint p, const GLchar *n) { SWEETEAGL_ENTER(ctx); return glGetUniformLocation(p, n); }
static void s_BindAttribLocation(CTX, GLuint p, GLuint i, const GLchar *n) { SWEETEAGL_ENTER(ctx); glBindAttribLocation(p, i, n); }
static void s_BindFramebuffer(CTX, GLenum t, GLuint f)
{
	SWEETEAGL_ENTER(ctx);
	count(&sweeteagl_diag.fb_binds, ctx);
	offscreen_done(t, f);
	glBindFramebuffer(t, f);
}
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
	{ 600, s_UseProgram_entry },        { 602, s_Uniform1f },           { 605, s_Uniform4f },
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
