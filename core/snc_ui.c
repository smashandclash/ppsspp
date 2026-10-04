// What every platform's screens share. See snc_ui.h.
#include "snc_ui.h"

#include <string.h>

const char CODE_ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

void view_init(view_t *v) {
	memset(v, 0, sizeof *v);
	v->last_screen = -1;
}

void view_begin(view_t *v, const snc_client *c) {
	v->n = 0;
	if ((int)c->screen != v->last_screen) {
		v->scroll = 0;
		v->last_screen = c->screen;
	}
	if (!snc_client_over(c)) v->show_replay = 0;
}

void view_add(view_t *v, int x, int y, int w, int h, int id, int arg) {
	if (v->n >= (int)(sizeof v->hit / sizeof v->hit[0])) return;
	view_hit *t = &v->hit[v->n++];
	t->x = (short)x, t->y = (short)y, t->w = (short)w, t->h = (short)h;
	t->id = (unsigned char)id;
	t->arg = (signed char)arg;
}

int view_focused(const view_t *v, int id, int arg) {
	return v->show_focus && v->focus_id == id && v->focus_arg == arg;
}

int view_find(const view_t *v, int id, int arg) {
	for (int i = 0; i < v->n; i++)
		if (v->hit[i].id == id && v->hit[i].arg == arg) return i;
	return -1;
}

int view_hit_at(const view_t *v, int x, int y) {
	for (int i = v->n - 1; i >= 0; i--) {
		const view_hit *t = &v->hit[i];
		if (x >= t->x - 1 && y >= t->y - 1 && x < t->x + t->w + 1 && y < t->y + t->h + 1) return i;
	}
	return -1;
}

// After a redraw: if what had the focus is gone, give it to something sensible
// (on your turn: a green tile, else a card you can play).
void view_end(view_t *v, const snc_client *c) {
	if (v->n == 0 || view_find(v, v->focus_id, v->focus_arg) >= 0) return;
	int pick = -1;
	if (c->screen == SC_GAME && c->game.your_turn) {
		uint16_t lit = snc_targets(&c->game, &c->pick);
		for (int i = 0; i < v->n && pick < 0; i++)
			if (v->hit[i].id == H_CELL && (lit & (1u << v->hit[i].arg))) pick = i;
		for (int i = 0; i < v->n && pick < 0; i++)
			if (v->hit[i].id == H_HAND && snc_playable(&c->game, v->hit[i].arg)) pick = i;
	}
	if (pick < 0) pick = 0;
	v->focus_id = v->hit[pick].id;
	v->focus_arg = v->hit[pick].arg;
}

void view_press(view_t *v, snc_client *c, int index) {
	if (index < 0 || index >= v->n) return;
	const view_hit *t = &v->hit[index];
	v->focus_id = t->id;
	v->focus_arg = t->arg;
	switch (t->id) {
	case H_HAND: snc_act_hand(c, t->arg); break;
	case H_CELL: snc_act_cell(c, t->arg); break;
	case H_NEW: snc_act_start(c, 'h'); break;
	case H_QUICK: snc_act_start(c, 'q'); break;
	case H_INVITE: snc_act_start(c, 'i'); break;
	case H_CODE: snc_act_code_screen(c); break;
	case H_RULESET: snc_act_toggle_rules(c); break;
	case H_HOW: snc_act_how_to_play(c); break;
	case H_RESIGN: snc_act_resign(c); break;
	case H_ACTION: snc_act_action(c); break;
	case H_AGAIN: snc_act_start(c, c->mode == 'q' ? 'q' : c->mode == 'i' ? 'i' : 'h'); break;
	case H_LOBBY: snc_act_lobby(c); break;
	case H_REPLAY: v->show_replay = !v->show_replay; break;
	case H_BACK: snc_act_back(c); break;
	case H_KEY:
		if (c->code_len < 6) c->code[c->code_len++] = CODE_ALPHABET[(int)t->arg], c->code[c->code_len] = 0;
		break;
	case H_DEL:
		if (c->code_len > 0) c->code[--c->code_len] = 0;
		break;
	case H_JOIN: snc_act_join(c); break;
	case H_HOST: snc_act_start(c, 'c'); break;
	case H_UP: view_scroll(v, -8); break;
	case H_DOWN: view_scroll(v, 8); break;
	default: break;
	}
}

void view_nav(view_t *v, int dx, int dy) {
	int cur = view_find(v, v->focus_id, v->focus_arg);
	if (!v->show_focus || cur < 0) {
		v->show_focus = 1;
		return;
	}
	const view_hit *a = &v->hit[cur];
	int ax = a->x + a->w / 2, ay = a->y + a->h / 2;
	int a0 = dx ? a->y : a->x, a1 = a0 + (dx ? a->h : a->w);  // its span across the move
	int best = -1;
	long best_score = 0;
	for (int i = 0; i < v->n; i++) {
		if (i == cur) continue;
		const view_hit *b = &v->hit[i];
		int bx = b->x + b->w / 2, by = b->y + b->h / 2;
		int along = dx ? (bx - ax) * dx : (by - ay) * dy;
		int across = dx ? by - ay : bx - ax;
		if (along <= 0) continue;
		if (across < 0) across = -across;
		// the nearest target that lines up with this one wins, before any that does not:
		// up from a wide button reaches the row right above it, not the one aligned further on
		int b0 = dx ? b->y : b->x, b1 = b0 + (dx ? b->h : b->w);
		int lined_up = b0 < a1 && a0 < b1;
		long score = lined_up ? 4L * along + across : 100000L + along + 2L * across;
		if (best < 0 || score < best_score) best = i, best_score = score;
	}
	if (best >= 0) {
		v->focus_id = v->hit[best].id;
		v->focus_arg = v->hit[best].arg;
	}
}

void view_activate(view_t *v, snc_client *c) {
	int cur = view_find(v, v->focus_id, v->focus_arg);
	if (!v->show_focus) {
		v->show_focus = 1;
		return;
	}
	if (cur >= 0) view_press(v, c, cur);
}

void view_back(view_t *v, snc_client *c) {
	if (c->screen == SC_RULES || c->screen == SC_CODE) {
		snc_act_back(c);
		return;
	}
	if (v->show_replay) {
		v->show_replay = 0;
		return;
	}
	c->pick.selected = -1;  // put the card back
	c->pick.path_n = 0;
}

void view_scroll(view_t *v, int lines) {
	v->scroll += lines;
	if (v->scroll < 0) v->scroll = 0;
	if (v->scroll > v->scroll_max) v->scroll = v->scroll_max;
}

void view_step_hand(view_t *v, snc_client *c, int dir) {
	const snc_game *g = &c->game;
	if (c->screen != SC_GAME || !g->your_turn || g->has_hop || c->busy[0] || g->hand_n == 0) return;
	int cur = c->pick.selected;
	for (int k = 1; k <= g->hand_n; k++) {
		int start = cur < 0 ? (dir > 0 ? -1 : 0) : cur;
		int i = ((start + dir * k) % g->hand_n + g->hand_n) % g->hand_n;
		if (snc_playable(g, i)) {
			c->pick.selected = i;  // never "pick again": that would play Flip! or Swap!
			c->pick.path_n = 0;
			v->focus_id = H_HAND;
			v->focus_arg = i;
			return;
		}
	}
}

/* ------------------------------- board helpers -------------------------------- */

void view_tile_vals(const snc_game *g, const snc_tile *t, int out[4]) {
	if (!t->has_sides) {
		out[0] = out[1] = out[2] = out[3] = -1;
		return;
	}
	if (g->seat == 'B') out[0] = t->s, out[1] = t->w, out[2] = t->n, out[3] = t->e;
	else out[0] = t->n, out[1] = t->e, out[2] = t->s, out[3] = t->w;
}

void view_hand_vals(const snc_hcard *h, int out[4]) {
	if (h->effect) {
		out[0] = out[1] = out[2] = out[3] = -1;
		return;
	}
	out[0] = h->top, out[1] = h->right, out[2] = h->bottom, out[3] = h->left;
}

const char *view_chess_at(const snc_game *g, int cell) {
	for (int i = 0; i < g->chess_n; i++)
		if (g->chess[i].cell == cell) return g->chess[i].piece;
	return NULL;
}

int view_power_at(const snc_game *g, int cell) {
	for (int i = 0; i < g->power_n; i++)
		if (g->power[i].cell == cell) return i;
	return -1;
}

int view_overrun_at(const snc_game *g, int cell) {
	for (int i = 0; i < g->overrun_n; i++)
		if (g->overrun[i] == cell) return 1;
	return 0;
}

void view_cell_slot(const snc_game *g, int cell, int *col, int *row) {
	int c = cell % 5, r = cell / 5;
	int flip = g->seat == 'B';  // seat B sits at row 3: the board turns so your side is nearest you
	*col = flip ? 4 - c : c;
	*row = flip ? r : 2 - r;
}
