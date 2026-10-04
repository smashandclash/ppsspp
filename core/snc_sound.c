// What the game sounds like. See snc_sound.h.
#include "snc_sound.h"

#include <stdio.h>
#include <string.h>

const uint8_t SFX_PRIORITY[SFX_COUNT] = {
	[SFX_MOVE] = 1, [SFX_BACK] = 2, [SFX_SELECT] = 3, [SFX_START] = 4, [SFX_PLACE] = 5, [SFX_TURN] = 6, [SFX_CAPTURE] = 7, [SFX_ERROR] = 8,
};

static int bits(unsigned v) {
	int n = 0;
	for (; v; v &= v - 1) n++;
	return n;
}

unsigned snc_sound_update(snc_sound *s, const snc_client *c, snc_music *music) {
	const snc_game *g = &c->game;
	int have = c->have_game;
	int active = have && g->status == SNC_ACTIVE;
	int waiting = have && g->status == SNC_WAITING;
	int over = snc_client_over(c);
	int same = have && s->have_game && !strcmp(s->game_id, g->id);
	unsigned fx = 0;

	if (s->init) {
		if (active && (!same || s->waiting)) fx |= 1u << SFX_START;  // a game begins, or the other player is in
		if (same && g->move_count > s->move_count) {                  // a card landed (theirs or yours)
			fx |= 1u << SFX_PLACE;
			if (bits(c->flash) >= 2) fx |= 1u << SFX_CAPTURE;          // and turned others
		}
		if (active && g->your_turn && !s->your_turn && !c->busy[0]) fx |= 1u << SFX_TURN;
		if (c->notice_until != s->notice_until && c->notice_bad && snc_client_notice(c)) fx |= 1u << SFX_ERROR;
	}

	if (!same) s->jingle = MUS_NONE;
	if (over && s->jingle == MUS_NONE) s->jingle = g->status == SNC_FINISHED && g->winner == g->seat ? MUS_WIN : MUS_LOSE;
	*music = over ? s->jingle : active ? MUS_GAME : MUS_LOBBY;

	s->init = 1;
	s->have_game = have;
	s->active = active;
	s->waiting = waiting;
	s->your_turn = active && g->your_turn;
	s->move_count = g->move_count;
	s->notice_until = c->notice_until;
	snprintf(s->game_id, sizeof s->game_id, "%s", have ? g->id : "");

	if (!c->rec.sound) {
		*music = MUS_NONE;
		return 0;
	}
	return fx;
}
