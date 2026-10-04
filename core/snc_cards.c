// Which card is which: the art index of a board tile, a hand card or a move (cards that
// share a name, like the two Lizzies, are told apart by their sides), and the power
// tiles' colours. No drawing here, so every build can use it. See snc_draw.h.
#include <string.h>

#include "assets.h"
#include "snc_draw.h"
#include "snc_game.h"

int sd_tile_card(const void *game, int cell) {
	const snc_game *g = game;
	const snc_tile *t = &g->board[cell];
	if (!t->card[0]) return -1;
	if (!t->has_sides) return snc_card_index(t->card);
	// board-frame sides back to the printed ones, from the owner's seat
	char mine = g->seat == 'B' ? 'B' : 'A';
	char seat = t->owner == 'y' ? mine : (mine == 'A' ? 'B' : 'A');
	if (seat == 'B') return snc_card_find(t->card, t->s, t->w, t->n, t->e);
	return snc_card_find(t->card, t->n, t->e, t->s, t->w);
}

int sd_hand_card(const void *hand_card) {
	const snc_hcard *h = hand_card;
	return h->effect ? snc_card_index(h->card) : snc_card_find(h->card, h->top, h->right, h->bottom, h->left);
}

int sd_move_card(const void *game, const char *move) {
	const snc_game *g = game;
	snc_move m;
	if (!move || !move[0]) return -1;
	snc_parse_move(move, &m);
	if (!strcmp(m.key, "hop")) return m.ncells ? sd_tile_card(g, m.cells[0]) : -1;
	// a placement or an overrun: the card is on its tile now (unless it was captured away)
	if (m.ncells == 1 && !strcmp(g->board[(int)m.cells[0]].card, m.key)) return sd_tile_card(g, m.cells[0]);
	for (int i = 0; i < SNC_CARDS; i++) {
		const char *name = snc_cards[i].name;
		char key[24];
		size_t n = 0;
		for (const char *s = name; *s && *s != '!' && n + 1 < sizeof key; s++) key[n++] = (snc_cards[i].effect && *s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
		key[n] = 0;
		if (!strcmp(name, m.key) || (snc_cards[i].effect && !strcmp(key, m.key))) return i;  // "Boulder!" plays as BOULDER
	}
	return -1;
}

uint32_t sd_power_color(const char *name) {
	if (!strcmp(name, "pink")) return 0xFF5FB4;
	if (!strcmp(name, "blue")) return 0x3D8BFF;
	if (!strcmp(name, "red")) return 0xF0404A;
	if (!strcmp(name, "green")) return 0x3CCB5A;
	if (!strcmp(name, "orange")) return 0xFF8A2A;
	if (!strcmp(name, "purple")) return 0x9B5CFF;
	if (!strcmp(name, "gray") || !strcmp(name, "grey")) return 0x9AA6BF;
	return 0xC0C8D8;
}
