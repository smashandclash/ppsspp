// The PSP's screens, drawn by its GPU in the web edition's look (smashandclash.in):
// a sky lobby with a champion standing on the wooden board, a VS intro, the table in
// perspective with cards that drop, flip and slide, skewed candy slabs and sheets.
// Input goes through the shared targets (core/snc_ui.h): no touch, the D-pad moves a
// sun-yellow ring and cross presses.
#include "view3d.h"

#include <math.h>
#include <pspgu.h>
#include <stdio.h>
#include <string.h>

#include "assets.h"
#include "gx.h"
#include "snc_draw.h"
#include "snc_platform.h"
#include "textures.h"

#define SKEW 0.1405f  // tan(8°): the slabs lean like the web's
#define PI 3.14159265f

// Arena Pop · Blue
#define WORLD_TOP 0x0ABEFF
#define WORLD_BOT 0x016DCB
#define SKY_TOP 0xA9E6FF
#define SKY_BOT 0x2E9FEA

/* ------------------------------- state & anims -------------------------------- */

enum { A_NONE, A_DROP, A_FLIP, A_GONE, A_SLIDE };

typedef struct {
	int idx;      // card index (snc_cards), -1 = empty
	char owner;   // 'y' / 'o'
	char frozen;
} cell_t;

typedef struct {
	int type;
	uint32_t start;
	int from;      // slide: the cell it came from
	cell_t old;    // gone / flip: what was there
} anim_t;

static cell_t shown[15];
static anim_t anim[15];
static char shown_game[32];
static int shown_score[2];
static uint32_t score_bump[2];
static float hand_lift[8];
static uint32_t vs_start;    // the VS intro
static char vs_game[32];
static int spot_champ;       // the lobby's champion spotlight

static float clamp01(float t) {
	return t < 0 ? 0 : t > 1 ? 1 : t;
}

static float ease_out(float t) {
	t = clamp01(t);
	return 1 - (1 - t) * (1 - t) * (1 - t);
}

static float ease_back(float t) {
	t = clamp01(t);
	float c = 1.70158f;
	return 1 + (c + 1) * powf(t - 1, 3) + c * powf(t - 1, 2);
}

static uint32_t name_hash(const char *s) {
	uint32_t h = 2166136261u;
	for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
	return h;
}

// Champions: a character card id (1..46), its VS render and its avatar.
static int champ_tex(int champ) {
	return TEX_CHAMP0 + (champ - 1) * 2;
}

static int avatar_tex(int champ) {
	return TEX_CHAMP0 + (champ - 1) * 2 + 1;
}

static int my_champ(const snc_client *c) {
	if (c->rec.champ >= 1 && c->rec.champ <= 46) return c->rec.champ;
	return (int)(name_hash(c->name) % 46) + 1;
}

static int opp_champ(const snc_client *c) {
	const snc_game *g = &c->game;
	int other = g->seat == 'B' ? 0 : 1;
	uint32_t h = g->kinds[other] == 'h' ? name_hash(g->id) : name_hash(g->players[other]);
	int ch = (int)(h % 46) + 1;
	if (ch == my_champ(c)) ch = ch % 46 + 1;
	return ch;
}

/* ---------------------------------- pieces ------------------------------------ */

static uint32_t W(uint32_t rgb) {
	return gcol(rgb, 255);
}

static void sky(void) {
	gx_rect(0, 0, 480, 272, W(SKY_TOP), W(SKY_BOT));
	gx_glow(400, 20, 260, 140, gcol(0xFFFFFF, 110));
	for (int i = 0; i < 4; i++) {  // a few soft clouds
		float x = 30 + i * 130 + (float)((i * 37) % 23), y = 236 + (i % 2) * 14;
		gx_glow(x, y, 70, 26, gcol(0xFFFFFF, 120));
	}
}

static void world(uint32_t now) {
	gx_rect(0, 0, 480, 272, W(WORLD_TOP), W(WORLD_BOT));
	gx_rays(240, 130, 360, (float)(now % 120000) / 120000.0f * 2 * PI, gcol(0xFFFFFF, 14));
	gx_glow(240, 40, 300, 120, gcol(0xFFFFFF, 40));
}

static void glass(float x, float y, float w, float h, float r, int a) {
	gx_rrect(x, y, w, h, r, gcol(C_INK, a), gcol(C_INK, a), 0);
}

static void panel(float x, float y, float w, float h, float r) {
	gx_rrect(x, y + 3, w, h, r, W(C_SUGAR_EDGE), W(C_SUGAR_EDGE), 0);
	gx_rrect(x, y, w, h, r, W(C_SUGAR), W(C_SUGAR2), 0);
}

static void fill_cols(sd_fill fill, uint32_t *hi, uint32_t *base, uint32_t *lip, uint32_t *on) {
	switch (fill) {
	case FILL_SUN: *hi = C_SUN1, *base = C_SUN2, *lip = C_SUN_LIP, *on = C_INK; break;
	case FILL_MINT: *hi = C_MINT1, *base = C_MINT2, *lip = C_MINT_LIP, *on = C_INK; break;
	case FILL_CHERRY: *hi = C_CHERRY1, *base = C_CHERRY2, *lip = C_CHERRY_LIP, *on = C_SUGAR; break;
	case FILL_GRAPE: *hi = C_GRAPE1, *base = C_GRAPE2, *lip = C_GRAPE_LIP, *on = C_SUGAR; break;
	case FILL_SKY: *hi = C_SKY1, *base = C_SKY2, *lip = C_SKY_LIP, *on = C_SUGAR; break;
	default: *hi = C_SUGAR, *base = 0xE6F0FF, *lip = C_SUGAR_EDGE, *on = C_INK; break;
	}
}

static void focus_ring(float x, float y, float w, float h, float r, float skew, uint32_t now) {
	float p = 0.5f + 0.5f * sinf((float)(now % 1200) / 1200.0f * 2 * PI);
	gx_glow(x + w / 2, y + h / 2, w * 0.7f, h * 0.9f, gcol(C_SUN1, (int)(60 + 50 * p)));
	gx_rframe(x - 3, y - 3, w + 6, h + 6, r + 3, 3, W(C_SUN2), skew);
}

// A face-button glyph: 'x' cross, 'o' circle, 't' triangle, 's' square, 'S' START.
static float glyph(char g, float x, float y, float size) {
	int tex = g == 'x' ? TEX_GLYPH_CROSS : g == 'o' ? TEX_GLYPH_CIRCLEG : g == 't' ? TEX_GLYPH_TRIANGLE : g == 's' ? TEX_GLYPH_SQUARE : -1;
	if (tex >= 0) {
		gx_sprite(tex, 0, 0, 32, 32, x, y, size, size, W(0xFFFFFF), 0);
		return size + 3;
	}
	if (g == 'S' || g == 'L' || g == 'R') {
		const char *t = g == 'S' ? "START" : g == 'L' ? "L" : "R";
		float w = gx_text_w(&tfont_small, t) + 8;
		gx_rrect(x, y + 1, w, size - 2, (size - 2) / 2, gcol(C_INK_DEEP, 230), gcol(C_INK_DEEP, 230), 0);
		gx_text(&tfont_small, x + 4, y + (size - tfont_small.line) / 2, t, W(C_SUGAR));
		return w + 3;
	}
	return 0;
}

// The candy slab: a lip under it, a gradient body, a gloss, the label; skewed.
static void slab(view_t *v, float x, float y, float w, float h, sd_fill fill, const char *label, const char *sub, char g, int id, int arg,
                 int off, const snc_tfont *f, uint32_t now) {
	uint32_t hi, base, lip, on;
	fill_cols(fill, &hi, &base, &lip, &on);
	if (off) hi = C_SUGAR3, base = C_SUGAR3, lip = C_SUGAR_EDGE, on = C_INK3;
	float r = h * 0.28f;
	float l = h >= 30 ? 4 : 3;
	gx_rrect(x, y + l, w, h - l, r, W(lip), W(lip), SKEW);
	gx_rrect(x, y, w, h - l, r, W(hi), W(base), SKEW);
	gx_rrect(x + 3, y + 2, w - 6, (h - l) * 0.38f, r * 0.7f, gcol(0xFFFFFF, 70), gcol(0xFFFFFF, 0), SKEW);
	float tw = gx_text_w(f, label) + (g ? 19 : 0);
	float lines = sub ? f->line + tfont_small.line - 2 : f->line;
	float ty = y + (h - l - lines) / 2;
	float tx = x + (w - tw) / 2;
	if (g) tx += glyph(g, tx, ty + (f->line - 16) / 2, 16);
	uint32_t shadow = (fill == FILL_SUN || fill == FILL_SUGAR || fill == FILL_MINT || off) ? gcol(0xFFFFFF, 0) : gcol(C_INK_DEEP, 120);
	gx_text_shadow(f, tx, ty, label, W(on), shadow);
	if (sub) gx_text_c(&tfont_small, x + w / 2, ty + f->line - 2, sub, gcol(on, 210));
	if (v && !off) {
		view_add(v, (int)x, (int)y, (int)w, (int)h, id, arg);
		if (view_focused(v, id, arg)) focus_ring(x, y, w, h - l, r, SKEW, now);
	}
}

static void ribbon(float x, float y, float w, float h, const char *title, uint32_t fill_hi, uint32_t fill, uint32_t lip) {
	gx_rrect(x, y + 3, w, h, 6, W(lip), W(lip), SKEW);
	gx_rrect(x, y, w, h, 6, W(fill_hi), W(fill), SKEW);
	gx_text_outline(&tfont_label_l, x + (w - gx_text_w(&tfont_label_l, title)) / 2, y + (h - tfont_label_l.line) / 2, title, W(C_SUGAR),
	                W(C_INK));
}

static void sheet(float x, float y, float w, float h, const char *title, uint32_t hi, uint32_t base, uint32_t lip) {
	gx_rect(0, 0, 480, 272, gcol(C_INK_DEEP, 90), gcol(C_INK_DEEP, 130));
	panel(x, y, w, h, 12);
	ribbon(x + 14, y - 12, gx_text_w(&tfont_label_l, title) + 40, 28, title, hi, base, lip);
}

static void toast(const snc_client *c, float y) {
	const char *n = snc_client_notice(c);
	const char *text = n ? n : c->busy[0] ? c->busy : NULL;
	if (!text) return;
	float w = 300;
	int lines = gx_wrap(&tfont_small, 0, 0, w - 24, tfont_small.line, text, 0, 3);
	float h = lines * tfont_small.line + 14;
	glass(240 - w / 2, y, w, h, 10, 235);
	gx_rframe(240 - w / 2, y, w, h, 10, 2, W(n && c->notice_bad ? C_GUM2 : C_SUN2), 0);
	gx_wrap(&tfont_small, 240 - w / 2 + 12, y + 7, w - 24, tfont_small.line, text, W(n && c->notice_bad ? 0xFF8CC8 : C_SUN1), 3);
}

/* ---------------------------------- the board ---------------------------------- */

#define BW 6.0f
#define BK (BW / 512.0f)  // world units per board texel
#define BD (BOARD_TEX_H * BK)

static float tile_w(void) {
	return BOARD_TILE_W * BK;
}

static float tile_d(void) {
	return BOARD_TILE_H * BK;
}

// The middle of a screen slot (col 0..4 from the left, row 0..2 from the far side).
static void slot_xz(float ox, float oz, int col, int row, float *x, float *z) {
	*x = ox + (BOARD_BORDER + col * (BOARD_TILE_W + BOARD_GAP) + BOARD_TILE_W / 2 - 256) * BK;
	*z = oz + (BOARD_BORDER + row * (BOARD_TILE_H + BOARD_GAP) + BOARD_TILE_H / 2 - BOARD_TEX_H / 2.0f) * BK;
}

static void draw_board_wood(float ox, float oz, float yaw) {
	// a soft shadow, the board's edge, then the board
	gx_flat(TEX_GLOW, 0, 0, 64, 64, ox, -0.02f, oz + 0.25f, BW * 1.25f, BD * 1.3f, 0, 1, gcol(C_INK_DEEP, 120));
	gx_flat(TEX_CIRCLE, 32, 32, 1, 1, ox, -0.01f, oz + 0.08f, BW * 1.01f, BD * 1.02f, yaw, 1, W(0xB0844C));
	gx_flat(TEX_BOARD, 0, 0, 512, BOARD_TEX_H, ox, 0, oz, BW, BD, yaw, 1, W(0xFFFFFF));
}

static void card_tex_uv(int idx, int *tex, float *su, float *sv, float *sw, float *sh) {
	*tex = idx >= 0 && idx < SNC_CARDS ? TEX_CARD0 + idx : TEX_BACK;
	*su = (128 - CARD_TW) / 2.0f, *sv = 0, *sw = CARD_TW, *sh = CARD_TH;
}

static void draw_card3d(int idx, float x, float y, float z, float scale, float yaw, float squash, int owner, uint32_t tint) {
	int tex;
	float su, sv, sw, sh;
	card_tex_uv(idx, &tex, &su, &sv, &sw, &sh);
	float w = tile_w() * 0.94f * scale, d = tile_d() * 0.94f * scale;
	if (owner) {  // the owner's colour as a rim under the card
		uint32_t col = owner == 'y' ? C_YOU : C_OPP;
		gx_flat(TEX_CIRCLE, 32, 32, 1, 1, x, y + 0.005f, z, w * 1.07f, d * 1.05f, yaw, squash, W(col));
	}
	if (y > 0.05f) gx_flat(TEX_TILEGLOW, 0, 0, 64, 64, x + y * 0.3f, 0.004f, z + y * 0.4f, w * 1.2f, d * 1.2f, yaw, 1, gcol(C_INK_DEEP, 90));
	gx_flat(tex, su, sv, sw, sh, x, y + 0.01f, z, w, d, yaw, squash, tint);
}

/* ----------------------------------- the lobby ---------------------------------- */

static void draw_lobby(view_t *v, snc_client *c, uint32_t now) {
	sky();
	gx_sprite(TEX_LOGO, 0, 0, 256, 128, 6, 4, 112, 56, W(0xFFFFFF), 0);
	// the champion spotlight: your champion on the board, like the web lobby
	if (!spot_champ) spot_champ = my_champ(c);
	gx_camera(0, 6.4f, 11.0f, 0, 0.3f, 0, 34);
	float bx = -3.5f;
	draw_board_wood(bx, 0.6f, 0.12f);
	float sx, sy;
	gx_project(bx + 0.2f, 0, 0.5f, &sx, &sy);
	float bob = sinf((float)(now % 3000) / 3000.0f * 2 * PI) * 2;
	gx_glow(sx, sy + 6, 70, 18, gcol(C_INK_DEEP, 90));
	gx_sprite(champ_tex(spot_champ), 0, 0, 256, 256, sx - 78, sy - 150 + bob, 156, 156, W(0xFFFFFF), 0);
	int ci = snc_card_index(snc_cards[spot_champ - 1].name);
	int ctex;
	float su, sv, sw, sh;
	card_tex_uv(spot_champ - 1, &ctex, &su, &sv, &sw, &sh);
	(void)ci;
	gx_sprite_rot(ctex, su, sv, sw, sh, sx + 92, sy - 74 - bob, 47, 64, 0.16f, W(0xFFFFFF));
	// the spotlight pill: L and R change it
	glass(sx - 82, 222, 164, 34, 10, 230);
	gx_text_c(&tfont_small, sx, 225, "CHAMPION SPOTLIGHT", W(C_SKY1));
	gx_text_c(&tfont_label, sx, 238, snc_cards[spot_champ - 1].name, W(C_SUGAR));
	glyph('L', sx - 78, 232, 16);
	glyph('R', sx + 64, 232, 16);

	// you, top right
	char line[64];
	snprintf(line, sizeof line, "%s", c->name);
	float pw = gx_text_w(&tfont_label, line) + 70;
	if (pw < 130) pw = 130;
	panel(472 - pw, 6, pw, 34, 17);
	gx_sprite(avatar_tex(my_champ(c)), 0, 0, 64, 64, 474 - pw, 6, 34, 34, W(0xFFFFFF), 0);
	gx_text_fit(&tfont_label, 474 - pw + 40, 8, pw - 48, line, W(C_INK));
	snprintf(line, sizeof line, "Rating %d \xC2\xB7 %d won", c->rec.rating, c->rec.won);
	gx_text_fit(&tfont_small, 474 - pw + 40, 24, pw - 48, line, W(C_INK2));

	// the right column: the web lobby's stack
	float x = 280, w = 188;
	slab(v, x, 52, w, 30, FILL_GRAPE, "INVITE A FRIEND", NULL, 0, H_INVITE, 0, 0, &tfont_label, now);
	slab(v, x, 88, w, 30, FILL_SKY, "PLAY BY CODE", NULL, 0, H_CODE, 0, 0, &tfont_label, now);
	// the ruleset switch: a segmented trough
	glass(x, 124, w, 30, 9, 255);
	int mut = c->rec.ruleset != 'c';
	float half = (w - 8) / 2;
	gx_rrect(x + 4 + (mut ? 0 : half), 128, half, 22, 7, W(mut ? C_GRAPE1 : C_SKY1), W(mut ? C_GRAPE2 : C_SKY2), 0);
	gx_text_c(&tfont_label, x + 4 + half / 2, 130, "MUTATORS", W(mut ? C_SUGAR : C_SUGAR3));
	gx_text_c(&tfont_label, x + 4 + half * 1.5f, 130, "CLASSIC", W(!mut ? C_SUGAR : C_SUGAR3));
	view_add(v, (int)x, 124, (int)w, 30, H_RULESET, 0);
	if (view_focused(v, H_RULESET, 0)) focus_ring(x, 124, w, 30, 9, 0, now);
	float hw = (w - 8) / 2;
	slab(v, x, 160, hw, 28, FILL_SUGAR, "QUICK MATCH", NULL, 0, H_QUICK, 0, 0, &tfont_small, now);
	slab(v, x + hw + 8, 160, hw, 28, FILL_SUGAR, "HOW TO PLAY", NULL, 0, H_HOW, 0, 0, &tfont_small, now);
	slab(v, x - 6, 196, w + 6, 54, FILL_SUN, "PLAY", "New game \xC2\xB7 at your level", 0, H_NEW, 0, 0, &tfont_title, now);
	toast(c, 222);
}

/* ------------------------------------ VS intro ---------------------------------- */

static int draw_vs(snc_client *c, uint32_t now) {
	float t = (float)(now - vs_start) / 1000.0f;
	if (t > 2.4f) return 0;
	const snc_game *g = &c->game;
	char opp[48];
	snc_opponent_name(g, opp, sizeof opp);
	float in = ease_out(t / 0.45f);
	// the two halves, split on a slant, sliding in
	gx_rect(0, 0, 480, 272, W(0x0A8FE0), W(0x016DCB));
	float split = 240 + (1 - in) * 300;
	gx_rect(split + 20, 0, 480, 272, W(0xFF9A62), W(0xF4581F));  // the orange side
	gx_rays(120, 136, 300, t * 0.3f, gcol(0xFFFFFF, 26));
	gx_rays(380, 136, 300, -t * 0.3f, gcol(0xFFFFFF, 26));
	float slide = (1 - ease_out((t - 0.15f) / 0.5f)) * 260;
	gx_sprite(champ_tex(my_champ(c)), 0, 0, 256, 256, -10 - slide, 30, 230, 230, W(0xFFFFFF), 1);
	gx_sprite(champ_tex(opp_champ(c)), 0, 0, 256, 256, 260 + slide, 30, 230, 230, W(0xFFFFFF), 0);
	float pop = ease_back((t - 0.45f) / 0.35f);
	if (t > 0.45f) {
		float s = 0.3f + 0.7f * pop;
		float vw = gx_text_w(&tfont_huge, "VS") * s;
		(void)vw;
		gx_glow(240, 120, 90 * s, 60 * s, gcol(0xFFFFFF, 120));
		gx_text_outline(&tfont_huge, 240 - gx_text_w(&tfont_huge, "VS") / 2, 96, "VS", W(C_SUN1), W(C_INK));
	}
	// the names, on plates
	float py = 222 + (1 - in) * 60;
	gx_rrect(16, py, 200, 34, 10, W(C_YOU_HI), W(C_YOU), SKEW);
	gx_text_fit(&tfont_label_l, 28, py + 6, 180, c->name, W(C_SUGAR));
	gx_rrect(264, py, 200, 34, 10, W(C_OPP_HI), W(C_OPP), SKEW);
	gx_text_fit(&tfont_label_l, 276, py + 6, 180, opp, W(C_SUGAR));
	gx_text_c(&tfont_small, 240, 252, "Cross: skip", gcol(0xFFFFFF, 200));
	return 1;
}

/* ------------------------------------ the table --------------------------------- */

static void track_board(const snc_client *c, uint32_t now) {
	const snc_game *g = &c->game;
	if (strcmp(shown_game, g->id)) {  // a new game: no animations for what was dealt
		snprintf(shown_game, sizeof shown_game, "%s", g->id);
		for (int i = 0; i < 15; i++) {
			shown[i].idx = g->board[i].card[0] ? sd_tile_card(g, i) : -1;
			shown[i].owner = g->board[i].owner;
			shown[i].frozen = g->board[i].frozen;
			anim[i].type = A_NONE;
		}
		shown_score[0] = g->score_you, shown_score[1] = g->score_opp;
		return;
	}
	cell_t now_c[15];
	for (int i = 0; i < 15; i++) {
		now_c[i].idx = g->board[i].card[0] ? sd_tile_card(g, i) : -1;
		now_c[i].owner = g->board[i].owner;
		now_c[i].frozen = g->board[i].frozen;
	}
	int stagger = 0;
	for (int i = 0; i < 15; i++) {
		if (now_c[i].idx == shown[i].idx && now_c[i].owner == shown[i].owner) continue;
		anim_t *a = &anim[i];
		a->start = now;
		a->old = shown[i];
		if (now_c[i].idx >= 0 && shown[i].idx < 0) {
			// arrived: from another tile (a hop, a recruit) or from a hand
			int from = -1;
			for (int k = 0; k < 15; k++)
				if (k != i && shown[k].idx == now_c[i].idx && now_c[k].idx != shown[k].idx) from = k;
			a->type = from >= 0 ? A_SLIDE : A_DROP;
			a->from = from;
		} else if (now_c[i].idx < 0) {
			a->type = A_GONE;
		} else if (now_c[i].idx == shown[i].idx) {
			a->type = A_FLIP;
			a->start = now + 160 + (uint32_t)(stagger++ * 70);  // after the card that captured lands
		} else {
			a->type = A_DROP;  // overrun: a new card on top
		}
	}
	memcpy(shown, now_c, sizeof shown);
	for (int k = 0; k < 2; k++) {
		int s = k ? g->score_opp : g->score_you;
		if (s != shown_score[k]) score_bump[k] = now, shown_score[k] = s;
	}
}

static int pulse_alpha(uint32_t now, int lo, int hi) {
	float p = 0.5f + 0.5f * sinf((float)(now % 1100) / 1100.0f * 2 * PI);
	return lo + (int)((float)(hi - lo) * p);
}

static void draw_table_board(view_t *v, const snc_client *c, uint32_t now, int interactive) {
	const snc_game *g = &c->game;
	float ox = 0, oz = 0;
	float yaw_board = g->seat == 'B' ? PI : 0;
	gx_camera(0, 7.4f, 6.3f, 0, 0, 0.3f, 40);
	draw_board_wood(ox, oz, yaw_board);
	uint16_t lit = interactive && !c->busy[0] ? snc_targets(g, &c->pick) : 0;
	int mutators = g->ruleset != 'c';
	float tw = tile_w(), td = tile_d();
	// the tiles' extras under the cards: chess tiles, power glows, overrun zones
	for (int cell = 0; cell < 15; cell++) {
		int col, row;
		view_cell_slot(g, cell, &col, &row);
		float x, z;
		slot_xz(ox, oz, col, row, &x, &z);
		const char *piece = view_chess_at(g, cell);
		int pw = view_power_at(g, cell);
		if (piece && shown[cell].idx < 0) {
			int tex = !strcmp(piece, "knight") ? TEX_CHESS_KNIGHT : !strcmp(piece, "bishop") ? TEX_CHESS_BISHOP : !strcmp(piece, "rook") ? TEX_CHESS_ROOK : TEX_CHESS_QUEEN;
			gx_flat(tex, 0, 0, 64, 64, x, 0.006f, z, tw * 0.92f, td * 0.92f, 0, 1, W(0xFFFFFF));
		}
		if (pw >= 0) {
			uint32_t pc = sd_power_color(g->power[pw].color);
			gx_flat(TEX_TILEGLOW, 0, 0, 64, 64, x, 0.007f, z, tw * 1.25f, td * 1.2f, 0, 1, gcol(pc, pulse_alpha(now, 120, 220)));
		}
		if (mutators && view_overrun_at(g, cell) && shown[cell].idx < 0)
			gx_flat(TEX_TILEFRAME, 0, 0, 64, 64, x, 0.006f, z, tw * 0.9f, td * 0.9f, 0, 1, gcol(C_GRAPE1, 150));
	}
	// the cards, with their animations
	for (int cell = 0; cell < 15; cell++) {
		int col, row;
		view_cell_slot(g, cell, &col, &row);
		float x, z;
		slot_xz(ox, oz, col, row, &x, &z);
		anim_t *a = &anim[cell];
		float t = (float)((int32_t)(now - a->start)) / 1000.0f;
		cell_t cc = shown[cell];
		if (cc.idx < 0 && c->pending_cell == cell) {  // your card, on its way
			int pi = snc_card_index(c->pending_card);
			draw_card3d(pi, x, 0.25f, z, 1.05f, 0, 1, 'y', gcol(0xFFFFFF, 190));
			continue;
		}
		if (a->type == A_GONE && t < 0.3f) {
			float k = 1 - ease_out(t / 0.3f);
			draw_card3d(a->old.idx, x, 0.3f * (1 - k), z, 0.6f + 0.4f * k, a->old.owner == 'o' ? PI : 0, 1, a->old.owner, gcol(0xFFFFFF, (int)(255 * k)));
			continue;
		}
		if (cc.idx < 0) continue;
		float y = 0, scale = 1, squash = 1;
		char owner = cc.owner;
		if (a->type == A_DROP && t < 0.32f) {
			float k = ease_back(t / 0.32f);
			y = 0.9f * (1 - k);
			scale = 1.25f - 0.25f * k;
		} else if (a->type == A_SLIDE && t < 0.34f && a->from >= 0) {
			int fc, fr;
			view_cell_slot(g, a->from, &fc, &fr);
			float fx, fz;
			slot_xz(ox, oz, fc, fr, &fx, &fz);
			float k = ease_out(t / 0.34f);
			x = fx + (x - fx) * k, z = fz + (z - fz) * k;
			y = sinf(k * PI) * 0.45f;
		} else if (a->type == A_FLIP && t < 0.36f) {
			if (t < 0) {
				owner = a->old.owner;
			} else {
				float k = t / 0.36f;
				squash = fabsf(cosf(k * PI));
				y = sinf(k * PI) * 0.35f;
				if (k < 0.5f) owner = a->old.owner;
			}
		}
		draw_card3d(cc.idx, x, y, z, scale, owner == 'o' ? PI : 0, squash, owner, W(0xFFFFFF));
		if (cc.frozen) {
			gx_flat(TEX_TILEGLOW, 0, 0, 64, 64, x, y + 0.02f, z, tw, td, 0, 1, gcol(0xBFEAFF, 150));
			gx_flat(TEX_FROZEN, 0, 0, 64, 64, x, y + 0.03f, z, tw * 0.5f, tw * 0.5f, 0, 1, gcol(0xFFFFFF, 230));
		}
		if (c->flash & (1u << cell) && (int32_t)(now - a->start) < 1600)
			gx_flat(TEX_TILEFRAME, 0, 0, 64, 64, x, 0.03f, z, tw * 1.02f, td * 1.02f, 0, 1, gcol(0xFFFFFF, 200));
	}
	// green tiles, the focus, and the targets the D-pad moves between
	for (int cell = 0; cell < 15; cell++) {
		int col, row;
		view_cell_slot(g, cell, &col, &row);
		float x, z;
		slot_xz(ox, oz, col, row, &x, &z);
		int chosen = 0;
		for (int k = 0; k < c->pick.path_n; k++)
			if (c->pick.path[k] == cell) chosen = 1;
		if (lit & (1u << cell)) {
			gx_flat(TEX_TILEGLOW, 0, 0, 64, 64, x, 0.035f, z, tw * 1.1f, td * 1.06f, 0, 1, gcol(C_MINT2, pulse_alpha(now, 110, 210)));
			gx_flat(TEX_TILEFRAME, 0, 0, 64, 64, x, 0.04f, z, tw * 0.98f, td * 0.98f, 0, 1, W(C_MINT2));
		} else if (chosen) {
			gx_flat(TEX_TILEGLOW, 0, 0, 64, 64, x, 0.035f, z, tw, td, 0, 1, gcol(C_MINT2, 220));
		}
		if (!interactive) continue;
		float x0, y0, x1, y1, xa, ya, xb, yb;
		gx_project(x - tw / 2, 0, z - td / 2, &x0, &y0);
		gx_project(x + tw / 2, 0, z - td / 2, &x1, &y1);
		gx_project(x - tw / 2, 0, z + td / 2, &xa, &ya);
		gx_project(x + tw / 2, 0, z + td / 2, &xb, &yb);
		float l = fminf(x0, xa), r = fmaxf(x1, xb), t = fminf(y0, y1), b = fmaxf(ya, yb);
		view_add(v, (int)l, (int)t, (int)(r - l), (int)(b - t), H_CELL, cell);
		if (view_focused(v, H_CELL, cell)) {
			gx_flat(TEX_TILEGLOW, 0, 0, 64, 64, x, 0.05f, z, tw * 1.25f, td * 1.18f, 0, 1, gcol(C_SUN1, pulse_alpha(now, 90, 170)));
			gx_flat(TEX_TILEFRAME, 0, 0, 64, 64, x, 0.06f, z, tw * 1.06f, td * 1.04f, 0, 1, W(C_SUN2));
		}
	}
	// what sits on top of the cards, facing you: power boosts and chess pieces under cards
	for (int cell = 0; cell < 15; cell++) {
		int col, row;
		view_cell_slot(g, cell, &col, &row);
		float x, z, sx, sy;
		slot_xz(ox, oz, col, row, &x, &z);
		int pw = view_power_at(g, cell);
		if (pw >= 0) {
			gx_project(x + tw * 0.28f, 0.05f, z - td * 0.32f, &sx, &sy);
			uint32_t pc = sd_power_color(g->power[pw].color);
			gx_rrect(sx - 11, sy - 9, 22, 18, 9, W(0xFFFFFF), W(0xFFFFFF), 0);
			gx_rrect(sx - 9, sy - 7, 18, 14, 7, W(pc), W(pc), 0);
			char b[4];
			snprintf(b, sizeof b, "+%d", g->power[pw].boost);
			gx_text_c(&tfont_small, sx, sy - 7, b, W(C_SUGAR));
		}
		const char *piece = view_chess_at(g, cell);
		if (piece && shown[cell].idx >= 0) {
			int tex = !strcmp(piece, "knight") ? TEX_CHESS_KNIGHT : !strcmp(piece, "bishop") ? TEX_CHESS_BISHOP : !strcmp(piece, "rook") ? TEX_CHESS_ROOK : TEX_CHESS_QUEEN;
			gx_project(x - tw * 0.3f, 0.05f, z - td * 0.34f, &sx, &sy);
			gx_sprite(tex, 0, 0, 64, 64, sx - 9, sy - 9, 18, 18, W(0xFFFFFF), 0);
		}
	}
}

// Your hand: a fan of cards across the bottom.
static void draw_hand(view_t *v, const snc_client *c, uint32_t now) {
	const snc_game *g = &c->game;
	int n = g->hand_n > 5 ? 5 : g->hand_n;
	float cw = 58, ch = 79, gap = 50;
	for (int i = 0; i < n; i++) {
		int sel = c->pick.selected == i;
		int foc = view_focused(v, H_HAND, i);
		float target = sel ? 18 : foc ? 8 : 0;
		hand_lift[i] += (target - hand_lift[i]) * 0.3f;
		float off = (float)i - (n - 1) / 2.0f;
		float cx = 240 + off * gap, cy = 238 + off * off * 2.2f - hand_lift[i];
		float ang = off * 0.07f;
		int dim = g->your_turn && !g->has_hop && !snc_playable(g, i);
		int idx = sd_hand_card(&g->hand[i]);
		int tex;
		float su, sv, sw, sh;
		card_tex_uv(idx, &tex, &su, &sv, &sw, &sh);
		if (sel) gx_glow(cx, cy, 52, 62, gcol(C_SUN1, 170));
		gx_sprite_rot(TEX_TILEGLOW, 0, 0, 64, 64, cx + 3, cy + 5, cw + 10, ch + 10, ang, gcol(C_INK_DEEP, 110));
		gx_sprite_rot(tex, su, sv, sw, sh, cx, cy, cw, ch, ang, dim ? W(0x8A93A8) : W(0xFFFFFF));
		if (foc && !sel) {
			gx_sprite_rot(TEX_TILEFRAME, 0, 0, 64, 64, cx, cy, cw + 8, ch + 8, ang, W(C_SUN2));
		}
		if (sel) gx_sprite_rot(TEX_TILEFRAME, 0, 0, 64, 64, cx, cy, cw + 8, ch + 8, ang, W(C_SUN1));
		view_add(v, (int)(cx - cw / 2), (int)(cy - ch / 2), (int)cw, (int)ch, H_HAND, i);
	}
}

static void plate(const snc_client *c, int k, uint32_t now) {
	const snc_game *g = &c->game;
	char opp[48];
	snc_opponent_name(g, opp, sizeof opp);
	int score = k ? g->score_opp : g->score_you;
	int champ = k ? opp_champ(c) : my_champ(c);
	float bump = 1 + 0.25f * (1 - ease_out((float)(now - score_bump[k]) / 300.0f));
	int moving = c->busy[0] != 0;
	int turn = g->status == SNC_ACTIVE && (k ? (!g->your_turn || moving) : (g->your_turn && !moving));
	float x = k ? 480 - 8 - 86 : 8;
	uint32_t hi = k ? C_OPP_HI : C_YOU_HI, base = k ? C_OPP : C_YOU, lip = k ? C_OPP_LIP : C_YOU_LIP;
	float ax = k ? x + 50 : x, bx = k ? x : x + 40;
	if (turn) gx_glow(ax + 18, 24, 34, 30, gcol(C_SUN1, pulse_alpha(now, 90, 180)));
	gx_rrect(ax - 2, 4, 40, 40, 20, W(C_SUGAR), W(C_SUGAR), 0);
	gx_sprite(avatar_tex(champ), 0, 0, 64, 64, ax, 6, 36, 36, W(0xFFFFFF), 0);
	gx_rrect(bx, 9, 44, 34, 9, W(lip), W(lip), 0);
	gx_rrect(bx, 6, 44, 34, 9, W(hi), W(base), 0);
	char t[8];
	snprintf(t, sizeof t, "%d", score);
	float s = bump;
	(void)s;
	gx_text_outline(&tfont_title, bx + 22 - gx_text_w(&tfont_title, t) / 2, 9, t, W(C_SUGAR), W(lip));
	const char *name = k ? opp : "You";
	float nw = gx_text_w(&tfont_small, name) + 12;
	if (nw > 96) nw = 96;
	float nx = k ? 480 - 8 - nw : 8;
	glass(nx, 46, nw, 16, 8, 200);
	gx_text_fit(&tfont_small, nx + 6, 47, nw - 12, name, W(C_SUGAR));
}

static void inspector(const snc_client *c, view_t *v, uint32_t now) {
	const snc_game *g = &c->game;
	int idx = -1;
	char caption[48] = "";
	if (v->focus_id == H_HAND && v->focus_arg >= 0 && v->focus_arg < g->hand_n) {
		idx = sd_hand_card(&g->hand[v->focus_arg]);
		snprintf(caption, sizeof caption, "Your card");
	} else if (v->focus_id == H_CELL && v->focus_arg >= 0 && v->focus_arg < 15 && g->board[(int)v->focus_arg].card[0]) {
		idx = sd_tile_card(g, v->focus_arg);
		char cell[3];
		snc_cell_name(v->focus_arg, cell);
		snprintf(caption, sizeof caption, "On %s", cell);
	} else if (g->last_move[0]) {
		idx = sd_move_card(g, g->last_move);
		snprintf(caption, sizeof caption, "Last: %s", g->last_move);
	}
	if (idx < 0) return;
	float wob = sinf((float)(now % 4000) / 4000.0f * 2 * PI) * 0.02f;
	int tex;
	float su, sv, sw, sh;
	card_tex_uv(idx, &tex, &su, &sv, &sw, &sh);
	gx_sprite_rot(TEX_TILEGLOW, 0, 0, 64, 64, 56, 136, 92, 122, -0.1f + wob, gcol(C_INK_DEEP, 120));
	gx_sprite_rot(tex, su, sv, sw, sh, 52, 130, 84, 114, -0.1f + wob, W(0xFFFFFF));
	glass(6, 192, 96, 30, 8, 215);
	gx_text_fit(&tfont_small, 12, 193, 84, caption, W(C_SKY1));
	gx_text_fit(&tfont_label, 12, 205, 84, snc_cards[idx].name, W(C_SUGAR));
}

static void draw_game(view_t *v, snc_client *c, uint32_t now) {
	const snc_game *g = &c->game;
	if (g->status == SNC_ACTIVE && strcmp(vs_game, g->id) && g->move_count < 2) {
		snprintf(vs_game, sizeof vs_game, "%s", g->id);
		vs_start = now;
	}
	if (!strcmp(vs_game, g->id) && draw_vs(c, now)) {
		view_add(v, 0, 0, 480, 272, H_NONE, 0);
		return;
	}
	track_board(c, now);
	world(now);
	int over = snc_client_over(c);
	if (over && v->show_replay && g->replay_url[0]) {
		sheet(40, 26, 400, 236, "REPLAY", C_SKY1, C_SKY2, C_SKY_LIP);
		int scale = 2, size = gx_qr_size(scale, g->replay_url);
		gx_qr(56, 44 + (200 - size) / 2.0f, scale, g->replay_url);
		gx_wrap(&tfont_body, 56 + size + 16, 60, 400 - size - 50, tfont_body.line, "Scan it with a phone to watch this game again, move by move.",
		        W(C_INK), 5);
		slab(v, 56 + size + 16, 200, 120, 32, FILL_SUGAR, "BACK", NULL, 'o', H_REPLAY, 0, 0, &tfont_label, now);
		return;
	}
	draw_table_board(v, c, now, !over);
	plate(c, 0, now);
	plate(c, 1, now);
	// the middle of the top: the board filling up, whose turn it is
	int filled = 0;
	for (int i = 0; i < 15; i++) filled += g->board[i].card[0] != 0;
	glass(170, 4, 140, 12, 6, 180);
	for (int i = 0; i < 15; i++) gx_rrect(173 + i * 9, 6, 7, 8, 3, gcol(i < filled ? C_SUN1 : C_SUGAR, i < filled ? 255 : 60), gcol(i < filled ? C_SUN2 : C_SUGAR, i < filled ? 255 : 60), 0);
	snc_status_t st;
	snc_client_status(c, &st);
	char opp[48];
	snc_opponent_name(g, opp, sizeof opp);
	if (!over) {
		int mine = g->your_turn && !c->busy[0];
		char pill[64];
		if (mine) snprintf(pill, sizeof pill, "YOUR TURN");
		else snprintf(pill, sizeof pill, "%s", opp);
		float pw = gx_text_w(&tfont_label, pill) + (mine ? 30 : 46);
		if (pw > 190) pw = 190;
		if (mine) gx_rrect(240 - pw / 2, 19, pw, 22, 8, W(C_SUN1), W(C_SUN2), SKEW);
		else glass(240 - pw / 2, 19, pw, 22, 8, 220);
		gx_text_fit(&tfont_label, 240 - pw / 2 + 14, 21, pw - (mine ? 28 : 40), pill, W(mine ? C_INK : C_SUGAR));
		if (!mine)
			for (int d = 0; d < 3; d++) {  // thinking dots
				float b = sinf((float)(now % 900) / 900.0f * 2 * PI - d * 0.9f) * 2;
				gx_rrect(240 + pw / 2 - 26 + d * 7, 28 - b, 5, 5, 2.5f, W(C_SUN1), W(C_SUN1), 0);
			}
		// what to do now
		const char *n = snc_client_notice(c);
		const char *line = n ? n : st.main;
		float lw = gx_text_w(&tfont_small, line) + 20;
		if (lw > 300) lw = 300;
		glass(240 - lw / 2, 44, lw, 17, 8, 200);
		gx_text_fit(&tfont_small, 240 - lw / 2 + 10, 45, lw - 20, line, W(n && c->notice_bad ? 0xFF8CC8 : n ? C_SUN1 : C_SUGAR));
		// the other side's face-down hand
		int nb = c->opp_hand;
		for (int i = 0; i < nb; i++) {
			float off = i - (nb - 1) / 2.0f;
			gx_sprite_rot(TEX_BACK, (128 - CARD_TW) / 2.0f, 0, CARD_TW, CARD_TH, 430 + off * 9, 92 + off * off * 0.8f, 22, 30, off * 0.12f, W(0xFFFFFF));
		}
		inspector(c, v, now);
		draw_hand(v, c, now);
		// the actions, stacked on the right
		if (st.action[0]) slab(v, 392, 160, 82, 30, FILL_MINT, st.action_id == ACT_STAY ? "STAY" : "PLAY", NULL, 's', H_ACTION, 0, c->busy[0] != 0, &tfont_label, now);
		slab(v, 392, 196, 82, 24, FILL_SUGAR, "RULES", NULL, 't', H_HOW, 0, 0, &tfont_small, now);
		slab(v, 392, 226, 82, 24, FILL_CHERRY, "RESIGN", NULL, 0, H_RESIGN, 0, c->busy[0] != 0, &tfont_small, now);
		char dk[24];
		snprintf(dk, sizeof dk, "Deck %d", g->draw_pile);
		gx_text_c(&tfont_small, 433, 140, dk, W(C_SUGAR));
		if (st.detail[0] && !n) {
			glass(6, 228, 120, 40, 8, 200);
			gx_wrap(&tfont_small, 12, 230, 108, tfont_small.line - 1, st.detail, W(C_SUGAR3), 3);
		}
		return;
	}
	// game over: the result over the table
	gx_rect(0, 0, 480, 272, gcol(C_INK_DEEP, 120), gcol(C_INK_DEEP, 170));
	int won = g->status == SNC_FINISHED && g->winner == g->seat && !c->resigned;
	const char *title = g->status == SNC_ABANDONED ? "CALLED OFF" : won ? "VICTORY!" : g->winner == 'd' ? "DRAW" : "DEFEAT";
	uint32_t hi = won ? C_SUN1 : C_CHERRY1, base = won ? C_SUN2 : C_CHERRY2, lip = won ? C_SUN_LIP : C_CHERRY_LIP;
	if (won) gx_rays(240, 70, 300, (float)(now % 20000) / 20000.0f * 2 * PI, gcol(0xFFFFFF, 40));
	float tw = gx_text_w(&tfont_huge, title) + 60;
	gx_rrect(240 - tw / 2, 40 + 5, tw, 58, 12, W(lip), W(lip), SKEW);
	gx_rrect(240 - tw / 2, 40, tw, 58, 12, W(hi), W(base), SKEW);
	gx_text_outline(&tfont_huge, 240 - gx_text_w(&tfont_huge, title) / 2, 44, title, W(C_SUGAR), W(lip));
	panel(110, 108, 260, 92, 12);
	gx_text_c(&tfont_label_l, 240, 112, st.main, W(C_INK));
	if (c->review.ok) {
		int mine = g->seat == 'B' ? 1 : 0;
		for (int k = 0; k < 2; k++) {
			int acc = c->review.accuracy[k ? 1 - mine : mine];
			float y = 140 + k * 18;
			gx_text_fit(&tfont_small, 124, y, 66, k ? opp : "You", W(C_INK2));
			gx_rrect(196, y + 3, 120, 10, 5, W(C_SUGAR3), W(C_SUGAR3), 0);
			gx_rrect(196, y + 3, 120 * acc / 100.0f, 10, 5, W(k ? C_OPP_HI : C_YOU_HI), W(k ? C_OPP : C_YOU), 0);
			char a[8];
			snprintf(a, sizeof a, "%d%%", acc);
			gx_text(&tfont_small, 322, y, a, W(C_INK));
		}
	} else {
		gx_text_c(&tfont_small, 240, 146, "Reviewing the game...", W(C_INK3));
	}
	if (c->rated) {
		char r[48];
		snprintf(r, sizeof r, "Rating %d \xE2\x86\x92 %d", c->rating_before, c->rec.rating);
		gx_text_c(&tfont_small, 240, 180, r, W(c->rec.rating >= c->rating_before ? C_MINT_LIP : C_CHERRY_LIP));
	}
	int again_off = c->busy[0] != 0 || c->mode == 'j' || c->mode == 'c';
	slab(v, 96, 214, 120, 38, FILL_SUN, "AGAIN", NULL, 'x', H_AGAIN, 0, again_off, &tfont_label_l, now);
	slab(v, 224, 214, 80, 38, FILL_SUGAR, "LOBBY", NULL, 0, H_LOBBY, 0, c->busy[0] != 0, &tfont_label, now);
	if (g->replay_url[0]) slab(v, 312, 214, 80, 38, FILL_SKY, "REPLAY", NULL, 0, H_REPLAY, 0, 0, &tfont_label, now);
}

/* ------------------------------- the other screens ------------------------------ */

static void draw_waiting(view_t *v, snc_client *c, uint32_t now) {
	const snc_game *g = &c->game;
	snc_status_t st;
	snc_client_status(c, &st);
	sky();
	if (c->mode == 'i' && g->invite_url[0]) {
		sheet(24, 30, 432, 232, "INVITE A FRIEND", C_GRAPE1, C_GRAPE2, C_GRAPE_LIP);
		int scale = 4, size = gx_qr_size(scale, g->invite_url);
		while (size > 196 && scale > 2) size = gx_qr_size(--scale, g->invite_url);
		gx_qr(40, 46 + (200 - size) / 2.0f, scale, g->invite_url);
		float tx = 40 + size + 16, tw = 456 - 16 - tx;
		gx_text(&tfont_label_l, tx, 50, "Waiting for your friend...", W(C_INK));
		gx_wrap(&tfont_body, tx, 78, tw, tfont_body.line + 1, "Scan the code with a phone. Your friend plays you in their browser: no app, no sign-up.", W(C_INK2), 5);
		gx_wrap(&tfont_small, tx, 160, tw, tfont_small.line, g->invite_url, W(C_INK3), 3);
	} else if (c->mode == 'c' && g->code[0]) {
		sheet(24, 30, 432, 232, "PLAY BY CODE", C_SKY1, C_SKY2, C_SKY_LIP);
		gx_text_c(&tfont_body, 240, 50, "Your code", W(C_INK2));
		for (int i = 0; i < 6 && g->code[i]; i++) {
			char ch[2] = {g->code[i], 0};
			float bx = 240 - 3 * 50 + i * 50 + 4;
			gx_rrect(bx, 72 + 4, 42, 54, 9, W(C_SUGAR_EDGE), W(C_SUGAR_EDGE), 0);
			gx_rrect(bx, 72, 42, 54, 9, W(C_SUGAR2), W(C_SUGAR3), 0);
			gx_text_c(&tfont_huge, bx + 21, 72, ch, W(C_INK));
		}
		gx_wrap(&tfont_body, 48, 140, 384, tfont_body.line + 1, st.detail, W(C_INK2), 4);
	} else {
		sheet(24, 30, 432, 232, "QUICK MATCH", C_SUN1, C_SUN2, C_SUN_LIP);
		float bob = sinf((float)(now % 2000) / 2000.0f * 2 * PI) * 3;
		gx_sprite(champ_tex(my_champ(c)), 0, 0, 256, 256, 40, 40 + bob, 160, 160, W(0xFFFFFF), 1);
		gx_text(&tfont_label_l, 210, 70, st.main, W(C_INK));
		gx_wrap(&tfont_body, 210, 100, 230, tfont_body.line + 1, st.detail, W(C_INK2), 4);
		for (int d = 0; d < 3; d++) {
			float b = sinf((float)(now % 900) / 900.0f * 2 * PI - d * 0.9f) * 3;
			gx_rrect(212 + d * 14, 150 - b, 9, 9, 4.5f, W(C_SUN2), W(C_SUN2), 0);
		}
	}
	slab(v, 160, 218, 130, 32, FILL_SUGAR, "HOW TO PLAY", NULL, 't', H_HOW, 0, 0, &tfont_label, now);
	slab(v, 300, 218, 130, 32, FILL_CHERRY, "CANCEL", NULL, 0, H_RESIGN, 0, c->busy[0] != 0, &tfont_label, now);
	toast(c, 190);
}

static void draw_code(view_t *v, snc_client *c, uint32_t now) {
	sky();
	sheet(16, 24, 448, 240, "PLAY BY CODE", C_SKY1, C_SKY2, C_SKY_LIP);
	for (int i = 0; i < 6; i++) {
		float bx = 240 - 3 * 40 + i * 40 + 3;
		gx_rrect(bx, 40 + 3, 34, 40, 8, W(C_SUGAR_EDGE), W(C_SUGAR_EDGE), 0);
		gx_rrect(bx, 40, 34, 40, 8, W(C_SUGAR2), W(C_SUGAR3), 0);
		if (i < c->code_len) {
			char ch[2] = {c->code[i], 0};
			gx_text_c(&tfont_title, bx + 17, 44, ch, W(C_INK));
		} else if (i == c->code_len) {
			gx_rect(bx + 9, 70, 16, 3, W(C_INK3), W(C_INK3));
		}
	}
	float kw = 46, kh = 26;
	for (int i = 0; CODE_ALPHABET[i]; i++) {
		char ch[2] = {CODE_ALPHABET[i], 0};
		float x = 240 - 4 * (kw + 4) + (i % 8) * (kw + 4) + 2, y = 92 + (i / 8) * (kh + 4);
		slab(v, x, y, kw, kh, FILL_SUGAR, ch, NULL, 0, H_KEY, i, c->code_len >= 6, &tfont_label, now);
	}
	float y = 92 + 4 * (kh + 4) + 4;
	slab(v, 30, y, 96, 30, FILL_SUGAR, "BACK", NULL, 'o', H_BACK, 0, 0, &tfont_label, now);
	slab(v, 134, y, 96, 30, FILL_SUGAR, "DELETE", NULL, 0, H_DEL, 0, c->code_len == 0, &tfont_label, now);
	slab(v, 238, y, 100, 30, FILL_SKY, "HOST", NULL, 0, H_HOST, 0, c->busy[0] != 0, &tfont_label, now);
	slab(v, 346, y, 104, 30, FILL_SUN, "JOIN", NULL, 0, H_JOIN, 0, c->code_len != 6 || c->busy[0] != 0, &tfont_label, now);
	toast(c, 128);
}

static void draw_rules(view_t *v, snc_client *c, uint32_t now) {
	sky();
	sheet(12, 22, 456, 244, "HOW TO PLAY", C_SUN1, C_SUN2, C_SUN_LIP);
	const char *text = c->rules_ready ? c->rules : "Fetching the rules...";
	float lh = tfont_small.line;
	int lines = gx_wrap(&tfont_small, 0, 0, 280, lh, text, 0, 0);
	int visible = (int)(214 / lh);
	v->scroll_max = lines > visible ? lines - visible : 0;
	if (v->scroll > v->scroll_max) v->scroll = v->scroll_max;
	sceGuScissor(24, 40, 304, 258);
	gx_wrap(&tfont_small, 26, 42 - v->scroll * lh, 280, lh, text, W(C_INK), 0);
	sceGuScissor(0, 0, 480, 272);
	if (v->scroll_max > 0) {
		float bar = 210.0f * visible / lines, pos = (210 - bar) * v->scroll / v->scroll_max;
		gx_rrect(312, 44 + pos, 4, bar, 2, W(C_SUGAR_EDGE), W(C_SUGAR_EDGE), 0);
	}
	glass(324, 40, 132, 172, 10, 235);
	gx_text(&tfont_label, 332, 44, "On the PSP", W(C_SUN1));
	gx_wrap(&tfont_small, 332, 64, 118, tfont_small.line - 1,
	        "D-pad: move. Cross: pick a card, then a green tile. Circle: put it back. Square: Stay, or play Flip! / Swap!. L / R: your next card. "
	        "START: resign. Up / down here: scroll.",
	        W(C_SUGAR), 11);
	slab(v, 330, 222, 120, 32, FILL_SUN, "BACK", NULL, 'o', H_BACK, 0, 0, &tfont_label, now);
}

/* ----------------------------------- frame ------------------------------------- */

void view3d_draw(view_t *v, snc_client *c, uint32_t now) {
	view_begin(v, c);
	v->show_focus = 1;
	int screen = c->screen;
	if (screen == SC_GAME && !c->have_game) screen = SC_LOBBY;
	switch (screen) {
	case SC_RULES: draw_rules(v, c, now); break;
	case SC_CODE: draw_code(v, c, now); break;
	case SC_GAME:
		if (c->game.status == SNC_WAITING) draw_waiting(v, c, now);
		else draw_game(v, c, now);
		break;
	default: draw_lobby(v, c, now); break;
	}
	view_end(v, c);
}

void view3d_skip_intro(void) {
	vs_start = 0;
}

int view3d_in_intro(const snc_client *c, uint32_t now) {
	return c->have_game && !strcmp(vs_game, c->game.id) && vs_start && (now - vs_start) < 2400;
}

void view3d_champ_step(snc_client *c, int dir) {
	if (!spot_champ) spot_champ = my_champ(c);
	spot_champ = ((spot_champ - 1 + dir) % 46 + 46) % 46 + 1;
	c->rec.champ = spot_champ;
	c->rec_dirty = 1;
}
