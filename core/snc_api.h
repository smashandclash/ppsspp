// The Smash&Clash SDK's game calls, in C: the same HTTP API that @smashandclash/sdk
// wraps (https://docs.smashandclash.in), for clients that cannot run JavaScript.
//
//   SDK (JavaScript)                         here
//   sc.games.startHouse({name, ...})         snc_start_house()
//   sc.games.quickMatch({name, ...})         snc_quick_match()
//   sc.games.createDuel({opponent:'person'}) snc_create_invite()   -> g->invite_url
//   sc.games.createDuel({})                  snc_host_code()       -> g->code
//   sc.games.joinDuel(code)                  snc_join_code()
//   sc.games.resume(id, token)               snc_refresh()
//   game.play('Pengu@C2')                    snc_play()
//   game.waitForTurn(20)                     snc_wait()
//   game.resign()                            snc_resign()
//   game.sync({since})                       snc_opponent_hand()
//   game.review()                            snc_review()
//   sc.rules()                               snc_rules()
//
// Every call blocks: run them on a network thread. They return 0 on success, an
// SNC_E_* transport error (< 0), or the HTTP status of an API error (> 0); either
// way api->err says what happened in words.
//
// Two ways out: straight to the API over HTTPS (the DS, the PSP), or, built with
// SNC_BRIDGE, through an SDK bridge that makes each call with @smashandclash/sdk (the
// GBA, which has no network; see snc_bridge.h).
#ifndef SNC_API_H
#define SNC_API_H

#include <stddef.h>

#include "snc_errors.h"
#ifndef SNC_BRIDGE
#include "snc_http.h"
#endif

#define SNC_HOST "www.smashandclash.in"
#define SNC_SITE "https://www.smashandclash.in"
#define SNC_MAX_MOVES 200

typedef enum { SNC_WAITING, SNC_ACTIVE, SNC_FINISHED, SNC_ABANDONED } snc_status;

// One board tile, as your seat sees it.
typedef struct {
	char card[24];      // "" = empty
	char owner;         // 'y' you, 'o' opponent, 0 empty
	char frozen;
	char has_sides;
	signed char n, e, s, w;  // board-frame sides (north = toward row 3)
} snc_tile;

typedef struct {
	char card[24];
	char effect;        // 1 = an effect card (Boulder!, Flip!, ...)
	signed char top, right, bottom, left;
	char color[10];
	char does[160];     // an effect's text
} snc_hcard;

typedef struct {
	char id[32];
	char token[64];         // your seat's secret
	char invite_url[200];   // a game for a person: the link to send them
	char code[8];           // a duel by code: the code to share
	char replay_url[400];
	char kind[8];           // "house" / "duel"
	snc_status status;
	char ruleset;           // 'm' mutators, 'c' classic
	char seat;              // 'A' or 'B'
	char players[2][48];    // [0] = seat A
	char kinds[2];          // 'a' agent, 'p' person, 'h' house, 0 = nobody yet
	char turn;              // 'A', 'B' or 0
	int move_count;
	char last_move[48];
	int score[2];
	char winner;            // 'A', 'B', 'd' (draw) or 0
	char matchmaking;

	// your seat's view
	char has_view, your_turn;
	int score_you, score_opp;
	snc_tile board[15];     // [r * 5 + c]: row r 0..2 (row 1..3), column c 0..4 (A..E)
	snc_hcard hand[8];
	int hand_n;
	int draw_pile;
	struct { char cell; char piece[10]; } chess[8];  // cell = index into board
	int chess_n;
	struct { char cell; char color[10]; int boost; } power[8];
	int power_n;
	char overrun[8];
	int overrun_n;
	char has_hop;
	char hop_from;
	char moves[SNC_MAX_MOVES][32];  // legal moves on your turn, by name
	int moves_n;
} snc_game;

typedef struct {
	int ok;
	int accuracy[2];  // 0-100, [0] = seat A
} snc_review_t;

typedef struct {
#ifndef SNC_BRIDGE
	snc_http http;
#endif
	char err[200];
	char *buf;       // the last answer
	size_t cap;
	void *toks;
	unsigned max_toks;
} snc_api;

int snc_api_init(snc_api *a, const char *user_agent, const char *sdk);
// Cut short the call in flight (a long wait the player no longer needs). Any thread.
void snc_api_cancel(snc_api *a);

int snc_start_house(snc_api *a, const char *name, char ruleset, int strength, snc_game *g);
int snc_quick_match(snc_api *a, const char *name, char ruleset, snc_game *g);
int snc_create_invite(snc_api *a, const char *name, char ruleset, snc_game *g);
int snc_host_code(snc_api *a, const char *name, char ruleset, snc_game *g);
int snc_join_code(snc_api *a, const char *code, const char *name, snc_game *g);
int snc_refresh(snc_api *a, snc_game *g);
int snc_play(snc_api *a, snc_game *g, const char *move);
int snc_wait(snc_api *a, snc_game *g, int seconds);
int snc_resign(snc_api *a, snc_game *g);
int snc_opponent_hand(snc_api *a, snc_game *g, int *count);
int snc_review(snc_api *a, const char *game_id, snc_review_t *r);
int snc_rules(snc_api *a, char *out, size_t cap);

// Cell names: index 0..14 <-> "A1".."E3". Returns -1 for anything else.
int snc_cell(const char *name);
void snc_cell_name(int cell, char out[3]);

#endif
