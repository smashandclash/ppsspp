// A small software renderer for 16-bit framebuffers. See snc_gfx.h.
#include "snc_gfx.h"

#include <math.h>
#include <string.h>

#include "qrcodegen.h"

#define R8(c) ((int)(((c) >> 16) & 255))
#define G8(c) ((int)(((c) >> 8) & 255))
#define B8(c) ((int)((c) & 255))

void sg_init(snc_surf *s, uint16_t *px, int w, int h, int stride) {
	s->px = px;
	s->w = w;
	s->h = h;
	s->stride = stride;
	sg_unclip(s);
}

void sg_clip(snc_surf *s, int x, int y, int w, int h) {
	s->cx0 = x < 0 ? 0 : x;
	s->cy0 = y < 0 ? 0 : y;
	s->cx1 = x + w > s->w ? s->w : x + w;
	s->cy1 = y + h > s->h ? s->h : y + h;
}

void sg_unclip(snc_surf *s) {
	s->cx0 = s->cy0 = 0;
	s->cx1 = s->w;
	s->cy1 = s->h;
}

static inline uint16_t mix(uint16_t d, int r, int g, int b, int a) {
	int dr = SNC_PR(d), dg = SNC_PG(d), db = SNC_PB(d);
	dr += ((r - dr) * a) >> 8;
	dg += ((g - dg) * a) >> 8;
	db += ((b - db) * a) >> 8;
	return SNC_PX(dr, dg, db);
}

// Clips a rectangle; returns 0 when nothing is left.
static int clip(const snc_surf *s, int *x, int *y, int *w, int *h) {
	if (*x < s->cx0) *w -= s->cx0 - *x, *x = s->cx0;
	if (*y < s->cy0) *h -= s->cy0 - *y, *y = s->cy0;
	if (*x + *w > s->cx1) *w = s->cx1 - *x;
	if (*y + *h > s->cy1) *h = s->cy1 - *y;
	return *w > 0 && *h > 0;
}

void sg_fill(snc_surf *s, int x, int y, int w, int h, uint32_t rgb) {
	if (!clip(s, &x, &y, &w, &h)) return;
	uint16_t c = SNC_PX(R8(rgb), G8(rgb), B8(rgb));
	for (int j = 0; j < h; j++) {
		uint16_t *p = s->px + (y + j) * s->stride + x;
		for (int i = 0; i < w; i++) p[i] = c;
	}
}

void sg_blend(snc_surf *s, int x, int y, int w, int h, uint32_t rgb, int a) {
	if (a >= 255) {
		sg_fill(s, x, y, w, h, rgb);
		return;
	}
	if (a <= 0 || !clip(s, &x, &y, &w, &h)) return;
	int r = R8(rgb), g = G8(rgb), b = B8(rgb);
	for (int j = 0; j < h; j++) {
		uint16_t *p = s->px + (y + j) * s->stride + x;
		for (int i = 0; i < w; i++) p[i] = mix(p[i], r, g, b, a);
	}
}

static void put(snc_surf *s, int x, int y, int r, int g, int b, int a) {
	if (x < s->cx0 || y < s->cy0 || x >= s->cx1 || y >= s->cy1 || a <= 0) return;
	uint16_t *p = s->px + y * s->stride + x;
	*p = a >= 255 ? SNC_PX(r, g, b) : mix(*p, r, g, b, a);
}

void sg_vgrad(snc_surf *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
	for (int j = 0; j < h; j++) {
		int t = h > 1 ? (j * 256) / (h - 1) : 0;
		int r = R8(top) + ((R8(bottom) - R8(top)) * t >> 8);
		int g = G8(top) + ((G8(bottom) - G8(top)) * t >> 8);
		int b = B8(top) + ((B8(bottom) - B8(top)) * t >> 8);
		// a touch of ordered dither keeps 16-bit gradients from banding
		for (int i = 0; i < w; i++) {
			int d = ((i ^ j) & 1) ? 2 : -2;
			int rr = r + d < 0 ? 0 : r + d > 255 ? 255 : r + d;
			int gg = g + d < 0 ? 0 : g + d > 255 ? 255 : g + d;
			int bb = b + d < 0 ? 0 : b + d > 255 ? 255 : b + d;
			put(s, x + i, y + j, rr, gg, bb, 255);
		}
	}
}

// Coverage (0..255) of pixel (px, py) by a rounded rect of size w x h, corner radius r.
static int cover(int px, int py, int w, int h, int r) {
	float fx = (float)px + 0.5f, fy = (float)py + 0.5f, cx, cy;
	if (fx < (float)r) cx = (float)r;
	else if (fx > (float)(w - r)) cx = (float)(w - r);
	else return 255;
	if (fy < (float)r) cy = (float)r;
	else if (fy > (float)(h - r)) cy = (float)(h - r);
	else return 255;
	float c = (float)r - sqrtf((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy)) + 0.5f;
	if (c >= 1.0f) return 255;
	if (c <= 0.0f) return 0;
	return (int)(c * 255.0f);
}

void sg_rrect(snc_surf *s, int x, int y, int w, int h, int r, uint32_t rgb, int a) {
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	if (r <= 0) {
		sg_blend(s, x, y, w, h, rgb, a);
		return;
	}
	int cr = R8(rgb), cg = G8(rgb), cb = B8(rgb);
	// the middle band and the straight parts in one go, then the corners pixel by pixel
	sg_blend(s, x, y + r, w, h - 2 * r, rgb, a);
	sg_blend(s, x + r, y, w - 2 * r, r, rgb, a);
	sg_blend(s, x + r, y + h - r, w - 2 * r, r, rgb, a);
	for (int j = 0; j < r; j++)
		for (int i = 0; i < r; i++) {
			int c = cover(i, j, w, h, r) * a / 255;
			put(s, x + i, y + j, cr, cg, cb, c);
			put(s, x + w - 1 - i, y + j, cr, cg, cb, c);
			put(s, x + i, y + h - 1 - j, cr, cg, cb, c);
			put(s, x + w - 1 - i, y + h - 1 - j, cr, cg, cb, c);
		}
}

void sg_rframe(snc_surf *s, int x, int y, int w, int h, int r, int t, uint32_t rgb, int a) {
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	int cr = R8(rgb), cg = G8(rgb), cb = B8(rgb);
	int ri = r - t < 0 ? 0 : r - t;
	for (int j = 0; j < h; j++) {
		int edge = j < t || j >= h - t;
		if (j >= r && j < h - r && !edge) {
			sg_blend(s, x, y + j, t, 1, rgb, a);
			sg_blend(s, x + w - t, y + j, t, 1, rgb, a);
			continue;
		}
		for (int i = 0; i < w; i++) {
			if (!edge && i >= t + r && i < w - t - r) continue;
			int outer = cover(i, j, w, h, r);
			int inner = (i >= t && i < w - t && j >= t && j < h - t) ? cover(i - t, j - t, w - 2 * t, h - 2 * t, ri) : 0;
			int c = (outer - inner) * a / 255;
			if (c > 0) put(s, x + i, y + j, cr, cg, cb, c);
		}
	}
}

void sg_frame(snc_surf *s, int x, int y, int w, int h, int t, uint32_t rgb) {
	sg_fill(s, x, y, w, t, rgb);
	sg_fill(s, x, y + h - t, w, t, rgb);
	sg_fill(s, x, y + t, t, h - 2 * t, rgb);
	sg_fill(s, x + w - t, y + t, t, h - 2 * t, rgb);
}

void sg_img(snc_surf *s, int x, int y, const uint16_t *img, int w, int h, int flags) {
	int sx = x, sy = y, cw = w, ch = h;
	if (!clip(s, &sx, &sy, &cw, &ch)) return;
	if (!flags) {  // the common case: rows straight across
		for (int j = 0; j < ch; j++) memcpy(s->px + (sy + j) * s->stride + sx, img + (sy - y + j) * w + (sx - x), (size_t)cw * 2);
		return;
	}
	for (int j = 0; j < ch; j++) {
		int iy = sy - y + j;
		uint16_t *p = s->px + (sy + j) * s->stride + sx;
		for (int i = 0; i < cw; i++) {
			int ix = sx - x + i;
			uint16_t v = (flags & SG_ROT180) ? img[(h - 1 - iy) * w + (w - 1 - ix)] : img[iy * w + ix];
			if (flags & SG_DIM) {
				int r = SNC_PR(v), g = SNC_PG(v), b = SNC_PB(v);
				int l = (r * 77 + g * 150 + b * 29) >> 8;  // grey, then darker: "can't play this now"
				v = SNC_PX((l * 3 + 40) >> 2, (l * 3 + 50) >> 2, (l * 3 + 70) >> 2);
			}
			if (flags & SG_GHOST) p[i] = mix(p[i], SNC_PR(v), SNC_PG(v), SNC_PB(v), 150);
			else p[i] = v;
		}
	}
}

void sg_img_alpha(snc_surf *s, int x, int y, const uint16_t *img, const uint8_t *a, int w, int h) {
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			int al = a[j * w + i];
			if (!al) continue;
			uint16_t v = img[j * w + i];
			put(s, x + i, y + j, SNC_PR(v), SNC_PG(v), SNC_PB(v), al);
		}
}

// Alpha-mapped pixels in one colour (glyphs, icons): solid pixels written straight,
// the clip worked out once per row.
static void mask(snc_surf *s, int x, int y, const uint8_t *a, int w, int h, int r, int g, int b) {
	uint16_t c = SNC_PX(r, g, b);
	int i0 = x < s->cx0 ? s->cx0 - x : 0, i1 = x + w > s->cx1 ? s->cx1 - x : w;
	for (int j = 0; j < h; j++) {
		int py = y + j;
		if (py < s->cy0 || py >= s->cy1) continue;
		const uint8_t *row = a + j * w;
		uint16_t *p = s->px + py * s->stride + x;
		for (int i = i0; i < i1; i++) {
			int al = row[i];
			if (al >= 255) p[i] = c;
			else if (al) p[i] = mix(p[i], r, g, b, al);
		}
	}
}

void sg_mask(snc_surf *s, int x, int y, const uint8_t *a, int w, int h, uint32_t rgb) {
	mask(s, x, y, a, w, h, R8(rgb), G8(rgb), B8(rgb));
}

/* ----------------------------------- text ------------------------------------ */

static unsigned next_cp(const char **sp) {
	const unsigned char *s = (const unsigned char *)*sp;
	unsigned c = *s++;
	if (c >= 0xF0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
		c = ((c & 7) << 18) | ((s[0] & 63u) << 12) | ((s[1] & 63u) << 6) | (s[2] & 63u);
		s += 3;
	} else if (c >= 0xE0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80) {
		c = ((c & 15) << 12) | ((s[0] & 63u) << 6) | (s[1] & 63u);
		s += 2;
	} else if (c >= 0xC0 && (s[0] & 0xC0) == 0x80) {
		c = ((c & 31) << 6) | (s[0] & 63u);
		s += 1;
	}
	*sp = (const char *)s;
	return c;
}

static const snc_glyph *glyph(const snc_font *f, unsigned cp) {
	int lo = 0, hi = f->n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (f->g[mid].cp == cp) return &f->g[mid];
		if (f->g[mid].cp < cp) lo = mid + 1;
		else hi = mid - 1;
	}
	return cp == '?' ? NULL : glyph(f, '?');
}

static int draw_glyph(snc_surf *s, const snc_font *f, const snc_glyph *g, int x, int y, int r, int gg, int b) {
	if (g->w && g->h) mask(s, x + g->xo, y + g->yo, f->alpha + g->off, g->w, g->h, r, gg, b);
	return g->adv;
}

static int text_n(snc_surf *s, const snc_font *f, int x, int y, const char *text, size_t n, uint32_t rgb) {
	int pen = x;
	const char *end = text + n;
	while (text < end && *text) {
		const snc_glyph *g = glyph(f, next_cp(&text));
		if (!g) continue;
		if (s) pen += draw_glyph(s, f, g, pen, y, R8(rgb), G8(rgb), B8(rgb));
		else pen += g->adv;
	}
	return pen - x;
}

int sg_text(snc_surf *s, const snc_font *f, int x, int y, const char *text, uint32_t rgb) {
	return text_n(s, f, x, y, text, strlen(text), rgb);
}

int sg_text_w(const snc_font *f, const char *text) {
	return text_n(NULL, f, 0, 0, text, strlen(text), 0);
}

int sg_text_c(snc_surf *s, const snc_font *f, int cx, int y, const char *text, uint32_t rgb) {
	int w = sg_text_w(f, text);
	return sg_text(s, f, cx - w / 2, y, text, rgb);
}

int sg_text_r(snc_surf *s, const snc_font *f, int rx, int y, const char *text, uint32_t rgb) {
	int w = sg_text_w(f, text);
	return sg_text(s, f, rx - w, y, text, rgb);
}

int sg_text_fit(snc_surf *s, const snc_font *f, int x, int y, int w, const char *text, uint32_t rgb) {
	if (sg_text_w(f, text) <= w) return sg_text(s, f, x, y, text, rgb);
	static const char ELLIPSIS[] = "\xE2\x80\xA6";
	int ew = sg_text_w(f, ELLIPSIS);
	const char *p = text;
	int pen = 0;
	while (*p) {
		const char *q = p;
		const snc_glyph *g = glyph(f, next_cp(&q));
		int adv = g ? g->adv : 0;
		if (pen + adv + ew > w) break;
		pen += adv;
		p = q;
	}
	int drawn = text_n(s, f, x, y, text, (size_t)(p - text), rgb);
	return drawn + sg_text(s, f, x + drawn, y, ELLIPSIS, rgb);
}

int sg_text_outline(snc_surf *s, const snc_font *f, int x, int y, const char *text, uint32_t fill, uint32_t outline, int d) {
	for (int dy = -d; dy <= d; dy++)
		for (int dx = -d; dx <= d; dx++)
			if ((dx || dy) && dx * dx + dy * dy <= d * d + 1) sg_text(s, f, x + dx, y + dy, text, outline);
	return sg_text(s, f, x, y, text, fill);
}

int sg_wrap(snc_surf *s, const snc_font *f, int x, int y, int w, int lh, const char *text, uint32_t rgb, int max_lines) {
	int lines = 0;
	const char *p = text;
	while (*p) {
		if (max_lines && lines >= max_lines) break;
		// the longest run of words that fits
		const char *line_end = p, *q = p;
		int width = 0;
		while (*q && *q != '\n') {
			const char *word = q;
			while (*q == ' ') q++;
			while (*q && *q != ' ' && *q != '\n') q++;
			int ww = text_n(NULL, f, 0, 0, p, (size_t)(q - p), 0);
			if (ww > w && line_end != p) break;
			if (ww > w) {  // one word longer than the line: break it
				q = word;
				while (*q && *q != ' ' && *q != '\n') {
					const char *r = q;
					next_cp(&r);
					if (text_n(NULL, f, 0, 0, p, (size_t)(r - p), 0) > w && q != p) break;
					q = r;
				}
				line_end = q;
				break;
			}
			line_end = q;
			width = ww;
		}
		(void)width;
		int last = max_lines && lines == max_lines - 1 && *line_end && *line_end != '\n';
		if (s) {
			if (last) {
				char tmp[256];
				size_t n = strlen(p);
				if (n > sizeof tmp - 1) n = sizeof tmp - 1;
				memcpy(tmp, p, n);
				tmp[n] = 0;
				char *nl = strchr(tmp, '\n');
				if (nl) *nl = 0;
				sg_text_fit(s, f, x, y + lines * lh, w, tmp, rgb);
			} else {
				text_n(s, f, x, y + lines * lh, p, (size_t)(line_end - p), rgb);
			}
		}
		lines++;
		p = line_end;
		while (*p == ' ') p++;
		if (*p == '\n') p++;
	}
	return lines;
}

/* ------------------------------------ QR ------------------------------------- */

int sg_qr(snc_surf *s, int x, int y, int scale, const char *text) {
	static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(20)];
	static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(20)];
	if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, 1, 20, qrcodegen_Mask_AUTO, true)) return 0;
	int n = qrcodegen_getSize(qr);
	int size = (n + 4) * scale;  // two modules of quiet zone each side
	if (!s) return size;
	sg_fill(s, x, y, size, size, 0xFFFFFF);
	for (int j = 0; j < n; j++)
		for (int i = 0; i < n; i++)
			if (qrcodegen_getModule(qr, i, j)) sg_fill(s, x + (i + 2) * scale, y + (j + 2) * scale, scale, scale, 0x0D1A4A);
	return size;
}
