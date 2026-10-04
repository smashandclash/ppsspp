// The client's rules of thumb, shared by every platform: reading move names, which
// tiles a selected card can reach, the house opponent's persona, and the rating.
#ifndef SNC_GAME_H
#define SNC_GAME_H

#include <stddef.h>
#include <stdint.h>

#include "snc_api.h"

// A move name, taken apart:
//   "Pengu@C2" places, "Teddy!C2" overruns, "hop→E3" / "hop: stay" resolve a hop,
//   "BOULDER(D2)", "FREEZE(C1)", "RECRUIT(D3→B1)", "FLIP" and "SWAP" play an effect.
typedef struct {
	char key[24];   // the card name, the effect keyword, or "hop"
	char cells[2];  // board indices
	int ncells;
} snc_move;

int snc_parse_move(const char *name, snc_move *m);

// The key a hand card's moves start with: its name, or an effect's keyword ("Boulder!" -> BOULDER).
void snc_hand_key(const snc_hcard *c, char *key, size_t cap);

// Whether a hand card has any legal move right now.
int snc_playable(const snc_game *g, int hand_index);

// What is picked so far on your turn: a hand card and the tiles chosen for it.
typedef struct {
	int selected;   // index into the hand, or -1
	char path[2];
	int path_n;
} snc_pick;

// The tiles that continue the pick (bit i = board index i). During a hop: the hop's tiles.
uint16_t snc_targets(const snc_game *g, const snc_pick *p);
// A move the selected card plays with no tile (FLIP, SWAP), or -1.
int snc_immediate(const snc_game *g, const snc_pick *p);
// The move the pick spells out exactly, or -1.
int snc_complete(const snc_game *g, const snc_pick *p);
// "hop: stay" during a hop, or -1.
int snc_hop_stay(const snc_game *g);

// The house opponent wears a player's handle, never a label for what it is: the same
// names the tldraw client and the lobby use, derived from the game id.
void snc_persona(const char *game_id, char *out, size_t cap);
// The other player's name as the screen shows it.
void snc_opponent_name(const snc_game *g, char *out, size_t cap);

// Your new rating after a game against an opponent rated opp (score 1, 0.5 or 0).
int snc_elo(int rating, int opp, double score);

#endif
