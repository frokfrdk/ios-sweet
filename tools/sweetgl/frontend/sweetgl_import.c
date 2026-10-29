/*
 * sweetgl (Stage 7.5): an IOSurface as a freedreno resource. Built with
 * freedreno_resource.c's command, since it needs fd_screen's device.
 */
#include "sweetdrm.h"

#include "drm-uapi/drm_fourcc.h"
#include "frontend/winsys_handle.h"
#include "pipe/p_defines.h"
#include "util/u_inlines.h"        /* before freedreno's headers, which use it without including it */
#include "freedreno_screen.h"

struct pipe_resource *sweetgl_fd_import_iosurface(struct pipe_screen *pscreen, int fd, uint32_t surface_id,
    void *cpu, const struct pipe_resource *tmpl, unsigned stride);

struct pipe_resource *
sweetgl_fd_import_iosurface(struct pipe_screen *pscreen, int fd, uint32_t surface_id, void *cpu,
    const struct pipe_resource *tmpl, unsigned stride)
{
	uint32_t handle = 0;
	uint64_t size = 0;
	int e = sweetdrm_import_iosurface(fd, surface_id, cpu, &handle, &size);
	if (e) {
		sweetdrm_say("7.5 image: import of surface %u: %d", surface_id, e);
		return NULL;
	}
	/*
	 * resource_from_handle imports a KMS handle with size 0, as a real kernel
	 * reports the size on the dma-buf path only, and the layout check then fails.
	 * So put the BO in the device's handle table first, with its size; the
	 * resource's import finds it there and takes its own reference.
	 */
	struct fd_bo *bo = fd_bo_from_handle(fd_screen(pscreen)->dev, handle, (uint32_t)size);
	if (bo == NULL) {
		sweetdrm_say("7.5 image: no BO for handle %u", handle);       /* the handle is closed */
		return NULL;
	}
	struct winsys_handle wh = {
		.type = WINSYS_HANDLE_TYPE_KMS,
		.handle = handle,
		.stride = stride,
		.modifier = DRM_FORMAT_MOD_LINEAR,
	};
	struct pipe_resource *r = pscreen->resource_from_handle(pscreen, tmpl, &wh, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
	if (r == NULL) {
		sweetdrm_say("7.5 image: surface %u (%ux%u, %u bytes per row, %llu bytes) is no linear resource",
		    surface_id, tmpl->width0, tmpl->height0, stride, (unsigned long long)size);
	}
	fd_bo_del(bo);
	return r;
}
