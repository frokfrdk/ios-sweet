/*
 * sweetgl (ios-sweet plan Stage 7.3): a surfaceless OpenGL ES frontend for
 * Mesa's freedreno driver on SweetGPU. There are no window-system drawables: a
 * context is made current with no framebuffer and renders only into FBOs it
 * creates. The gl* entry points (libsweetgl.a, from Mesa's GLESv2 dispatch
 * stubs) go to whichever context is current on the calling thread.
 *
 * This is the layer the EAGL shim sits on: an EAGLContext holds one
 * sweetgl_context, and its renderbuffer storage is an FBO's.
 */
#ifndef SWEETGL_H
#define SWEETGL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* exported even though Mesa's flags make everything else hidden, for the shim's dylib */
#define SWEETGL_EXPORT __attribute__((visibility("default")))

struct sweetgl_display;
struct sweetgl_context;

/*
 * Create the GPU screen on fd, an open sweetdrm device (drmOpenWithType("msm")).
 * The caller keeps fd open while the display lives. NULL on failure.
 */
SWEETGL_EXPORT struct sweetgl_display *sweetgl_display_open(int fd);

/* The driver's renderer name, e.g. "FD618". */
SWEETGL_EXPORT const char *sweetgl_display_name(struct sweetgl_display *d);

/* Destroy the screen. Every context of d must be destroyed first. */
SWEETGL_EXPORT void sweetgl_display_close(struct sweetgl_display *d);

/*
 * An OpenGL ES context of at least version major.minor (2.0, 3.0, ...),
 * sharing objects with share unless NULL. NULL if the driver can't do that
 * version or on failure.
 */
SWEETGL_EXPORT struct sweetgl_context *sweetgl_context_create(struct sweetgl_display *d, int major, int minor,
    struct sweetgl_context *share);

/* Make c current on the calling thread, or release the current one if NULL. */
SWEETGL_EXPORT bool sweetgl_make_current(struct sweetgl_context *c);

SWEETGL_EXPORT struct sweetgl_context *sweetgl_get_current(void);

/* Flush c's pending GL work to the GPU without waiting (glFlush on c). */
SWEETGL_EXPORT void sweetgl_flush(struct sweetgl_context *c);

/* Destroy c, releasing it first if it is current. */
SWEETGL_EXPORT void sweetgl_context_destroy(struct sweetgl_context *c);

/* The entry point of a GL function by name, or NULL. */
SWEETGL_EXPORT void (*sweetgl_get_proc_address(const char *name))(void);

#ifdef __cplusplus
}
#endif

#endif
