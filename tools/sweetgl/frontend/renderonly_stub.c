/*
 * Mesa builds renderonly.c only when meson finds libdrm, and our cross build
 * uses sweetdrm without pkg-config, so it is left out. freedreno calls these
 * two only for a renderonly device (a display GPU apart from the render GPU);
 * sweetgl passes none, so they are never reached.
 */
#include <stddef.h>
#include "renderonly/renderonly.h"

struct renderonly_scanout *
renderonly_create_gpu_import_for_resource(struct pipe_resource *rsc, struct renderonly *ro,
    struct winsys_handle *out_handle)
{
	(void)rsc;
	(void)ro;
	(void)out_handle;
	return NULL;
}

void
renderonly_scanout_destroy(struct renderonly_scanout *scanout, struct renderonly *ro)
{
	(void)scanout;
	(void)ro;
}
