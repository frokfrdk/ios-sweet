#pragma once
#include <stdint.h>

/*
 * Framebuffer = ABL's continuous-splash surface (reserved-memory cont_splash_region,
 * 0x9c000000 on both sweet and sunfish). The geometry below is the board default;
 * panel_detect() replaces it with what the MDP source pipe is actually scanning out.
 */
#define FB_DEF_BASE	0x9c000000UL
#define FB_DEF_W	1080
#ifdef BOARD_SUNFISH
#define FB_DEF_H	2340		/* Pixel 4a, sofef01 cmd-mode panel */
#else
#define FB_DEF_H	2400		/* sweet, DSC video-mode panel */
#endif
#define FB_MAX_BYTES	0x1700000	/* size of cont_splash_region */

#define FB_BASE		fb_base
#define FB_W		((int)fb_w)
#define FB_H		((int)fb_h)
#define FB_STRIDE	((int)fb_stride)	/* bytes per line */
#define FB_BPP		((int)fb_bpp)	/* bytes per pixel: 4 = a8r8g8b8, 3 = r8g8b8 */

extern uintptr_t fb_base;
extern uint32_t fb_w, fb_h, fb_stride, fb_bpp;

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
