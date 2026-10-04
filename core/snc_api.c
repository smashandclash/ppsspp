// The Smash&Clash SDK's game calls, in C. See snc_api.h.
#include "snc_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snc_json.h"
#ifdef SNC_BRIDGE
#include "snc_bridge.h"
#endif

#ifndef SNC_BUF_CAP
#define SNC_BUF_CAP (40 * 1024)
#endif
#ifndef SNC_MAX_TOKS
#define SNC_MAX_TOKS 3600
#endif
#define BUF_CAP SNC_BUF_CAP
#define MAX_TOKS SNC_MAX_TOKS

int snc_cell(const char *name) {
	if (!name || name[0] < 'A' || name[0] > 'E' || name[1] < '1' || name[1] > '3') return -1;
	return (name[1] - '1') * 5 + (name[0] - 'A');
}

void snc_cell_name(int cell, char out[3]) {
	out[0] = (char)('A' + cell % 5);
	out[1] = (char)('1' + cell / 5);
	out[2] = 0;
}

int snc_api_init(snc_api *a, const char *user_agent, const char *sdk) {
	memset(a, 0, sizeof *a);
	a->cap = BUF_CAP;
	a->buf = malloc(BUF_CAP);
	a->max_toks = MAX_TOKS;
	a->toks = malloc(sizeof(jsmntok_t) * MAX_TOKS);
	if (!a->buf || !a->toks) {
		snprintf(a->err, sizeof a->err, "Out of memory.");
		return SNC_E_NET;
	}
#ifdef SNC_BRIDGE
	(void)user_agent, (void)sdk;  // the bridge's SDK identifies itself
	return 0;
#else
	int r = snc_http_init(&a->http, SNC_HOST, user_agent, sdk);
	if (r) snprintf(a->err, sizeof a->err, "%s", a->http.err);
	return r;
#endif
}

void snc_api_cancel(snc_api *a) {
#ifdef SNC_BRIDGE
	(void)a;
	snc_bridge_cancel();
#else
	snc_http_cancel(&a->http);
#endif
}

/* -------------------------------- the request -------------------------------- */

// Turns an API failure into words: problem+json {error, hint}, {error: {message}}, or the status.
static void api_error(snc_api *a, int status, const sj *j, int ok) {
	char msg[140] = "", hint[120] = "";
	if (ok > 0 && sj_is_object(j, 0)) {
		int e = sj_key(j, 0, "error");
		if (sj_is_object(j, e)) sj_str(j, sj_key(j, e, "message"), msg, sizeof msg);
		else sj_str(j, e, msg, sizeof msg);
		if (!msg[0]) sj_str(j, sj_key(j, 0, "detail"), msg, sizeof msg);
		sj_str(j, sj_key(j, 0, "hint"), hint, sizeof hint);
	}
	if (!msg[0]) {
		if (status == 429) snprintf(msg, sizeof msg, "Too many requests. Wait a moment.");
		else if (status >= 500) snprintf(msg, sizeof msg, "The game server had a problem (%d). Try again.", status);
		else snprintf(msg, sizeof msg, "The game server said no (%d).", status);
	}
	if (msg[0] >= 'a' && msg[0] <= 'z') msg[0] = (char)(msg[0] - 32);  // "no duel with that code" -> "No duel..."
	size_t n = strlen(msg);
	if (n && msg[n - 1] != '.' && msg[n - 1] != '!' && msg[n - 1] != '?' && n + 1 < sizeof msg) msg[n] = '.', msg[n + 1] = 0;
	if (hint[0]) snprintf(a->err, sizeof a->err, "%s %s", msg, hint);
	else snprintf(a->err, sizeof a->err, "%s", msg);
}

// One call: on success the answer is parsed into *j and 0 comes back. `op` is the SDK
// method it stands for (what the SDK bridge calls); method + path are the API request.
static int call(snc_api *a, const char *op, const char *method, const char *path, const char *token, const char *body, int timeout_ms,
                sj *j) {
	int status = 0;
	a->err[0] = 0;
#ifdef SNC_BRIDGE
	int n = snc_bridge_request(op, method, path, token, body, a->buf, a->cap, timeout_ms, &status, a->err, sizeof a->err);
	if (n < 0) {
		if (!a->err[0]) snprintf(a->err, sizeof a->err, "The SDK bridge did not answer.");
		return n;
	}
#else
	(void)op;
	int n = snc_http_request(&a->http, method, path, token, body, a->buf, a->cap, timeout_ms, &status);
	if (n < 0) {
		snprintf(a->err, sizeof a->err, "%s", a->http.err[0] ? a->http.err : "The connection failed.");
		return n;
	}
#endif
	int ok = sj_parse(j, a->buf, (size_t)n, a->toks, a->max_toks);
	if (status < 200 || status >= 300) {
		api_error(a, status, j, ok);
		return status ? status : SNC_E_PROTO;
	}
	if (ok < 1) {
		snprintf(a->err, sizeof a->err, "The game server's answer did not parse.");
		return SNC_E_PROTO;
	}
	return 0;
}

/* ------------------------------ reading a game ------------------------------- */

static void copy_str(const sj *j, int tok, char *dst, size_t cap) {
	if (sj_str(j, tok, dst, cap) < 0) dst[0] = 0;
}

static char kind_of(const sj *j, int tok) {
	if (sj_eq(j, tok, "agent")) return 'a';
	if (sj_eq(j, tok, "person")) return 'p';
	if (sj_eq(j, tok, "house")) return 'h';
	return 0;
}

static char seat_of(const sj *j, int tok) {
	if (sj_eq(j, tok, "A")) return 'A';
	if (sj_eq(j, tok, "B")) return 'B';
	if (sj_eq(j, tok, "draw")) return 'd';
	return 0;
}

static void parse_view(const sj *j, int v, snc_game *g) {
	g->has_view = 1;
	g->your_turn = (char)sj_bool(j, sj_key(j, v, "yourTurn"), 0);
	int sc = sj_key(j, v, "score");
	g->score_you = (int)sj_int(j, sj_key(j, sc, "you"), 0);
	g->score_opp = (int)sj_int(j, sj_key(j, sc, "opponent"), 0);
	g->draw_pile = (int)sj_int(j, sj_key(j, v, "drawPile"), 0);

	memset(g->board, 0, sizeof g->board);
	int b = sj_key(j, v, "board");
	for (int k = 0, t = b + 1; sj_is_array(j, b) && k < sj_size(j, b); k++, t = sj_next(j, t)) {
		char cell[4];
		copy_str(j, sj_key(j, t, "cell"), cell, sizeof cell);
		int c = snc_cell(cell);
		if (c < 0) continue;
		snc_tile *tile = &g->board[c];
		copy_str(j, sj_key(j, t, "card"), tile->card, sizeof tile->card);
		int o = sj_key(j, t, "owner");
		tile->owner = sj_eq(j, o, "you") ? 'y' : sj_eq(j, o, "opponent") ? 'o' : 0;
		tile->frozen = (char)sj_bool(j, sj_key(j, t, "frozen"), 0);
		int s = sj_key(j, t, "sides");
		tile->has_sides = sj_is_object(j, s);
		if (tile->has_sides) {
			tile->n = (signed char)sj_int(j, sj_key(j, s, "north"), 0);
			tile->e = (signed char)sj_int(j, sj_key(j, s, "east"), 0);
			tile->s = (signed char)sj_int(j, sj_key(j, s, "south"), 0);
			tile->w = (signed char)sj_int(j, sj_key(j, s, "west"), 0);
		}
	}

	g->hand_n = 0;
	int h = sj_key(j, v, "hand");
	for (int k = 0, t = h + 1; sj_is_array(j, h) && k < sj_size(j, h) && g->hand_n < 8; k++, t = sj_next(j, t)) {
		snc_hcard *c = &g->hand[g->hand_n++];
		memset(c, 0, sizeof *c);
		copy_str(j, sj_key(j, t, "card"), c->card, sizeof c->card);
		c->effect = sj_eq(j, sj_key(j, t, "kind"), "effect");
		c->top = (signed char)sj_int(j, sj_key(j, t, "top"), 0);
		c->right = (signed char)sj_int(j, sj_key(j, t, "right"), 0);
		c->bottom = (signed char)sj_int(j, sj_key(j, t, "bottom"), 0);
		c->left = (signed char)sj_int(j, sj_key(j, t, "left"), 0);
		copy_str(j, sj_key(j, t, "color"), c->color, sizeof c->color);
		copy_str(j, sj_key(j, t, "does"), c->does, sizeof c->does);
	}

	g->chess_n = g->power_n = g->overrun_n = 0;
	int sp = sj_key(j, v, "special");
	int ct = sj_key(j, sp, "chessTiles");
	for (int k = 0, t = ct + 1; sj_is_array(j, ct) && k < sj_size(j, ct) && g->chess_n < 8; k++, t = sj_next(j, t)) {
		char cell[4];
		copy_str(j, sj_key(j, t, "cell"), cell, sizeof cell);
		if (snc_cell(cell) < 0) continue;
		g->chess[g->chess_n].cell = (char)snc_cell(cell);
		copy_str(j, sj_key(j, t, "piece"), g->chess[g->chess_n].piece, sizeof g->chess[0].piece);
		g->chess_n++;
	}
	int pt = sj_key(j, sp, "powerTiles");
	for (int k = 0, t = pt + 1; sj_is_array(j, pt) && k < sj_size(j, pt) && g->power_n < 8; k++, t = sj_next(j, t)) {
		char cell[4];
		copy_str(j, sj_key(j, t, "cell"), cell, sizeof cell);
		if (snc_cell(cell) < 0) continue;
		g->power[g->power_n].cell = (char)snc_cell(cell);
		copy_str(j, sj_key(j, t, "color"), g->power[g->power_n].color, sizeof g->power[0].color);
		g->power[g->power_n].boost = (int)sj_int(j, sj_key(j, t, "boost"), 1);
		g->power_n++;
	}
	int oz = sj_key(j, sp, "overrunZones");
	for (int k = 0, t = oz + 1; sj_is_array(j, oz) && k < sj_size(j, oz) && g->overrun_n < 8; k++, t = sj_next(j, t)) {
		char cell[4];
		copy_str(j, t, cell, sizeof cell);
		if (snc_cell(cell) >= 0) g->overrun[g->overrun_n++] = (char)snc_cell(cell);
	}

	int hop = sj_key(j, v, "pendingHop");
	g->has_hop = sj_is_object(j, hop);
	if (g->has_hop) {
		char cell[4];
		copy_str(j, sj_key(j, hop, "from"), cell, sizeof cell);
		g->hop_from = (char)snc_cell(cell);
	}

	g->moves_n = 0;
	int lm = sj_key(j, v, "legalMoves");
	for (int k = 0, t = lm + 1; sj_is_array(j, lm) && k < sj_size(j, lm) && g->moves_n < SNC_MAX_MOVES; k++, t = sj_next(j, t))
		copy_str(j, t, g->moves[g->moves_n++], sizeof g->moves[0]);
}

static void parse_game(const sj *j, int gi, snc_game *g) {
	copy_str(j, sj_key(j, gi, "id"), g->id, sizeof g->id);
	copy_str(j, sj_key(j, gi, "kind"), g->kind, sizeof g->kind);
	int st = sj_key(j, gi, "status");
	g->status = sj_eq(j, st, "waiting") ? SNC_WAITING : sj_eq(j, st, "finished") ? SNC_FINISHED : sj_eq(j, st, "abandoned") ? SNC_ABANDONED : SNC_ACTIVE;
	g->ruleset = sj_eq(j, sj_key(j, gi, "ruleset"), "classic") ? 'c' : 'm';
	char seat = seat_of(j, sj_key(j, gi, "seat"));
	if (seat) g->seat = seat;
	int code = sj_key(j, gi, "code");
	if (code >= 0) copy_str(j, code, g->code, sizeof g->code);
	else if (g->status != SNC_WAITING) g->code[0] = 0;
	g->matchmaking = (char)sj_bool(j, sj_key(j, gi, "matchmaking"), 0);
	int p = sj_key(j, gi, "players");
	copy_str(j, sj_key(j, p, "A"), g->players[0], sizeof g->players[0]);
	copy_str(j, sj_key(j, p, "B"), g->players[1], sizeof g->players[1]);
	int pk = sj_key(j, gi, "playerKinds");
	g->kinds[0] = kind_of(j, sj_key(j, pk, "A"));
	g->kinds[1] = kind_of(j, sj_key(j, pk, "B"));
	g->turn = seat_of(j, sj_key(j, gi, "turn"));
	g->move_count = (int)sj_int(j, sj_key(j, gi, "moveCount"), 0);
	copy_str(j, sj_key(j, gi, "lastMove"), g->last_move, sizeof g->last_move);
	int sc = sj_key(j, gi, "score");
	g->score[0] = (int)sj_int(j, sj_key(j, sc, "A"), 0);
	g->score[1] = (int)sj_int(j, sj_key(j, sc, "B"), 0);
	g->winner = seat_of(j, sj_key(j, gi, "winner"));
	int ru = sj_key(j, gi, "replayUrl");
	if (ru >= 0) copy_str(j, ru, g->replay_url, sizeof g->replay_url);
	int v = sj_key(j, gi, "view");
	if (sj_is_object(j, v)) parse_view(j, v, g);
	else {
		g->your_turn = 0;
		g->moves_n = 0;
		g->has_hop = 0;
	}
	if (g->status != SNC_ACTIVE) {
		g->your_turn = 0;
		g->moves_n = 0;
	}
}

/* --------------------------------- the calls --------------------------------- */

static int start(snc_api *a, const char *op, const char *body, snc_game *g) {
	sj j;
	int r = call(a, op, "POST", "/api/v1/games", NULL, body, 30000, &j);
	if (r) return r;
	memset(g, 0, sizeof *g);
	parse_game(&j, sj_key(&j, 0, "game"), g);
	copy_str(&j, sj_key(&j, 0, "playerToken"), g->token, sizeof g->token);
	copy_str(&j, sj_key(&j, 0, "inviteUrl"), g->invite_url, sizeof g->invite_url);
	if (!g->id[0] || !g->token[0]) {
		snprintf(a->err, sizeof a->err, "The game server's answer had no game in it.");
		return SNC_E_PROTO;
	}
	return 0;
}

static const char *ruleset_name(char r) {
	return r == 'c' ? "classic" : "mutators";
}

int snc_start_house(snc_api *a, const char *name, char ruleset, int strength, snc_game *g) {
	char body[256], qn[120];
	sj_quote(qn, sizeof qn, name);
	snprintf(body, sizeof body, "{\"mode\":\"house\",\"name\":%s,\"as\":\"person\",\"ruleset\":\"%s\",\"strength\":%d}", qn, ruleset_name(ruleset), strength);
	return start(a, "games.startHouse", body, g);
}

int snc_quick_match(snc_api *a, const char *name, char ruleset, snc_game *g) {
	char body[256], qn[120];
	sj_quote(qn, sizeof qn, name);
	snprintf(body, sizeof body, "{\"mode\":\"quick\",\"name\":%s,\"as\":\"person\",\"ruleset\":\"%s\",\"opponent\":\"any\"}", qn, ruleset_name(ruleset));
	return start(a, "games.quickMatch", body, g);
}

int snc_create_invite(snc_api *a, const char *name, char ruleset, snc_game *g) {
	char body[256], qn[120];
	sj_quote(qn, sizeof qn, name);
	// no opponentName: the friend's own name shows once they open the link
	snprintf(body, sizeof body, "{\"mode\":\"duel\",\"name\":%s,\"as\":\"person\",\"ruleset\":\"%s\",\"opponent\":\"person\"}", qn, ruleset_name(ruleset));
	return start(a, "games.createDuel", body, g);
}

int snc_host_code(snc_api *a, const char *name, char ruleset, snc_game *g) {
	char body[256], qn[120];
	sj_quote(qn, sizeof qn, name);
	snprintf(body, sizeof body, "{\"mode\":\"duel\",\"name\":%s,\"as\":\"person\",\"ruleset\":\"%s\"}", qn, ruleset_name(ruleset));
	return start(a, "games.createDuel", body, g);
}

int snc_join_code(snc_api *a, const char *code, const char *name, snc_game *g) {
	char body[256], qn[120], qc[24];
	sj_quote(qn, sizeof qn, name);
	sj_quote(qc, sizeof qc, code);
	snprintf(body, sizeof body, "{\"code\":%s,\"name\":%s,\"as\":\"person\"}", qc, qn);
	sj j;
	int r = call(a, "games.joinDuel", "POST", "/api/v1/games/join", NULL, body, 30000, &j);
	if (r) return r;
	memset(g, 0, sizeof *g);
	parse_game(&j, sj_key(&j, 0, "game"), g);
	copy_str(&j, sj_key(&j, 0, "playerToken"), g->token, sizeof g->token);
	return 0;
}

// A call that answers with the game itself (moves, wait, resign, refresh).
static int game_call(snc_api *a, snc_game *g, const char *op, const char *method, const char *path, const char *body, int timeout_ms) {
	sj j;
	int r = call(a, op, method, path, g->token, body, timeout_ms, &j);
	if (r) return r;
	parse_game(&j, 0, g);
	return 0;
}

int snc_refresh(snc_api *a, snc_game *g) {
	char path[96];
	snprintf(path, sizeof path, "/api/v1/games/%s", g->id);
	return game_call(a, g, "games.resume", "GET", path, NULL, 30000);
}

int snc_play(snc_api *a, snc_game *g, const char *move) {
	char path[96], body[128], qm[80];
	snprintf(path, sizeof path, "/api/v1/games/%s/moves", g->id);
	sj_quote(qm, sizeof qm, move);
	snprintf(body, sizeof body, "{\"move\":%s}", qm);
	return game_call(a, g, "game.play", "POST", path, body, 40000);  // against the house, the reply comes with it
}

int snc_wait(snc_api *a, snc_game *g, int seconds) {
	char path[96];
	if (seconds < 1) seconds = 1;
	if (seconds > 20) seconds = 20;
	snprintf(path, sizeof path, "/api/v1/games/%s/wait?timeout=%d", g->id, seconds);
	return game_call(a, g, "game.waitForTurn", "GET", path, NULL, (seconds + 15) * 1000);
}

int snc_resign(snc_api *a, snc_game *g) {
	char path[96];
	snprintf(path, sizeof path, "/api/v1/games/%s/resign", g->id);
	return game_call(a, g, "game.resign", "POST", path, NULL, 30000);
}

int snc_opponent_hand(snc_api *a, snc_game *g, int *count) {
	char path[112];
	snprintf(path, sizeof path, "/api/v1/games/%s/sync?since=%d", g->id, g->move_count);
	sj j;
	int r = call(a, "game.sync", "GET", path, g->token, NULL, 30000, &j);
	if (r) return r;
	*count = (int)sj_int(&j, sj_path(&j, 0, "state.opponentHand"), 5);
	return 0;
}

int snc_review(snc_api *a, const char *game_id, snc_review_t *rv) {
	char path[96];
	snprintf(path, sizeof path, "/api/v1/games/%s/review", game_id);
	sj j;
	memset(rv, 0, sizeof *rv);
	int r = call(a, "games.review", "GET", path, NULL, NULL, 30000, &j);
	if (r) return r;
	int acc = sj_key(&j, 0, "accuracy");
	rv->accuracy[0] = (int)(sj_num(&j, sj_key(&j, acc, "A"), 0) + 0.5);
	rv->accuracy[1] = (int)(sj_num(&j, sj_key(&j, acc, "B"), 0) + 0.5);
	rv->ok = acc >= 0;
	return 0;
}

int snc_rules(snc_api *a, char *out, size_t cap) {
	sj j;
	int r = call(a, "rules", "GET", "/api/v1/games/rules", NULL, NULL, 30000, &j);
	if (r) return r;
	copy_str(&j, sj_key(&j, 0, "rules"), out, cap);
	return 0;
}
