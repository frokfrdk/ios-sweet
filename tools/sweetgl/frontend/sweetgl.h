/*
 * sweetgl (ios-sweet plan Stage 7.3): a surfaceless OpenGL ES frontend for
 * Mesa's freedreno driver on SweetGPU. There are no window-system drawables: a
 * context is made current with no framebuffer and renders only into FBOs it
 * creates. The gl* entry points (libsweetgl.a, from Mesa's GLESv2 dispatch
 * stubs) go to whichever context is current on the calling thread.
 *
 * This is the layer the EAGL shim sits on: an EAGLContext holds one
 * sweetgl_context, and IOSurfaces become its textures through sweetgl images.
 */
#ifndef SWEETGL_H
#define SWEETGL_H

#include <stdbool.h>
#include <stdint.h>

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

/*
 * Stage 7.5: IOSurface memory as GL storage, with no copy. An image is the
 * surface's pages imported into GPU VA (SweetGPU's import selector) and wrapped
 * as a linear 2D resource. Pass the image pointer as the GLeglImageOES of
 * glEGLImageTargetTexture2DOES or glEGLImageTargetRenderbufferStorageOES; the
 * texture or renderbuffer then keeps its own reference, so the image may be
 * destroyed straight after.
 */
struct sweetgl_image;

/* IOSurface pixel formats (kIOSurfacePixelFormat) an image can have */
#define SWEETGL_FOURCC_BGRA     0x42475241u     /* 'BGRA': B8G8R8A8 in memory order */
#define SWEETGL_FOURCC_RGBA     0x52474241u     /* 'RGBA' */

/*
 * Import surface_id (IOSurfaceGetID) as a width x height image with stride bytes
 * per row (a multiple of 256) of pixel format fourcc. cpu is the surface's base
 * address in this task, for the driver's CPU access. NULL on failure, with the
 * reason logged through sweetdrm_say.
 */
SWEETGL_EXPORT struct sweetgl_image *sweetgl_image_from_iosurface(struct sweetgl_display *d, uint32_t surface_id,
    void *cpu, unsigned width, unsigned height, unsigned stride, uint32_t fourcc);

SWEETGL_EXPORT void sweetgl_image_destroy(struct sweetgl_image *img);

#ifdef __cplusplus
}
#endif

#endif
