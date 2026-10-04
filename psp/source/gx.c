// The PSP's GPU, for this game. See gu.h.
#include "gx.h"

#include <math.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <string.h>

#include "qrcodegen.h"

static unsigned int __attribute__((aligned(16))) list[512 * 1024];
static int bound = -2;  // the texture in use (-1: none)

typedef struct {
	float u, v;
	uint32_t c;
	float x, y, z;
} vtx;

#define VTX2D (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)
#define VTX3D (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

static ScePspFMatrix4 proj, view, ident;

/* ---------------------------------- frame ------------------------------------ */

static void *vram_rel(unsigned int size) {
	static unsigned int next = 0;
	void *r = (void *)next;
	next += size;
	return r;
}

void gx_init(void) {
	void *fb0 = vram_rel(512 * 272 * 4);
	void *fb1 = vram_rel(512 * 272 * 4);
	void *zb = vram_rel(512 * 272 * 2);
	sceGuInit();
	sceGuStart(GU_DIRECT, list);
	sceGuDrawBuffer(GU_PSM_8888, fb0, 512);
	sceGuDispBuffer(480, 272, fb1, 512);
	sceGuDepthBuffer(zb, 512);
	sceGuOffset(2048 - 240, 2048 - 136);
	sceGuViewport(2048, 2048, 480, 272);
	sceGuDepthRange(65535, 0);
	sceGuScissor(0, 0, 480, 272);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDepthFunc(GU_GEQUAL);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuFrontFace(GU_CW);
	sceGuShadeModel(GU_SMOOTH);
	sceGuDisable(GU_CULL_FACE);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
	sceGuFinish();
	sceGuSync(0, 0);
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
	memset(&ident, 0, sizeof ident);
	ident.x.x = ident.y.y = ident.z.z = ident.w.w = 1.0f;
}

void gx_begin(uint32_t clear_rgb) {
	sceGuStart(GU_DIRECT, list);
	sceGuClearColor(gcol(clear_rgb, 255));
	sceGuClearDepth(0);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
	bound = -2;
}

void gx_end(void) {
	sceGuFinish();
	sceGuSync(0, 0);
	sceDisplayWaitVblankStart();
	sceGuSwapBuffers();
}

/* -------------------------------- textures ----------------------------------- */

int gx_texture_w(int tex) {
	return snc_tex[tex].w;
}

int gx_texture_h(int tex) {
	return snc_tex[tex].h;
}

static void bind(int tex) {
	if (tex == bound) return;
	bound = tex;
	if (tex < 0) {
		sceGuDisable(GU_TEXTURE_2D);
		return;
	}
	sceGuEnable(GU_TEXTURE_2D);
	const snc_tex_t *t = &snc_tex[tex];
	sceGuClutLoad(256 / 8, snc_tex_data + t->pal);
	sceGuTexMode(GU_PSM_T8, 0, 0, 0);
	sceGuTexImage(0, t->w, t->h, t->w, snc_tex_data + t->data);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
}

static vtx *verts(int n) {
	return (vtx *)sceGuGetMemory((unsigned)(n * (int)sizeof(vtx)));
}

static void v2(vtx *v, float u, float tv, uint32_t c, float x, float y) {
	v->u = u, v->v = tv, v->c = c, v->x = x, v->y = y, v->z = 0;
}

static uint32_t lerp_col(uint32_t a, uint32_t b, float t) {
	uint32_t out = 0;
	for (int s = 0; s < 32; s += 8) {
		int ca = (int)((a >> s) & 255), cb = (int)((b >> s) & 255);
		out |= (uint32_t)(ca + (int)((float)(cb - ca) * t)) << s;
	}
	return out;
}

/* ----------------------------------- 2D -------------------------------------- */

void gx_rect(float x, float y, float w, float h, uint32_t top, uint32_t bottom) {
	bind(-1);
	vtx *v = verts(4);
	v2(&v[0], 0, 0, top, x, y);
	v2(&v[1], 0, 0, top, x + w, y);
	v2(&v[2], 0, 0, bottom, x, y + h);
	v2(&v[3], 0, 0, bottom, x + w, y + h);
	sceGuDrawArray(GU_TRIANGLE_STRIP, VTX2D, 4, 0, v);
}

// Rounded rectangles: nine pieces of the circle texture (the middle of it is solid),
// coloured top to bottom, optionally skewed like the web edition's slabs.
static void rrect(float x, float y, float w, float h, float r, uint32_t top, uint32_t bottom, float skew, float cy) {
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	bind(TEX_CIRCLE);
	const float xs[4] = {x, x + r, x + w - r, x + w};
	const float ys[4] = {y, y + r, y + h - r, y + h};
	const float us[4] = {0, 32, 32, 64};
	for (int row = 0; row < 3; row++) {
		vtx *v = verts(8);
		for (int k = 0; k < 4; k++) {
			for (int e = 0; e < 2; e++) {
				float yy = ys[row + e];
				float t = h > 0 ? (yy - y) / h : 0;
				float sx = (cy - yy) * skew;
				v2(&v[k * 2 + e], us[k], us[row + e], lerp_col(top, bottom, t), xs[k] + sx, yy);
			}
		}
		sceGuDrawArray(GU_TRIANGLE_STRIP, VTX2D, 8, 0, v);
	}
}

void gx_rrect(float x, float y, float w, float h, float r, uint32_t top, uint32_t bottom, float skew) {
	rrect(x, y, w, h, r, top, bottom, skew, y + h / 2);
}

void gx_rframe(float x, float y, float w, float h, float r, float t, uint32_t col, float skew) {
	// a ring: four bars skewed about the frame's own middle, so they meet
	float cy = y + h / 2;
	(void)r;
	rrect(x, y, w, t, t / 2, col, col, skew, cy);
	rrect(x, y + h - t, w, t, t / 2, col, col, skew, cy);
	rrect(x, y, t, h, t / 2, col, col, skew, cy);
	rrect(x + w - t, y, t, h, t / 2, col, col, skew, cy);
}

void gx_sprite(int tex, float sx, float sy, float sw, float sh, float x, float y, float w, float h, uint32_t col, int flipx) {
	bind(tex);
	vtx *v = verts(2);
	v2(&v[0], flipx ? sx + sw : sx, sy, col, x, y);
	v2(&v[1], flipx ? sx : sx + sw, sy + sh, col, x + w, y + h);
	sceGuDrawArray(GU_SPRITES, VTX2D, 2, 0, v);
}

void gx_sprite_rot(int tex, float sx, float sy, float sw, float sh, float cx, float cy, float w, float h, float angle, uint32_t col) {
	bind(tex);
	float c = cosf(angle), s = sinf(angle);
	const float px[4] = {-w / 2, w / 2, -w / 2, w / 2}, py[4] = {-h / 2, -h / 2, h / 2, h / 2};
	const float pu[4] = {sx, sx + sw, sx, sx + sw}, pv[4] = {sy, sy, sy + sh, sy + sh};
	vtx *v = verts(4);
	for (int i = 0; i < 4; i++) v2(&v[i], pu[i], pv[i], col, cx + px[i] * c - py[i] * s, cy + px[i] * s + py[i] * c);
	sceGuDrawArray(GU_TRIANGLE_STRIP, VTX2D, 4, 0, v);
}

void gx_glow(float cx, float cy, float rx, float ry, uint32_t col) {
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
	gx_sprite(TEX_GLOW, 0, 0, 64, 64, cx - rx, cy - ry, rx * 2, ry * 2, col, 0);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}

void gx_rays(float cx, float cy, float radius, float angle, uint32_t col) {
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
	gx_sprite_rot(TEX_RAYS, 0, 0, 128, 128, cx, cy, radius * 2, radius * 2, angle, col);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}

/* ----------------------------------- text ------------------------------------ */

static unsigned next_cp(const char **sp) {
	const unsigned char *s = (const unsigned char *)*sp;
	unsigned c = *s++;
	if (c >= 0xF0 && s[0] && s[1] && s[2]) {
		c = ((c & 7) << 18) | ((s[0] & 63u) << 12) | ((s[1] & 63u) << 6) | (s[2] & 63u);
		s += 3;
	} else if (c >= 0xE0 && s[0] && s[1]) {
		c = ((c & 15) << 12) | ((s[0] & 63u) << 6) | (s[1] & 63u);
		s += 2;
	} else if (c >= 0xC0 && s[0]) {
		c = ((c & 31) << 6) | (s[0] & 63u);
		s += 1;
	}
	*sp = (const char *)s;
	return c;
}

static const snc_tglyph *glyph(const snc_tfont *f, unsigned cp) {
	int lo = 0, hi = f->n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (f->g[mid].cp == cp) return &f->g[mid];
		if (f->g[mid].cp < cp) lo = mid + 1;
		else hi = mid - 1;
	}
	if (cp >= 'a' && cp <= 'z') return glyph(f, cp - 32);  // the display faces are capitals anyway
	return cp == '?' ? NULL : glyph(f, '?');
}

static float text_n(const snc_tfont *f, float x, float y, const char *s, size_t n, uint32_t col) {
	float pen = x;
	const char *end = s + n;
	int count = 0;
	for (const char *p = s; p < end && *p;) {
		next_cp(&p);
		count++;
	}
	vtx *v = (col && count) ? verts(count * 2) : NULL;
	int k = 0;
	if (v) bind(f->tex);
	while (s < end && *s) {
		const snc_tglyph *g = glyph(f, next_cp(&s));
		if (!g) continue;
		if (v && g->w) {
			float gx = floorf(pen + g->xo + 0.5f), gy = floorf(y + g->yo + 0.5f);
			v2(&v[k++], g->x, g->y, col, gx, gy);
			v2(&v[k++], g->x + g->w, g->y + g->h, col, gx + g->w, gy + g->h);
		}
		pen += g->adv;
	}
	if (v && k) sceGuDrawArray(GU_SPRITES, VTX2D, k, 0, v);
	return pen - x;
}

float gx_text(const snc_tfont *f, float x, float y, const char *s, uint32_t col) {
	return text_n(f, x, y, s, strlen(s), col);
}

float gx_text_w(const snc_tfont *f, const char *s) {
	return text_n(f, 0, 0, s, strlen(s), 0);
}

float gx_text_c(const snc_tfont *f, float cx, float y, const char *s, uint32_t col) {
	return gx_text(f, cx - gx_text_w(f, s) / 2, y, s, col);
}

float gx_text_r(const snc_tfont *f, float rx, float y, const char *s, uint32_t col) {
	return gx_text(f, rx - gx_text_w(f, s), y, s, col);
}

float gx_text_fit(const snc_tfont *f, float x, float y, float w, const char *s, uint32_t col) {
	if (gx_text_w(f, s) <= w) return gx_text(f, x, y, s, col);
	static const char ELL[] = "\xE2\x80\xA6";
	float ew = gx_text_w(f, ELL), pen = 0;
	const char *p = s;
	while (*p) {
		const char *q = p;
		const snc_tglyph *g = glyph(f, next_cp(&q));
		float adv = g ? g->adv : 0;
		if (pen + adv + ew > w) break;
		pen += adv;
		p = q;
	}
	float d = text_n(f, x, y, s, (size_t)(p - s), col);
	return d + gx_text(f, x + d, y, ELL, col);
}

float gx_text_shadow(const snc_tfont *f, float x, float y, const char *s, uint32_t col, uint32_t shadow) {
	gx_text(f, x, y + 2, s, shadow);
	return gx_text(f, x, y, s, col);
}

float gx_text_outline(const snc_tfont *f, float x, float y, const char *s, uint32_t col, uint32_t outline) {
	static const int d[8][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
	gx_text(f, x, y + 3, s, outline);  // the drop under it
	for (int i = 0; i < 8; i++) gx_text(f, x + d[i][0], y + d[i][1], s, outline);
	return gx_text(f, x, y, s, col);
}

int gx_wrap(const snc_tfont *f, float x, float y, float w, float lh, const char *text, uint32_t col, int max_lines) {
	int lines = 0;
	const char *p = text;
	while (*p) {
		if (max_lines && lines >= max_lines) break;
		const char *line_end = p, *q = p;
		while (*q && *q != '\n') {
			const char *word = q;
			while (*q == ' ') q++;
			while (*q && *q != ' ' && *q != '\n') q++;
			float ww = text_n(f, 0, 0, p, (size_t)(q - p), 0);
			if (ww > w && line_end != p) break;
			if (ww > w) {
				q = word;
				while (*q && *q != ' ' && *q != '\n') {
					const char *r = q;
					next_cp(&r);
					if (text_n(f, 0, 0, p, (size_t)(r - p), 0) > w && q != p) break;
					q = r;
				}
				line_end = q;
				break;
			}
			line_end = q;
		}
		int last = max_lines && lines == max_lines - 1 && *line_end && *line_end != '\n';
		if (col) {
			if (last) {
				char tmp[256];
				size_t n = strlen(p);
				if (n > sizeof tmp - 1) n = sizeof tmp - 1;
				memcpy(tmp, p, n);
				tmp[n] = 0;
				char *nl = strchr(tmp, '\n');
				if (nl) *nl = 0;
				gx_text_fit(f, x, y + lines * lh, w, tmp, col);
			} else {
				text_n(f, x, y + lines * lh, p, (size_t)(line_end - p), col);
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

static uint8_t qr_buf[qrcodegen_BUFFER_LEN_FOR_VERSION(20)], qr_tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(20)];
static char qr_text[512];
static int qr_ok;

static int encode(const char *text) {
	if (!strcmp(text, qr_text)) return qr_ok;
	strncpy(qr_text, text, sizeof qr_text - 1);
	qr_ok = qrcodegen_encodeText(text, qr_tmp, qr_buf, qrcodegen_Ecc_LOW, 1, 20, qrcodegen_Mask_AUTO, true);
	return qr_ok;
}

int gx_qr_size(int scale, const char *text) {
	return encode(text) ? (qrcodegen_getSize(qr_buf) + 4) * scale : 0;
}

int gx_qr(float x, float y, int scale, const char *text) {
	if (!encode(text)) return 0;
	int n = qrcodegen_getSize(qr_buf), size = (n + 4) * scale;
	gx_rect(x, y, size, size, gcol(0xFFFFFF, 255), gcol(0xFFFFFF, 255));
	bind(-1);
	int count = 0;
	for (int j = 0; j < n; j++)
		for (int i = 0; i < n; i++) count += qrcodegen_getModule(qr_buf, i, j);
	vtx *v = verts(count * 2);
	int k = 0;
	uint32_t ink = gcol(0x0D1A4A, 255);
	for (int j = 0; j < n; j++)
		for (int i = 0; i < n; i++)
			if (qrcodegen_getModule(qr_buf, i, j)) {
				v2(&v[k++], 0, 0, ink, x + (i + 2) * scale, y + (j + 2) * scale);
				v2(&v[k++], 0, 0, ink, x + (i + 3) * scale, y + (j + 3) * scale);
			}
	sceGuDrawArray(GU_SPRITES, VTX2D, k, 0, v);
	return size;
}

/* ----------------------------------- 3D -------------------------------------- */

static void mat_mul(ScePspFMatrix4 *out, const ScePspFMatrix4 *a, const ScePspFMatrix4 *b) {
	// out = a * b, column vectors (the GU's convention: x, y, z, w are columns)
	const float *A = &a->x.x, *B = &b->x.x;
	float R[16];
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) {
			float s = 0;
			for (int k = 0; k < 4; k++) s += A[k * 4 + r] * B[c * 4 + k];
			R[c * 4 + r] = s;
		}
	memcpy(&out->x.x, R, sizeof R);
}

void gx_camera(float ex, float ey, float ez, float tx, float ty, float tz, float fov_deg) {
	float f = 1.0f / tanf(fov_deg * 3.14159265f / 360.0f), aspect = 480.0f / 272.0f, zn = 0.5f, zf = 100.0f;
	memset(&proj, 0, sizeof proj);
	proj.x.x = f / aspect;
	proj.y.y = f;
	proj.z.z = (zf + zn) / (zn - zf);
	proj.z.w = -1.0f;
	proj.w.z = 2.0f * zf * zn / (zn - zf);
	float fx = tx - ex, fy = ty - ey, fz = tz - ez;
	float fl = sqrtf(fx * fx + fy * fy + fz * fz);
	fx /= fl, fy /= fl, fz /= fl;
	float sx = fy * 0 - fz * 1, sy = fz * 0 - fx * 0, sz = fx * 1 - fy * 0;  // side = forward x up(0,1,0)
	float sl = sqrtf(sx * sx + sy * sy + sz * sz);
	sx /= sl, sy /= sl, sz /= sl;
	float ux = sy * fz - sz * fy, uy = sz * fx - sx * fz, uz = sx * fy - sy * fx;
	memset(&view, 0, sizeof view);
	view.x.x = sx, view.y.x = sy, view.z.x = sz;
	view.x.y = ux, view.y.y = uy, view.z.y = uz;
	view.x.z = -fx, view.y.z = -fy, view.z.z = -fz;
	view.w.x = -(sx * ex + sy * ey + sz * ez);
	view.w.y = -(ux * ex + uy * ey + uz * ez);
	view.w.z = (fx * ex + fy * ey + fz * ez);
	view.w.w = 1.0f;
	sceGuSetMatrix(GU_PROJECTION, &proj);
	sceGuSetMatrix(GU_VIEW, &view);
	sceGuSetMatrix(GU_MODEL, &ident);
}

int gx_project(float x, float y, float z, float *sx, float *sy) {
	ScePspFMatrix4 pv;
	mat_mul(&pv, &proj, &view);
	const float *M = &pv.x.x;
	float cx = M[0] * x + M[4] * y + M[8] * z + M[12];
	float cy = M[1] * x + M[5] * y + M[9] * z + M[13];
	float cw = M[3] * x + M[7] * y + M[11] * z + M[15];
	if (cw <= 0.0001f) return 0;
	*sx = (cx / cw * 0.5f + 0.5f) * 480.0f;
	*sy = (1.0f - (cy / cw * 0.5f + 0.5f)) * 272.0f;
	return 1;
}

void gx_quad3d(int tex, float u0, float v0, float u1, float v1, const float c[4][3], uint32_t col) {
	bind(tex);
	float tw = (float)snc_tex[tex].w, th = (float)snc_tex[tex].h;
	sceGuTexScale(1.0f, 1.0f);
	sceGuTexOffset(0.0f, 0.0f);
	vtx *v = verts(4);
	const int order[4] = {0, 1, 3, 2};  // a strip: tl, tr, bl, br
	const float us[4] = {u0, u1, u1, u0}, vs[4] = {v0, v0, v1, v1};
	for (int i = 0; i < 4; i++) {
		int k = order[i];
		v[i].u = us[k] / tw, v[i].v = vs[k] / th, v[i].c = col;
		v[i].x = c[k][0], v[i].y = c[k][1], v[i].z = c[k][2];
	}
	sceGuDrawArray(GU_TRIANGLE_STRIP, VTX3D, 4, 0, v);
}

void gx_flat(int tex, float su, float sv, float sw, float sh, float cx, float y, float cz, float w, float d, float yaw, float squash,
             uint32_t col) {
	float c = cosf(yaw), s = sinf(yaw);
	float hw = w / 2 * squash, hd = d / 2;
	const float lx[4] = {-hw, hw, hw, -hw}, lz[4] = {-hd, -hd, hd, hd};  // tl, tr, br, bl (far edge first)
	float corners[4][3];
	for (int i = 0; i < 4; i++) {
		corners[i][0] = cx + lx[i] * c - lz[i] * s;
		corners[i][1] = y;
		corners[i][2] = cz + lx[i] * s + lz[i] * c;
	}
	gx_quad3d(tex, su, sv, su + sw, sv + sh, corners, col);
}
