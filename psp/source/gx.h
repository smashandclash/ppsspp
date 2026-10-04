// The PSP's GPU, for this game: 2D sprites, gradients, skewed candy slabs, text from
// the font atlases, glows, and a 3D camera for the board. All drawing happens between
// gx_begin() and gx_end(); positions are screen pixels unless a call says world units.
#ifndef GX_H
#define GX_H

#include <stdint.h>

#include "textures.h"

// Colours are 0xRRGGBB plus an alpha (0..255); the GPU wants 0xAABBGGRR.
static inline uint32_t gcol(uint32_t rgb, int a) {
	return ((uint32_t)a << 24) | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}

void gx_init(void);
void gx_begin(uint32_t clear_rgb);
void gx_end(void);  // waits for the GPU and the vertical blank, then shows the frame

/* 2D */
void gx_rect(float x, float y, float w, float h, uint32_t top, uint32_t bottom);  // gcol() colours
void gx_rrect(float x, float y, float w, float h, float r, uint32_t top, uint32_t bottom, float skew);
void gx_rframe(float x, float y, float w, float h, float r, float t, uint32_t col, float skew);
void gx_sprite(int tex, float sx, float sy, float sw, float sh, float x, float y, float w, float h, uint32_t col, int flipx);
void gx_sprite_rot(int tex, float sx, float sy, float sw, float sh, float cx, float cy, float w, float h, float angle, uint32_t col);
void gx_glow(float cx, float cy, float rx, float ry, uint32_t col);  // additive
void gx_rays(float cx, float cy, float radius, float angle, uint32_t col);

/* text: y is the top of the line; each returns the width */
float gx_text(const snc_tfont *f, float x, float y, const char *s, uint32_t col);
float gx_text_w(const snc_tfont *f, const char *s);
float gx_text_c(const snc_tfont *f, float cx, float y, const char *s, uint32_t col);
float gx_text_r(const snc_tfont *f, float rx, float y, const char *s, uint32_t col);
float gx_text_fit(const snc_tfont *f, float x, float y, float w, const char *s, uint32_t col);
float gx_text_shadow(const snc_tfont *f, float x, float y, const char *s, uint32_t col, uint32_t shadow);
float gx_text_outline(const snc_tfont *f, float x, float y, const char *s, uint32_t col, uint32_t outline);
int gx_wrap(const snc_tfont *f, float x, float y, float w, float lh, const char *s, uint32_t col, int max_lines);  // lines used; col 0 = measure

/* a QR code (modules of `scale` pixels with a quiet zone); returns its size, 0 if too long */
int gx_qr(float x, float y, int scale, const char *text);
int gx_qr_size(int scale, const char *text);

/* 3D: world units, y up, the player looks toward -z */
void gx_camera(float ex, float ey, float ez, float tx, float ty, float tz, float fov_deg);
int gx_project(float x, float y, float z, float *sx, float *sy);  // 0 if behind the camera
// A textured rectangle lying on the board (y = height): centre, size, turned by `yaw`
// (radians, about the vertical), its width scaled by `squash` (a card flipping over).
void gx_flat(int tex, float su, float sv, float sw, float sh, float cx, float y, float cz, float w, float d, float yaw, float squash,
             uint32_t col);
void gx_quad3d(int tex, float u0, float v0, float u1, float v1, const float corners[4][3], uint32_t col);  // tl, tr, br, bl

int gx_texture_w(int tex);
int gx_texture_h(int tex);

#endif
