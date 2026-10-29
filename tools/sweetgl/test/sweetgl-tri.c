/*
 * sweetgl-tri (ios-sweet plan Stage 7.2): Mesa's freedreno gallium driver on
 * SweetGPU, offscreen, with no GL frontend. Through sweetdrm it creates the
 * screen, clears a 64x64 linear BGRA8 render target to blue and checks every
 * pixel, then draws one red triangle with Mesa's pass-through shaders and
 * checks that about half the pixels turned red, on the right side of the
 * diagonal. Each result line also reaches the kernel log (sweetdrm_say).
 *
 * FD_MESA_DEBUG defaults to nogmem,noubwc (sysmem rendering, no compression),
 * the simplest path; set it in the environment to try others.
 * Exit status 0 only if every check passes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xf86drm.h"
#include "sweetdrm.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/driconf.h"
#include "util/u_box.h"
#include "util/u_inlines.h"
#include "util/u_simple_shaders.h"
#include "util/xmlconfig.h"
#include "freedreno/drm/freedreno_drm_public.h"

#define W               64u
#define H               64u
#define BLUE            0xff0000ffu     /* BGRA8 read as a little-endian word: 0xAARRGGBB */
#define RED             0xffff0000u

/* the msm target's options (gallium/auxiliary/target-helpers/drm_helper.h); fd_screen_create reads them */
static const driOptionDescription fd_driconf[] = {
#include "freedreno/driinfo_freedreno.h"
};

static struct pipe_screen *screen;
static struct pipe_context *ctx;
static struct pipe_resource *rt;

/*
 * Map the render target and count its pixels: those equal to want_a in the
 * region where x + y < W - 1 (below the triangle's diagonal) and want_b
 * elsewhere, reporting the first few that are neither. Returns the bad count.
 */
static unsigned
check(const char *what, uint32_t want_a, uint32_t want_b, unsigned *count_a)
{
	struct pipe_box box;
	u_box_2d(0, 0, W, H, &box);
	struct pipe_transfer *xfer = NULL;
	const uint8_t *map = ctx->texture_map(ctx, rt, 0, PIPE_MAP_READ, &box, &xfer);
	if (map == NULL) {
		sweetdrm_say("7.2 %s: texture_map failed", what);
		return W * H;
	}
	unsigned bad = 0, a = 0;
	for (unsigned y = 0; y < H; ++y) {
		const uint32_t *row = (const uint32_t *)(map + (size_t)y * xfer->stride);
		for (unsigned x = 0; x < W; ++x) {
			uint32_t want = x + y < W - 1u ? want_a : want_b;
			/* pixels on the diagonal may go either way */
			bool edge = x + y == W - 1u || x + y == W - 2u || x + y == W;
			a += row[x] == want_a;
			if (row[x] == want || (edge && (row[x] == want_a || row[x] == want_b))) {
				continue;
			}
			if (bad++ < 4u) {
				sweetdrm_say("7.2 %s: pixel (%u,%u) 0x%08x, want 0x%08x", what, x, y, row[x], want);
			}
		}
	}
	sweetdrm_say("7.2 %s: %u of %u pixels wrong, %u are 0x%08x (stride %u)", what, bad, W * H, a, want_a,
	    xfer->stride);
	ctx->texture_unmap(ctx, xfer);
	if (count_a) {
		*count_a = a;
	}
	return bad;
}

static void
draw_triangle(void)
{
	struct pipe_blend_state blend = { 0 };
	blend.rt[0].colormask = PIPE_MASK_RGBA;
	ctx->bind_blend_state(ctx, ctx->create_blend_state(ctx, &blend));

	struct pipe_depth_stencil_alpha_state dsa = { 0 };
	ctx->bind_depth_stencil_alpha_state(ctx, ctx->create_depth_stencil_alpha_state(ctx, &dsa));

	struct pipe_rasterizer_state rs = { 0 };
	rs.cull_face = PIPE_FACE_NONE;
	rs.fill_front = rs.fill_back = PIPE_POLYGON_MODE_FILL;
	rs.half_pixel_center = 1;
	rs.bottom_edge_rule = 1;
	ctx->bind_rasterizer_state(ctx, ctx->create_rasterizer_state(ctx, &rs));

	struct pipe_viewport_state vp = {
		.scale = { W / 2.0f, H / 2.0f, 0.5f },
		.translate = { W / 2.0f, H / 2.0f, 0.5f },
		.swizzle_x = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X,
		.swizzle_y = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y,
		.swizzle_z = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z,
		.swizzle_w = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W,
	};
	ctx->set_viewport_states(ctx, 0, 1, &vp);

	const enum tgsi_semantic names[] = { TGSI_SEMANTIC_POSITION, TGSI_SEMANTIC_COLOR };
	const unsigned indexes[] = { 0, 0 };
	ctx->bind_vs_state(ctx, util_make_vertex_passthrough_shader(ctx, 2, names, indexes, false));
	ctx->bind_fs_state(ctx, util_make_fragment_passthrough_shader(ctx, TGSI_SEMANTIC_COLOR,
	    TGSI_INTERPOLATE_PERSPECTIVE, true));

	/* position, color; the triangle below the x + y = 0 diagonal in NDC */
	static const float verts[3][8] = {
		{ -1, -1, 0, 1, 1, 0, 0, 1 },
		{ 1, -1, 0, 1, 1, 0, 0, 1 },
		{ -1, 1, 0, 1, 1, 0, 0, 1 },
	};
	struct pipe_vertex_element ve[2] = { 0 };
	for (unsigned i = 0; i < 2; ++i) {
		ve[i].src_offset = i * 16;
		ve[i].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
		ve[i].src_stride = sizeof(verts[0]);
	}
	ctx->bind_vertex_elements_state(ctx, ctx->create_vertex_elements_state(ctx, 2, ve));

	struct pipe_resource *vbuf = pipe_buffer_create(screen, PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_DEFAULT,
	    sizeof(verts));
	pipe_buffer_write(ctx, vbuf, 0, sizeof(verts), verts);
	/* set_vertex_buffers takes the reference it is given */
	struct pipe_vertex_buffer vb = { 0 };
	pipe_resource_reference(&vb.buffer.resource, vbuf);
	ctx->set_vertex_buffers(ctx, 1, &vb);

	struct pipe_draw_info info = { .mode = MESA_PRIM_TRIANGLES, .instance_count = 1 };
	struct pipe_draw_start_count_bias draw = { .start = 0, .count = 3 };
	ctx->draw_vbo(ctx, &info, 0, NULL, &draw, 1);
	pipe_resource_reference(&vbuf, NULL);
}

int
main(void)
{
	setenv("FD_MESA_DEBUG", "nogmem,noubwc", 0);
	int fd = drmOpenWithType("msm", NULL, 0);
	if (fd < 0) {
		fprintf(stderr, "sweetgl-tri: no sweetdrm device (%d)\n", fd);
		return 1;
	}
	sweetdrm_say("7.2 sweetgl-tri: FD_MESA_DEBUG=%s", getenv("FD_MESA_DEBUG"));

	driOptionCache options_info = { 0 }, options = { 0 };
	driParseOptionInfo(&options_info, fd_driconf, ARRAY_SIZE(fd_driconf));
	driParseConfigFiles(&options, &options_info, &(driConfigFileParseParams){ .driverName = "msm" });
	struct pipe_screen_config config = { .options = &options, .options_info = &options_info };
	screen = fd_drm_screen_create_renderonly(fd, NULL, &config);
	if (screen == NULL) {
		sweetdrm_say("7.2: no screen");
		return 1;
	}
	sweetdrm_say("7.2: screen \"%s\" by %s", screen->get_name(screen), screen->get_vendor(screen));
	ctx = screen->context_create(screen, NULL, 0);
	if (ctx == NULL) {
		sweetdrm_say("7.2: no context");
		return 1;
	}

	struct pipe_resource tmpl = {
		.target = PIPE_TEXTURE_2D,
		.format = PIPE_FORMAT_B8G8R8A8_UNORM,
		.width0 = W,
		.height0 = H,
		.depth0 = 1,
		.array_size = 1,
		.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR,
		.usage = PIPE_USAGE_DEFAULT,
	};
	rt = screen->resource_create(screen, &tmpl);
	if (rt == NULL) {
		sweetdrm_say("7.2: no render target");
		return 1;
	}
	struct pipe_framebuffer_state fb = { .width = W, .height = H, .layers = 1, .nr_cbufs = 1 };
	fb.cbufs[0].format = tmpl.format;
	fb.cbufs[0].texture = rt;
	ctx->set_framebuffer_state(ctx, &fb);

	unsigned fails = 0, red = 0;
	const union pipe_color_union blue = { .f = { 0.0f, 0.0f, 1.0f, 1.0f } };
	ctx->clear(ctx, PIPE_CLEAR_COLOR0, 0, 0, NULL, &blue, 0.0, 0);
	ctx->flush(ctx, NULL, 0);
	fails += check("clear", BLUE, BLUE, NULL) != 0;

	draw_triangle();
	ctx->flush(ctx, NULL, 0);
	fails += check("triangle", RED, BLUE, &red) != 0;
	/* half the target, give or take the diagonal */
	if (red < W * H / 2u - W || red > W * H / 2u + W) {
		sweetdrm_say("7.2 triangle: %u red pixels, want about %u", red, W * H / 2u);
		++fails;
	}

	sweetdrm_say("7.2 sweetgl-tri: %s", fails ? "FAIL" : "pass");
	ctx->destroy(ctx);
	pipe_resource_reference(&rt, NULL);
	screen->destroy(screen);
	return fails ? 1 : 0;
}
