// A small software renderer for 16-bit framebuffers: fills, blends, rounded rects,
// images, alpha masks, UTF-8 text and QR codes. Colours go in as 0xRRGGBB.
//
// The pixel format is the platform's: build with SNC_PIXEL_PSP for the PSP (5:6:5,
// red in the low bits); the default is the Nintendo DS (5:5:5 plus the opaque bit).
#ifndef SNC_GFX_H
#define SNC_GFX_H

#include <stdint.h>

#if defined(SNC_PIXEL_PSP)
#define SNC_PX(r, g, b) ((uint16_t)(((r) >> 3) | (((g) >> 2) << 5) | (((b) >> 3) << 11)))
#define SNC_PR(p) ((((p) & 31) << 3) | (((p) & 31) >> 2))
#define SNC_PG(p) (((((p) >> 5) & 63) << 2) | ((((p) >> 5) & 63) >> 4))
#define SNC_PB(p) (((((p) >> 11) & 31) << 3) | ((((p) >> 11) & 31) >> 2))
#else
#define SNC_PX(r, g, b) ((uint16_t)(0x8000 | ((r) >> 3) | (((g) >> 3) << 5) | (((b) >> 3) << 10)))
#define SNC_PR(p) ((((p) & 31) << 3) | (((p) & 31) >> 2))
#define SNC_PG(p) (((((p) >> 5) & 31) << 3) | ((((p) >> 5) & 31) >> 2))
#define SNC_PB(p) (((((p) >> 10) & 31) << 3) | ((((p) >> 10) & 31) >> 2))
#endif

typedef struct {
	uint16_t cp;         // the code point
	uint8_t w, h;        // the bitmap
	int8_t xo, yo;       // where it sits: from the pen, and from the top of the line
	uint8_t adv;         // how far the pen moves
	uint32_t off;        // into the font's alpha data
} snc_glyph;

typedef struct {
	const snc_glyph *g;  // sorted by code point
	int n;
	const uint8_t *alpha;
	int line;            // line height
	int ascent;
} snc_font;

typedef struct {
	uint16_t *px;
	int w, h, stride;
	int cx0, cy0, cx1, cy1;  // the clip rectangle (x1, y1 exclusive)
} snc_surf;

enum { SG_ROT180 = 1, SG_DIM = 2, SG_GHOST = 4 };

void sg_init(snc_surf *s, uint16_t *px, int w, int h, int stride);
void sg_clip(snc_surf *s, int x, int y, int w, int h);
void sg_unclip(snc_surf *s);

void sg_fill(snc_surf *s, int x, int y, int w, int h, uint32_t rgb);
void sg_blend(snc_surf *s, int x, int y, int w, int h, uint32_t rgb, int a);  // a: 0..255
void sg_vgrad(snc_surf *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void sg_rrect(snc_surf *s, int x, int y, int w, int h, int r, uint32_t rgb, int a);
void sg_rframe(snc_surf *s, int x, int y, int w, int h, int r, int t, uint32_t rgb, int a);
void sg_frame(snc_surf *s, int x, int y, int w, int h, int t, uint32_t rgb);

void sg_img(snc_surf *s, int x, int y, const uint16_t *img, int w, int h, int flags);
void sg_img_alpha(snc_surf *s, int x, int y, const uint16_t *img, const uint8_t *a, int w, int h);
void sg_mask(snc_surf *s, int x, int y, const uint8_t *a, int w, int h, uint32_t rgb);

// Text: y is the top of the line. Each returns the width drawn.
int sg_text(snc_surf *s, const snc_font *f, int x, int y, const char *text, uint32_t rgb);
int sg_text_w(const snc_font *f, const char *text);
int sg_text_c(snc_surf *s, const snc_font *f, int cx, int y, const char *text, uint32_t rgb);  // centred on cx
int sg_text_r(snc_surf *s, const snc_font *f, int rx, int y, const char *text, uint32_t rgb);  // right edge at rx
// Text cut to fit w (with an ellipsis).
int sg_text_fit(snc_surf *s, const snc_font *f, int x, int y, int w, const char *text, uint32_t rgb);
// Text with a d-pixel outline (titles on the blue world).
int sg_text_outline(snc_surf *s, const snc_font *f, int x, int y, const char *text, uint32_t fill, uint32_t outline, int d);
// Word-wrapped text in a column w wide, lh apart, at most max_lines (0 = any).
// Returns the lines it took. With s == NULL it only measures.
int sg_wrap(snc_surf *s, const snc_font *f, int x, int y, int w, int lh, const char *text, uint32_t rgb, int max_lines);

// A QR code of text: modules of `scale` pixels, with a quiet zone. Returns its size in
// pixels (0 if it does not fit). With s == NULL it only measures.
int sg_qr(snc_surf *s, int x, int y, int scale, const char *text);

#endif
