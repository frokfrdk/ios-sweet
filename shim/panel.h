#pragma once
#include <stdint.h>

struct panel_info {
	int sspp;		/* source pipe scanning the splash, -1 if none found */
	uint32_t src_addr, src_size, ystride, format;
	uint32_t pp_te;		/* bit n: ping-pong n has tear check on (cmd-mode path) */
	uint32_t pp_ar_before;	/* bit n: ABL had already enabled autorefresh on PP n */
	uint32_t ctl_kicked;	/* bit n: CTL n was active and got a CTL_START */
};

/*
 * Read the framebuffer ABL left behind from the MDP source pipes and set
 * fb_base/fb_w/fb_h/fb_stride/fb_bpp. Call first, with the MMU off.
 */
void panel_detect(struct panel_info *pi);

/*
 * Command-mode panels (sunfish sofef00/01) only show memory the MDP pushes on a
 * kickoff. Turn on ping-pong autorefresh (re-fetch on every TE) so plain writes
 * to the framebuffer reach the glass, then kick each active CTL once.
 * No-op on video-mode panels (no PP has tear check on).
 */
void panel_keep_refreshing(struct panel_info *pi);
