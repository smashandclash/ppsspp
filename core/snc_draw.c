// Drawing the game's pieces in the Arena Pop look. See snc_draw.h.
#include "snc_draw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "snc_game.h"

/* ---------------------------------- surfaces ---------------------------------- */

// The world gradient, drawn once per screen size and copied after that.
void sd_world(snc_surf *s) {
	static struct { int w, h; uint16_t *px; } cache[2];
	int k = 0;
	while (k < 2 && cache[k].px && (cache[k].w != s->w || cache[k].h != s->h)) k++;
	if (k == 2) k = 1;
	if (!cache[k].px || cache[k].w != s->w || cache[k].h != s->h) {
		free(cache[k].px);
		cache[k].px = malloc((size_t)s->w * (size_t)s->h * 2);
		cache[k].w = s->w;
		cache[k].h = s->h;
		if (!cache[k].px) {
			sg_fill(s, 0, 0, s->w, s->h, C_WORLD_BOT);
			return;
		}
		snc_surf tmp;
		sg_init(&tmp, cache[k].px, s->w, s->h, s->w);
		sg_vgrad(&tmp, 0, 0, s->w, s->h, C_WORLD_TOP, C_WORLD_BOT);
		// a soft glow at the top, like the web edition's radial world
		for (int j = 0; j < s->h / 2; j++) sg_blend(&tmp, 0, j, s->w, 1, 0xFFFFFF, 26 - j * 52 / s->h);
	}
	for (int j = 0; j < s->h; j++) memcpy(s->px + j * s->stride, cache[k].px + j * s->w, (size_t)s->w * 2);
}

void sd_glass(snc_surf *s, int x, int y, int w, int h, int r, int alpha) {
	sg_rrect(s, x, y, w, h, r, C_INK, alpha);
}

void sd_panel(snc_surf *s, int x, int y, int w, int h, int r) {
	sg_rrect(s, x, y + 3, w, h - 3, r, C_SUGAR_EDGE, 255);  // the lip
	sg_rrect(s, x, y, w, h - 3, r, C_SUGAR2, 255);
	sg_rrect(s, x + 1, y + 1, w - 2, (h - 3) / 2, r - 1 > 0 ? r - 1 : 1, C_SUGAR, 255);
}

static void fill_colors(sd_fill fill, uint32_t *hi, uint32_t *base, uint32_t *lip, uint32_t *on) {
	switch (fill) {
	case FILL_SUN: *hi = C_SUN1, *base = C_SUN2, *lip = C_SUN_LIP, *on = C_INK; break;
	case FILL_MINT: *hi = C_MINT1, *base = C_MINT2, *lip = C_MINT_LIP, *on = C_INK; break;
	case FILL_CHERRY: *hi = C_CHERRY1, *base = C_CHERRY2, *lip = C_CHERRY_LIP, *on = C_SUGAR; break;
	case FILL_GRAPE: *hi = C_GRAPE1, *base = C_GRAPE2, *lip = C_GRAPE_LIP, *on = C_SUGAR; break;
	case FILL_SKY: *hi = C_SKY1, *base = C_SKY2, *lip = C_SKY_LIP, *on = C_INK; break;
	default: *hi = C_SUGAR, *base = 0xE6F0FF, *lip = C_SUGAR_EDGE, *on = C_INK; break;
	}
}

static void candy(snc_surf *s, int x, int y, int w, int h, sd_fill fill, int state, uint32_t *on) {
	uint32_t hi, base, lip;
	fill_colors(fill, &hi, &base, &lip, on);
	if (state & SD_OFF) {
		hi = C_SUGAR3, base = C_SUGAR3, lip = C_SUGAR_EDGE;
		*on = C_INK3;
	}
	int r = h / 4 < 6 ? h / 4 : 6;
	int lipd = h >= 28 ? 3 : 2;
	sg_rrect(s, x, y + lipd, w, h - lipd, r, lip, 255);  // the hard lip under it
	sg_rrect(s, x, y, w, h - lipd, r, base, 255);
	sg_rrect(s, x + 1, y + 1, w - 2, (h - lipd) / 2, r - 1 > 0 ? r - 1 : 1, hi, 255);
	sg_blend(s, x + 1, y + 1 + (h - lipd) / 2 - 2, w - 2, 2, base, 128);
	if (state & SD_FOCUS) sd_focus(s, x, y, w, h, r);
}

void sd_button(snc_surf *s, int x, int y, int w, int h, const snc_font *f, const char *label, sd_fill fill, int state) {
	uint32_t on;
	candy(s, x, y, w, h, fill, state, &on);
	int lipd = h >= 28 ? 3 : 2;
	int ty = y + (h - lipd - f->line) / 2;
	snc_surf clipped = *s;
	sg_clip(&clipped, x + 2, y, w - 4, h);
	if (sg_text_w(f, label) <= w - 6) sg_text_c(&clipped, f, x + w / 2, ty, label, on);
	else sg_text_fit(&clipped, f, x + 3, ty, w - 6, label, on);
}

void sd_button2(snc_surf *s, int x, int y, int w, int h, const snc_font *f, const char *label, const snc_font *fc, const char *caption,
                sd_fill fill, int state) {
	uint32_t on;
	candy(s, x, y, w, h, fill, state, &on);
	int lipd = h >= 28 ? 3 : 2;
	int total = f->line + fc->line - 2;
	int ty = y + (h - lipd - total) / 2;
	snc_surf clipped = *s;
	sg_clip(&clipped, x + 2, y, w - 4, h);
	sg_text_c(&clipped, f, x + w / 2, ty, label, on);
	uint32_t muted = (fill == FILL_CHERRY || fill == FILL_GRAPE) ? 0xF0E8FF : C_INK2;
	if (state & SD_OFF) muted = C_INK3;
	if (sg_text_w(fc, caption) <= w - 6) sg_text_c(&clipped, fc, x + w / 2, ty + f->line - 2, caption, muted);
	else sg_text_fit(&clipped, fc, x + 3, ty + f->line - 2, w - 6, caption, muted);
}

void sd_focus(snc_surf *s, int x, int y, int w, int h, int r) {
	sg_rframe(s, x - 3, y - 3, w + 6, h + 6, r + 3, 2, C_SUN2, 255);
	sg_rframe(s, x - 4, y - 4, w + 8, h + 8, r + 4, 1, C_INK, 200);
}

/* ------------------------------------ cards ------------------------------------ */

static const uint16_t *art_s(int i) {
	return i < 0 || i >= SNC_CARDS ? NULL : snc_art_s + (size_t)i * ART_S_W * ART_S_H;
}



// The printed side colours of the card art: top red, right yellow, bottom blue, left green.
static const uint32_t SIDE_COLOR[4] = {0xF0525A, 0xF5C431, 0x4D6CF5, 0x5DC24A};

// The height of the font's digits and how far below the line's top they start.
static void digit_box(const snc_font *f, int *top, int *h) {
	for (int i = 0; i < f->n; i++)
		if (f->g[i].cp == '0') {
			*top = f->g[i].yo;
			*h = f->g[i].h;
			return;
		}
	*top = 0;
	*h = f->ascent;
}

static int badge_h(const snc_font *f) {
	int top, h;
	digit_box(f, &top, &h);
	return h + 4;
}

static void badge(snc_surf *s, int cx, int cy, int v, uint32_t bg, const snc_font *f) {
	char t[12];
	snprintf(t, sizeof t, "%d", v);
	int top, dh;
	digit_box(f, &top, &dh);
	int tw = sg_text_w(f, t);
	int bh = dh + 4, bw = tw + 4;
	if (bw < bh - 1) bw = bh - 1;
	int x = cx - bw / 2, y = cy - bh / 2;
	sg_rrect(s, x, y, bw, bh, 2, C_INK_DEEP, 255);
	sg_rrect(s, x + 1, y + 1, bw - 2, bh - 2, 1, bg, 255);
	int ty = y + 2 - top;  // the digits' tops two pixels into the badge
	sg_text(s, f, x + (bw - tw) / 2 + 1, ty + 1, t, C_INK_DEEP);
	sg_text(s, f, x + (bw - tw) / 2, ty, t, 0xFFFFFF);
}

void sd_card(snc_surf *s, int x, int y, int card, const char *name, int owner, const int vals[4], int flags, const snc_font *digits) {
	const uint16_t *img = art_s(card);
	int w = ART_S_W, h = ART_S_H;
	int imgflags = ((flags & CARD_ROT) ? SG_ROT180 : 0) | ((flags & CARD_DIM) ? SG_DIM : 0) | ((flags & CARD_GHOST) ? SG_GHOST : 0);
	if (img) sg_img(s, x, y, img, w, h, imgflags);
	else {
		sg_rrect(s, x, y, w, h, 3, C_SUGAR, 255);
		sg_wrap(s, digits, x + 2, y + h / 2 - digits->line, w - 4, digits->line, name ? name : "?", C_INK, 2);
	}
	if (flags & CARD_FROZEN) {
		sg_blend(s, x, y, w, h, 0xBFEAFF, 120);
		sd_icon(s, x + (w - sd_icon_w("frozen")) / 2, y + (h - sd_icon_h("frozen")) / 2, "frozen", 0xFFFFFF);
	}
	// the owner's colour where the card's white edge was
	if (owner) {
		uint32_t col = owner == 'y' ? C_YOU : C_OPP;
		sg_rframe(s, x, y, w, h, 3, 2, col, (flags & CARD_GHOST) ? 150 : 255);
	}
	if (vals && !(flags & CARD_GHOST)) {
		int rot = (flags & CARD_ROT) != 0;
		int bh = badge_h(digits);
		int cx = x + w / 2, cy = y + h / 2;
		// screen top, right, bottom, left -> the printed square under it
		if (vals[0] >= 0) badge(s, cx, y + bh / 2 + 1, vals[0], SIDE_COLOR[rot ? 2 : 0], digits);
		if (vals[1] >= 0) badge(s, x + w - bh / 2 - 1, cy, vals[1], SIDE_COLOR[rot ? 3 : 1], digits);
		if (vals[2] >= 0) badge(s, cx, y + h - bh / 2 - 1, vals[2], SIDE_COLOR[rot ? 0 : 2], digits);
		if (vals[3] >= 0) badge(s, x + bh / 2 + 1, cy, vals[3], SIDE_COLOR[rot ? 1 : 3], digits);
	}
	if (flags & CARD_SELECTED) {
		sg_rframe(s, x - 2, y - 2, w + 4, h + 4, 4, 3, C_SUN2, 255);
	}
	if (flags & CARD_FLASH) sg_rframe(s, x - 2, y - 2, w + 4, h + 4, 4, 2, C_SUGAR, 230);  // changed with the last move
}

void sd_card_large(snc_surf *s, int x, int y, int i) {
	if (i < 0 || i >= SNC_CARDS) {
		sg_rrect(s, x, y, ART_L_W, ART_L_H, 4, C_SUGAR, 255);
		return;
	}
	sg_img(s, x, y, snc_art_l + (size_t)i * ART_L_W * ART_L_H, ART_L_W, ART_L_H, 0);
}

void sd_card_back(snc_surf *s, int x, int y) {
	sg_img(s, x, y, snc_back_s, BACK_S_W, BACK_S_H, 0);
}

void sd_logo_l(snc_surf *s, int x, int y) {
	sg_img_alpha(s, x, y, snc_logo_l, snc_logo_l_a, LOGO_L_W, LOGO_L_H);
}

void sd_logo_s(snc_surf *s, int x, int y) {
	sg_img_alpha(s, x, y, snc_logo_s, snc_logo_s_a, LOGO_S_W, LOGO_S_H);
}

/* ------------------------------------ icons ------------------------------------ */

static const struct {
	const char *name;
	const uint8_t *a;
	int w, h;
} ICONS[] = {
	{"knight", icon_knight, ICON_KNIGHT_W, ICON_KNIGHT_H}, {"bishop", icon_bishop, ICON_BISHOP_W, ICON_BISHOP_H},
	{"rook", icon_rook, ICON_ROOK_W, ICON_ROOK_H},       {"queen", icon_queen, ICON_QUEEN_W, ICON_QUEEN_H},
	{"frozen", icon_frozen, ICON_FROZEN_W, ICON_FROZEN_H},
};

static int icon_index(const char *name) {
	for (int i = 0; i < (int)(sizeof ICONS / sizeof ICONS[0]); i++)
		if (!strcmp(ICONS[i].name, name)) return i;
	return -1;
}

void sd_icon(snc_surf *s, int x, int y, const char *name, uint32_t rgb) {
	int i = icon_index(name);
	if (i >= 0) sg_mask(s, x, y, ICONS[i].a, ICONS[i].w, ICONS[i].h, rgb);
}

int sd_icon_w(const char *name) {
	int i = icon_index(name);
	return i >= 0 ? ICONS[i].w : 0;
}

int sd_icon_h(const char *name) {
	int i = icon_index(name);
	return i >= 0 ? ICONS[i].h : 0;
}


