/*
 * sweetgl (Stage 7.3): Mesa's GL state tracker on the freedreno screen, with
 * no window system. Modelled on the hgl frontend (src/gallium/frontends/hgl):
 * a pipe_frontend_screen around the pipe_screen, st contexts created and made
 * current through the st API, but never a pipe_frontend_drawable, so every
 * context is surfaceless and draws into FBOs only. See sweetgl.h.
 */
#include <stdlib.h>

#include "sweetgl.h"

#include "frontend/api.h"
#include "glapi/glapi/glapi.h"
#include "main/menums.h"
#include "pipe/p_screen.h"
#include "state_tracker/st_context.h"
#include "util/driconf.h"
#include "util/u_memory.h"
#include "util/xmlconfig.h"
#include "freedreno/drm/freedreno_drm_public.h"

/* the msm target's options (gallium/auxiliary/target-helpers/drm_helper.h); fd_screen_create reads them */
static const driOptionDescription fd_driconf[] = {
#include "freedreno/driinfo_freedreno.h"
};

struct sweetgl_display {
	/* first, so the st's fscreen pointer is the display */
	struct pipe_frontend_screen fscreen;
	driOptionCache options_info, options;
};

struct sweetgl_context {
	struct sweetgl_display *display;
	struct st_context *st;
};

static _Thread_local struct sweetgl_context *current;

static int
sweetgl_get_param(struct pipe_frontend_screen *fscreen, enum st_manager_param param)
{
	(void)fscreen;
	switch (param) {
	case ST_MANAGER_BROKEN_INVALIDATE:
		/* no drawables to invalidate; glViewport need not revalidate */
		return 0;
	default:
		return 0;
	}
}

struct sweetgl_display *
sweetgl_display_open(int fd)
{
	struct sweetgl_display *d = CALLOC_STRUCT(sweetgl_display);
	if (d == NULL) {
		return NULL;
	}
	driParseOptionInfo(&d->options_info, fd_driconf, ARRAY_SIZE(fd_driconf));
	driParseConfigFiles(&d->options, &d->options_info, &(driConfigFileParseParams){ .driverName = "msm" });
	struct pipe_screen_config config = { .options = &d->options, .options_info = &d->options_info };
	d->fscreen.screen = fd_drm_screen_create_renderonly(fd, NULL, &config);
	if (d->fscreen.screen == NULL) {
		driDestroyOptionCache(&d->options);
		driDestroyOptionInfo(&d->options_info);
		FREE(d);
		return NULL;
	}
	d->fscreen.get_param = sweetgl_get_param;
	return d;
}

const char *
sweetgl_display_name(struct sweetgl_display *d)
{
	return d->fscreen.screen->get_name(d->fscreen.screen);
}

void
sweetgl_display_close(struct sweetgl_display *d)
{
	if (d == NULL) {
		return;
	}
	/* the st's per-screen state, made by the first context */
	st_screen_destroy(&d->fscreen);
	d->fscreen.screen->destroy(d->fscreen.screen);
	driDestroyOptionCache(&d->options);
	driDestroyOptionInfo(&d->options_info);
	FREE(d);
}

struct sweetgl_context *
sweetgl_context_create(struct sweetgl_display *d, int major, int minor, struct sweetgl_context *share)
{
	struct sweetgl_context *c = CALLOC_STRUCT(sweetgl_context);
	if (c == NULL) {
		return NULL;
	}
	c->display = d;

	/* no window-system buffers: color_format NONE gives the st no gl_config, i.e. surfaceless */
	struct st_context_attribs attribs = { 0 };
	attribs.profile = API_OPENGLES2;
	attribs.major = major;
	attribs.minor = minor;
	attribs.visual.color_format = PIPE_FORMAT_NONE;
	attribs.visual.depth_stencil_format = PIPE_FORMAT_NONE;
	attribs.visual.accum_format = PIPE_FORMAT_NONE;

	enum st_context_error error;
	c->st = st_api_create_context(&d->fscreen, &attribs, &error, share ? share->st : NULL);
	if (c->st == NULL) {
		FREE(c);
		return NULL;
	}
	c->st->frontend_context = c;
	return c;
}

bool
sweetgl_make_current(struct sweetgl_context *c)
{
	if (!st_api_make_current(c ? c->st : NULL, NULL, NULL) && c != NULL) {
		return false;
	}
	current = c;
	return true;
}

struct sweetgl_context *
sweetgl_get_current(void)
{
	return current;
}

void
sweetgl_flush(struct sweetgl_context *c)
{
	st_context_flush(c->st, 0, NULL, NULL, NULL);
}

void
sweetgl_context_destroy(struct sweetgl_context *c)
{
	if (c == NULL) {
		return;
	}
	st_context_flush(c->st, 0, NULL, NULL, NULL);
	if (current == c) {
		sweetgl_make_current(NULL);
	}
	/* binds c->st itself while it frees, then restores whatever was current */
	st_destroy_context(c->st);
	FREE(c);
}

void (*sweetgl_get_proc_address(const char *name))(void)
{
	return _mesa_glapi_get_proc_address(name);
}
