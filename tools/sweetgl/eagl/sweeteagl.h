/*
 * sweeteagl (ios-sweet plan Stage 7.5): EAGLContext on Mesa, for Core Animation's
 * GLES renderer. Internal header shared by the shim's files. The layouts here
 * are from docs/EAGL_NOTES.md (Stage 7.4), our own notes on the interface.
 */
#ifndef SWEETEAGL_H
#define SWEETEAGL_H

#include <stddef.h>
#include <stdint.h>

#include "sweetgl.h"

#define SWEETEAGL_SLOTS         1036u

/*
 * An EAGLContext's _private block. The first 0x40e8 bytes are the layout
 * OpenGLES's own code reads (its gl* exports, -API, -sharegroup,
 * GLIContextFromEAGLContext and the GLC*Dispatch functions); ours follow.
 */
struct sweeteagl_priv {
	void *object;                           /* +0x00 the EAGLContext, what +currentContext returns */
	void *sharegroup;                       /* +0x08 */
	uint64_t api;                           /* +0x10 */
	uint64_t flags;                         /* +0x18 */
	/* +0x20: getMacroContextPrivate returns this address */
	void *gli_ctx;                          /* passed as every slot's first argument */
	void *fn[SWEETEAGL_SLOTS];              /* +0x28 */
	void *back[SWEETEAGL_SLOTS];            /* +0x2088 the back dispatch, zero = "not saved" */
	/* ours */
	struct sweetgl_context *gl;
	int32_t multithreaded;                  /* parameter 0x139 */
};

_Static_assert(offsetof(struct sweeteagl_priv, gli_ctx) == 0x20, "gli_ctx");
_Static_assert(offsetof(struct sweeteagl_priv, fn) == 0x28, "fn");
_Static_assert(offsetof(struct sweeteagl_priv, back) == 0x2088, "back");
_Static_assert(offsetof(struct sweeteagl_priv, gl) == 0x40e8, "size of Apple's part");

/* Fill fn[] (sweeteagl_slots.c): Mesa for the slots we know, a logging stub elsewhere */
void sweeteagl_fill_table(void *fn[SWEETEAGL_SLOTS]);

/* The generated wrappers (sweeteagl_gen.c, from gen_slots.py): applied before the hand-written ones */
struct sweeteagl_gen_slot {
	uint16_t slot;
	void *fn;
};
extern const struct sweeteagl_gen_slot sweeteagl_gen_slots[];
extern const unsigned sweeteagl_gen_count;
/* Makes ctx (a slot's first argument) current in Mesa, then logs a generated slot's first use, once per slot */
void sweeteagl_gen_used(void *ctx, unsigned slot, const char *name);

/*
 * Makes p's Mesa context current on this thread if it is not (sweeteagl.m).
 * Every slot calls it first: CA reaches the table on threads where only
 * OpenGLES's TSD slot names the context.
 */
void sweeteagl_ensure_current(void *p);

/* 7.7 diagnostics: counted by the slots (sweeteagl_slots.c), reported by sweeteagl.m's diag thread */
#ifndef __cplusplus
#include <stdatomic.h>
struct sweeteagl_diag {
	atomic_uint draws, clears, fb_binds, programs, flushes, finishes;
	atomic_uint not_current;                /* counted calls whose slot context was not Mesa's current one */
	atomic_uint compiles, compile_fails, links, link_fails, fb_incomplete;
	atomic_uint gl_errors, last_gl_error;   /* as CA's own glGetError calls saw them */
	atomic_uint wait_fails;                 /* glClientWaitSync timeouts and failures */
	atomic_uint gen_calls, unknown_calls;   /* generated-wrapper and stub slots */
};
extern struct sweeteagl_diag sweeteagl_diag;
#endif

/* The generated stubs (sweeteagl_stubs.S): stub i loads i and calls this, which logs once and returns 0 */
extern const uint32_t sweeteagl_unknown_stubs[SWEETEAGL_SLOTS * 2u];
long sweeteagl_unknown_slot(unsigned slot);

#endif
