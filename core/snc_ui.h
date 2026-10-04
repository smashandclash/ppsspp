// What every platform's screens share: the touch/focus targets a draw leaves behind,
// D-pad navigation between them, and what pressing each one does. A platform's
// view_draw() lays out its own screens and records the targets with view_add();
// touch, buttons and the D-pad all end up in view_press().
#ifndef SNC_UI_H
#define SNC_UI_H

#include <stdint.h>

#include "snc_client.h"

enum {
	H_NONE,
	H_HAND,      // arg: hand index
	H_CELL,      // arg: board index
	H_NEW,
	H_QUICK,
	H_INVITE,
	H_CODE,
	H_RULESET,
	H_HOW,
	H_RESIGN,
	H_ACTION,
	H_AGAIN,
	H_LOBBY,
	H_REPLAY,
	H_BACK,
	H_KEY,       // arg: index into the code alphabet
	H_DEL,
	H_JOIN,
	H_HOST,
	H_UP,
	H_DOWN,
};

typedef struct {
	short x, y, w, h;
	unsigned char id;
	signed char arg;
} view_hit;

typedef struct {
	view_hit hit[72];
	int n;
	int focus_id, focus_arg;  // the D-pad's place, kept across redraws
	int show_focus;           // drawn after a button press (always, on a console without touch)
	int scroll;               // the rules text, in lines
	int scroll_max;
	int show_replay;          // the game-over QR
	int last_screen;
} view_t;

extern const char CODE_ALPHABET[];  // the API's duel codes: no I, O, 0 or 1

void view_init(view_t *v);
void view_begin(view_t *v, const snc_client *c);   // before a draw: forget the old targets
void view_end(view_t *v, const snc_client *c);     // after a draw: keep the focus on something real
void view_add(view_t *v, int x, int y, int w, int h, int id, int arg);
int view_focused(const view_t *v, int id, int arg);
int view_find(const view_t *v, int id, int arg);
int view_hit_at(const view_t *v, int x, int y);    // a hit index, or -1

void view_press(view_t *v, snc_client *c, int index);
void view_nav(view_t *v, int dx, int dy);
void view_activate(view_t *v, snc_client *c);      // A / cross: press what has the focus
void view_back(view_t *v, snc_client *c);          // B / circle
void view_scroll(view_t *v, int lines);
void view_step_hand(view_t *v, snc_client *c, int dir);  // L / R: the next playable card

// Board helpers for drawing: a tile's numbers as your screen shows them (top, right,
// bottom, left), a hand card's, and the special tiles.
void view_tile_vals(const snc_game *g, const snc_tile *t, int out[4]);
void view_hand_vals(const snc_hcard *h, int out[4]);
const char *view_chess_at(const snc_game *g, int cell);
int view_power_at(const snc_game *g, int cell);
int view_overrun_at(const snc_game *g, int cell);
// Where a cell sits on your screen: column 0..4 and row 0..2 from the top.
void view_cell_slot(const snc_game *g, int cell, int *col, int *row);

#endif
