// The whole client, minus the drawing. See snc_client.h.
#include "snc_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snc_platform.h"

static snc_api *net_api;  // set by the network thread; the UI only uses it to cancel a wait

/* ---------------------------------- record ----------------------------------- */

void snc_record_text(const snc_record *r, char *out, size_t cap) {
	snprintf(out, cap, "rating=%d\nplayed=%d\nwon=%d\nruleset=%c\nid=%s\ntoken=%s\nmode=%c\nstrength=%d\ninvite=%s\ncode=%s\nchamp=%d\n", r->rating, r->played,
	         r->won, r->ruleset ? r->ruleset : 'm', r->id, r->token, r->mode ? r->mode : '-', r->strength, r->invite, r->code, r->champ);
}

static void record_parse(snc_record *r, const char *text) {
	memset(r, 0, sizeof *r);
	r->rating = 1200;
	r->ruleset = 'm';
	r->strength = 1200;
	while (text && *text) {
		const char *eol = strchr(text, '\n');
		size_t n = eol ? (size_t)(eol - text) : strlen(text);
		char line[256];
		if (n >= sizeof line) n = sizeof line - 1;
		memcpy(line, text, n);
		line[n] = 0;
		char *eq = strchr(line, '=');
		if (eq) {
			*eq = 0;
			const char *k = line, *v = eq + 1;
			if (!strcmp(k, "rating")) r->rating = atoi(v);
			else if (!strcmp(k, "played")) r->played = atoi(v);
			else if (!strcmp(k, "won")) r->won = atoi(v);
			else if (!strcmp(k, "ruleset")) r->ruleset = *v == 'c' ? 'c' : 'm';
			else if (!strcmp(k, "id")) snprintf(r->id, sizeof r->id, "%s", v);
			else if (!strcmp(k, "token")) snprintf(r->token, sizeof r->token, "%s", v);
			else if (!strcmp(k, "mode")) r->mode = *v == '-' ? 0 : *v;
			else if (!strcmp(k, "strength")) r->strength = atoi(v);
			else if (!strcmp(k, "champ")) r->champ = atoi(v);
			else if (!strcmp(k, "invite")) snprintf(r->invite, sizeof r->invite, "%s", v);
			else if (!strcmp(k, "code")) snprintf(r->code, sizeof r->code, "%s", v);
		}
		text += n + (eol ? 1 : 0);
		if (!eol) break;
	}
	if (r->rating < 100 || r->rating > 4000) r->rating = 1200;
}

static void forget_active(snc_client *c) {
	c->rec.id[0] = c->rec.token[0] = c->rec.invite[0] = c->rec.code[0] = 0;
	c->rec.mode = 0;
	c->rec_dirty = 1;
}

/* ---------------------------------- helpers ---------------------------------- */

static void say(snc_client *c, const char *text, int ms, int bad) {
	snprintf(c->notice, sizeof c->notice, "%s", text);
	c->notice_until = snc_now_ms() + (uint32_t)ms;
	c->notice_bad = bad;
}

const char *snc_client_notice(const snc_client *c) {
	return c->notice[0] && (int32_t)(c->notice_until - snc_now_ms()) > 0 ? c->notice : NULL;
}

int snc_client_active(const snc_client *c) {
	return c->have_game && (c->game.status == SNC_ACTIVE || c->game.status == SNC_WAITING);
}

int snc_client_over(const snc_client *c) {
	return c->have_game && (c->game.status == SNC_FINISHED || c->game.status == SNC_ABANDONED);
}

static int confirm(snc_client *c, int what, const char *text) {
	if (c->armed == what && (int32_t)(c->armed_until - snc_now_ms()) > 0) {
		c->armed = ARM_NONE;
		return 1;
	}
	c->armed = what;
	c->armed_until = snc_now_ms() + 4000;
	say(c, text, 4000, 0);
	return 0;
}

// A tile's identity (card, owner, frozen), to see what a move changed.
static void owners(const snc_game *g, uint32_t out[15]) {
	for (int i = 0; i < 15; i++) {
		uint32_t h = 2166136261u;
		for (const char *s = g->board[i].card; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
		out[i] = g->board[i].card[0] ? (h ^ ((uint32_t)g->board[i].owner << 8) ^ (uint32_t)g->board[i].frozen) | 1 : 0;
	}
}

static int busy_job(snc_job_type t) {
	return t == JOB_START || t == JOB_JOIN || t == JOB_RESUME || t == JOB_PLAY || t == JOB_RESIGN || t == JOB_RULES;
}

static void queue(snc_client *c, snc_job_type t, char mode) {
	snc_job *j = &c->job;
	j->type = t;
	j->mode = mode;
	j->result = 0;
	j->err[0] = 0;
	snprintf(j->name, sizeof j->name, "%s", c->name);
	j->ruleset = c->rec.ruleset;
	j->strength = c->strength;
	if (t != JOB_START && t != JOB_JOIN && t != JOB_RULES) j->game = c->game;
	if (t == JOB_RESUME) {
		memset(&j->game, 0, sizeof j->game);
		snprintf(j->game.id, sizeof j->game.id, "%s", c->rec.id);
		snprintf(j->game.token, sizeof j->game.token, "%s", c->rec.token);
	}
	j->state = 1;  // the network thread takes it from here
}

static void push(snc_client *c, snc_job_type t, char mode) {
	for (int i = 0; i < c->pend_n; i++)
		if (c->pend[i].type == t && (t == JOB_OPPHAND || t == JOB_REVIEW || t == JOB_WAIT)) return;  // already on its way
	if (c->pend_n < 4) {
		c->pend[c->pend_n].type = t;
		c->pend[c->pend_n].mode = mode;
		c->pend_n++;
	}
}

// Start t now, or as soon as the jobs ahead of it return (a long wait is cut short).
static void request(snc_client *c, snc_job_type t, char mode) {
	if (c->job.state == 0 && c->pend_n == 0) {
		queue(c, t, mode);
		return;
	}
	if (c->job.state != 0 && (c->job.type == JOB_WAIT || c->job.type == JOB_OPPHAND) && net_api) snc_api_cancel(net_api);
	push(c, t, mode);
}

static void opponent(const snc_client *c, char *out, size_t cap) {
	snc_opponent_name(&c->game, out, cap);
}

/* ------------------------------ the network side ----------------------------- */

int snc_client_net_step(snc_client *c, snc_api *api) {
	net_api = api;
	snc_job *j = &c->job;
	if (j->state != 1) return 0;
	j->state = 2;
	int r = 0;
	switch (j->type) {
	case JOB_START:
		if (j->mode == 'h') r = snc_start_house(api, j->name, j->ruleset, j->strength, &j->game);
		else if (j->mode == 'q') r = snc_quick_match(api, j->name, j->ruleset, &j->game);
		else if (j->mode == 'i') r = snc_create_invite(api, j->name, j->ruleset, &j->game);
		else r = snc_host_code(api, j->name, j->ruleset, &j->game);
		break;
	case JOB_JOIN: r = snc_join_code(api, j->arg, j->name, &j->game); break;
	case JOB_RESUME: r = snc_refresh(api, &j->game); break;
	case JOB_PLAY: r = snc_play(api, &j->game, j->arg); break;
	case JOB_WAIT: r = snc_wait(api, &j->game, 20); break;
	case JOB_RESIGN: r = snc_resign(api, &j->game); break;
	case JOB_REVIEW: r = snc_review(api, j->game.id, &j->review); break;
	case JOB_RULES: r = snc_rules(api, j->rules, sizeof j->rules); break;
	case JOB_OPPHAND: r = snc_opponent_hand(api, &j->game, &j->opp_hand); break;
	default: break;
	}
	j->result = r;
	snprintf(j->err, sizeof j->err, "%s", r ? api->err : "");
	j->state = 3;
	return 1;
}

/* -------------------------------- game updates ------------------------------- */

static void finish(snc_client *c) {
	if (c->finished) return;
	c->finished = 1;
	c->pick.selected = -1;
	c->pick.path_n = 0;
	const snc_game *g = &c->game;
	c->rated = 0;
	if (g->status == SNC_FINISHED) {
		int won = g->winner == g->seat;
		c->rec.played++;
		if (won) c->rec.won++;
		if (c->mode == 'h') {
			c->rated = 1;
			c->rating_before = c->rec.rating;
			c->rec.rating = snc_elo(c->rec.rating, c->strength, won ? 1.0 : g->winner == 'd' ? 0.5 : 0.0);
		}
	}
	forget_active(c);
	if (g->move_count > 0 && g->status == SNC_FINISHED) push(c, JOB_REVIEW, 0);
}

static void after_change(snc_client *c, const uint32_t before[15], int have_before) {
	uint32_t after[15];
	owners(&c->game, after);
	c->flash = 0;
	if (have_before)
		for (int i = 0; i < 15; i++)
			if (after[i] != before[i] && after[i]) c->flash |= (uint16_t)(1u << i);
	if (snc_client_over(c)) finish(c);
	else if (c->game.has_view && c->game.draw_pile == 0 && c->game.status == SNC_ACTIVE) push(c, JOB_OPPHAND, 0);
}

static void adopt(snc_client *c, const snc_game *g, char mode) {
	c->game = *g;
	c->have_game = 1;
	c->mode = mode;
	c->screen = SC_GAME;
	c->pick.selected = -1;
	c->pick.path_n = 0;
	c->pending_cell = -1;
	c->flash = 0;
	memset(&c->review, 0, sizeof c->review);
	c->resigned = 0;
	c->finished = 0;
	c->opp_hand = 5;
	c->retry_at = 0;
	c->wait_failures = 0;
	if (!snc_client_over(c)) {
		snprintf(c->rec.id, sizeof c->rec.id, "%s", g->id);
		snprintf(c->rec.token, sizeof c->rec.token, "%s", g->token);
		snprintf(c->rec.invite, sizeof c->rec.invite, "%s", g->invite_url);
		snprintf(c->rec.code, sizeof c->rec.code, "%s", g->code);
		c->rec.mode = mode;
		c->rec.strength = c->strength;
		c->rec_dirty = 1;
	}
	after_change(c, NULL, 0);
}

static int pending_busy(const snc_client *c) {
	for (int i = 0; i < c->pend_n; i++)
		if (busy_job(c->pend[i].type)) return 1;
	return 0;
}

static void apply(snc_client *c) {
	snc_job *j = &c->job;
	int ok = j->result == 0;
	int cancelled = j->result == SNC_E_CANCEL;
	if (busy_job(j->type) && (!ok || !pending_busy(c))) c->busy[0] = 0;
	if (!ok && busy_job(j->type) && !cancelled) {
		// a failed step drops what was to follow it (resign failed: do not start the next game)
		int keep = 0;
		for (int i = 0; i < c->pend_n; i++)
			if (!busy_job(c->pend[i].type)) c->pend[keep++] = c->pend[i];
		c->pend_n = keep;
	}
	switch (j->type) {
	case JOB_START:
	case JOB_JOIN:
		if (ok) adopt(c, &j->game, j->type == JOB_JOIN ? 'j' : j->mode);
		else {
			say(c, j->err, 6000, 1);
			if (!c->have_game) c->screen = c->screen == SC_CODE ? SC_CODE : SC_LOBBY;
		}
		break;
	case JOB_RESUME:
		if (ok && j->game.status != SNC_ABANDONED && j->game.status != SNC_FINISHED) {
			snprintf(j->game.invite_url, sizeof j->game.invite_url, "%s", c->rec.invite);
			if (!j->game.code[0]) snprintf(j->game.code, sizeof j->game.code, "%s", c->rec.code);
			c->strength = c->rec.strength;
			adopt(c, &j->game, c->rec.mode ? c->rec.mode : 'h');
			say(c, "Picked up your game where you left it.", 3500, 0);
		} else if (ok || j->result > 0) {
			forget_active(c);  // over, or gone: start fresh
		} else {
			say(c, j->err, 6000, 1);
		}
		break;
	case JOB_PLAY:
	case JOB_WAIT:
	case JOB_RESIGN: {
		if (!c->have_game || strcmp(j->game.id, c->game.id)) break;  // a game we have since left
		c->pending_cell = -1;
		if (cancelled) break;
		if (!ok) {
			if (j->type == JOB_WAIT) {
				c->wait_failures++;
				c->retry_at = snc_now_ms() + (c->wait_failures > 3 ? 8000 : 3000);
				if (c->wait_failures > 1) say(c, j->err, 5000, 1);
			} else {
				say(c, j->err, 6000, 1);
				if (j->type == JOB_PLAY) push(c, JOB_WAIT, 0);  // see where the game really is
			}
			break;
		}
		c->wait_failures = 0;
		uint32_t before[15];
		owners(&c->game, before);
		int was_waiting = c->game.status == SNC_WAITING;
		char invite[200], code[8], token[64];
		snprintf(invite, sizeof invite, "%s", c->game.invite_url);
		snprintf(code, sizeof code, "%s", c->game.code);
		snprintf(token, sizeof token, "%s", c->game.token);
		c->game = j->game;
		snprintf(c->game.invite_url, sizeof c->game.invite_url, "%s", invite);
		snprintf(c->game.token, sizeof c->game.token, "%s", token);
		if (!c->game.code[0]) snprintf(c->game.code, sizeof c->game.code, "%s", code);
		if (j->type == JOB_RESIGN && was_waiting) {
			c->have_game = 0;
			c->screen = SC_LOBBY;
			forget_active(c);
			say(c, "Called off.", 3000, 0);
			break;
		}
		if (j->type == JOB_RESIGN) c->resigned = 1;
		if (was_waiting && c->game.status == SNC_ACTIVE) {
			char opp[48];
			opponent(c, opp, sizeof opp);
			char line[96];
			snprintf(line, sizeof line, "%s is here. Game on!", opp);
			say(c, line, 3500, 0);
			after_change(c, NULL, 0);
		} else {
			after_change(c, before, 1);
		}
		break;
	}
	case JOB_REVIEW:
		if (ok && c->have_game && !strcmp(j->game.id, c->game.id)) c->review = j->review;
		break;
	case JOB_RULES:
		if (ok) {
			memcpy(c->rules, j->rules, sizeof c->rules);
			c->rules_ready = 1;
		} else {
			say(c, j->err, 6000, 1);
			c->rules_asked = 0;
			if (c->screen == SC_RULES && !c->rules_ready) c->screen = c->have_game ? SC_GAME : SC_LOBBY;
		}
		break;
	case JOB_OPPHAND:
		if (ok && c->have_game && !strcmp(j->game.id, c->game.id)) c->opp_hand = j->opp_hand;
		break;
	default: break;
	}
}

void snc_client_tick(snc_client *c) {
	if (c->job.state == 3) {
		apply(c);
		c->job.state = 0;
	}
	if (c->job.state == 0 && c->pend_n > 0) {
		snc_job_type t = c->pend[0].type;
		char mode = c->pend[0].mode;
		memmove(c->pend, c->pend + 1, sizeof c->pend[0] * (size_t)(--c->pend_n));
		if (t == JOB_JOIN) snprintf(c->job.arg, sizeof c->job.arg, "%s", c->code);
		int stale = (t == JOB_WAIT || t == JOB_OPPHAND) && !snc_client_active(c);
		if (!stale) queue(c, t, mode);
	}
	// the other side's turn (or nobody has joined yet): wait for news, one long poll at a time
	if (c->job.state == 0 && c->pend_n == 0 && snc_client_active(c) && !c->game.your_turn && (int32_t)(snc_now_ms() - c->retry_at) >= 0)
		queue(c, JOB_WAIT, 0);
	if (c->armed && (int32_t)(c->armed_until - snc_now_ms()) <= 0) c->armed = ARM_NONE;
}

/* ---------------------------------- set up ----------------------------------- */

void snc_client_init(snc_client *c, const char *name, const char *platform, const char *record_text) {
	memset(c, 0, sizeof *c);
	snprintf(c->name, sizeof c->name, "%s", name && *name ? name : "Player");
	c->platform = platform;
	record_parse(&c->rec, record_text);
	c->strength = c->rec.strength;
	c->pick.selected = -1;
	c->pending_cell = -1;
	c->screen = SC_LOBBY;
}

void snc_client_start(snc_client *c) {
	if (c->rec.id[0] && c->rec.token[0]) {
		snprintf(c->busy, sizeof c->busy, "Picking up your game...");
		request(c, JOB_RESUME, 0);
	}
}

/* ---------------------------------- actions ---------------------------------- */

static int ready(snc_client *c) {
	return !c->busy[0];
}

static void begin_start(snc_client *c, char mode) {
	static const char *const what[] = {"Dealing...", "Joining the queue...", "Opening a game...", "Opening a game..."};
	int k = mode == 'h' ? 0 : mode == 'q' ? 1 : mode == 'i' ? 2 : 3;
	snprintf(c->busy, sizeof c->busy, "%s", what[k]);
	if (mode == 'h') {
		int s = c->rec.rating;
		c->strength = s < 800 ? 800 : s > 1600 ? 1600 : s;
	}
}

void snc_act_start(snc_client *c, char mode) {
	if (!ready(c)) return;
	int arm = mode == 'h' ? ARM_NEW_HOUSE : mode == 'q' ? ARM_NEW_QUICK : mode == 'i' ? ARM_NEW_INVITE : ARM_NEW_CODE;
	int active = snc_client_active(c);
	if (active && c->game.status == SNC_ACTIVE && !confirm(c, arm, "Press again to resign this game and start another.")) return;
	begin_start(c, mode);
	c->pend_n = 0;  // nothing queued for the old game matters now
	if (active) {
		// resign (or call off) the game in hand, then start
		request(c, JOB_RESIGN, 0);
		push(c, JOB_START, mode);
		forget_active(c);
	} else {
		request(c, JOB_START, mode);
	}
	c->have_game = 0;
	c->screen = SC_LOBBY;
}

void snc_act_join(snc_client *c) {
	if (!ready(c)) return;
	if (c->code_len != 6) {
		say(c, "A duel code has 6 letters and digits.", 3500, 1);
		return;
	}
	snprintf(c->busy, sizeof c->busy, "Joining %s...", c->code);
	c->pend_n = 0;
	if (snc_client_active(c)) {
		if (c->game.status == SNC_ACTIVE && !confirm(c, ARM_JOIN, "Press again to resign this game and join that one.")) {
			c->busy[0] = 0;
			return;
		}
		request(c, JOB_RESIGN, 0);
		push(c, JOB_JOIN, 0);
		forget_active(c);
	} else if (c->job.state == 0) {
		snprintf(c->job.arg, sizeof c->job.arg, "%s", c->code);
		queue(c, JOB_JOIN, 0);
	} else {
		request(c, JOB_JOIN, 0);
	}
	c->have_game = 0;
}

void snc_act_resign(snc_client *c) {
	if (!ready(c) || !snc_client_active(c)) return;
	int waiting = c->game.status == SNC_WAITING;
	if (!waiting && !confirm(c, ARM_RESIGN, "Press Resign again to concede the game.")) return;
	snprintf(c->busy, sizeof c->busy, "%s", waiting ? "Calling it off..." : "Resigning...");
	request(c, JOB_RESIGN, 0);
}

static void play(snc_client *c, int k) {
	if (k < 0 || k >= c->game.moves_n) return;
	snc_move m;
	snc_parse_move(c->game.moves[k], &m);
	const snc_hcard *card = c->pick.selected >= 0 && c->pick.selected < c->game.hand_n ? &c->game.hand[c->pick.selected] : NULL;
	c->pending_cell = -1;
	if (card && !card->effect && m.ncells == 1) {
		c->pending_cell = m.cells[0];
		snprintf(c->pending_card, sizeof c->pending_card, "%s", card->card);
	}
	c->pick.selected = -1;
	c->pick.path_n = 0;
	if (c->mode == 'h') {
		char opp[48];
		opponent(c, opp, sizeof opp);
		snprintf(c->busy, sizeof c->busy, "%s is thinking...", opp);
	} else {
		snprintf(c->busy, sizeof c->busy, "Playing %s...", c->game.moves[k]);
	}
	snprintf(c->job.arg, sizeof c->job.arg, "%s", c->game.moves[k]);
	request(c, JOB_PLAY, 0);
}

void snc_act_hand(snc_client *c, int i) {
	snc_game *g = &c->game;
	if (!ready(c) || !c->have_game || !g->your_turn || g->has_hop || i < 0 || i >= g->hand_n) return;
	if (!snc_playable(g, i)) {
		char line[96];
		snprintf(line, sizeof line, "%s has nothing to do right now.", g->hand[i].card);
		say(c, line, 3000, 0);
		return;
	}
	if (c->pick.selected == i) {
		int now = snc_immediate(g, &c->pick);
		if (now >= 0) {
			play(c, now);
			return;
		}
		c->pick.selected = -1;
	} else {
		c->pick.selected = i;
	}
	c->pick.path_n = 0;
}

void snc_act_cell(snc_client *c, int cell) {
	snc_game *g = &c->game;
	if (!ready(c) || !c->have_game || !g->your_turn || cell < 0 || cell > 14) return;
	if (!(snc_targets(g, &c->pick) & (1u << cell))) {
		c->pick.path_n = 0;  // a tile that goes nowhere starts the pick over
		return;
	}
	c->pick.path[c->pick.path_n++] = (char)cell;
	int done = snc_complete(g, &c->pick);
	if (done >= 0) play(c, done);
}

void snc_act_action(snc_client *c) {
	snc_status_t st;
	snc_client_status(c, &st);
	if (!ready(c)) return;
	if (st.action_id == ACT_STAY) play(c, snc_hop_stay(&c->game));
	else if (st.action_id == ACT_PLAY_EFFECT) play(c, snc_immediate(&c->game, &c->pick));
}

void snc_act_lobby(snc_client *c) {
	if (!ready(c) || snc_client_active(c)) return;
	c->have_game = 0;
	c->screen = SC_LOBBY;
	c->pick.selected = -1;
}

void snc_act_toggle_rules(snc_client *c) {
	c->rec.ruleset = c->rec.ruleset == 'c' ? 'm' : 'c';
	c->rec_dirty = 1;
	say(c, snc_client_active(c) ? "The next game uses these rules." : c->rec.ruleset == 'c' ? "Rules: Classic." : "Rules: Mutators.", 2500, 0);
}

void snc_act_how_to_play(snc_client *c) {
	c->screen = SC_RULES;
	if (!c->rules_ready && !c->rules_asked) {
		c->rules_asked = 1;
		request(c, JOB_RULES, 0);
	}
}

void snc_act_code_screen(snc_client *c) {
	c->screen = SC_CODE;
}

void snc_act_back(snc_client *c) {
	c->screen = c->have_game ? SC_GAME : SC_LOBBY;
}

/* ---------------------------------- status ----------------------------------- */

void snc_client_status(const snc_client *c, snc_status_t *st) {
	memset(st, 0, sizeof *st);
	const snc_game *g = &c->game;
	char opp[48] = "";
	if (c->have_game) opponent(c, opp, sizeof opp);
	if (c->busy[0]) {
		snprintf(st->main, sizeof st->main, "%s", c->busy);
		return;
	}
	if (!c->have_game) {
		snprintf(st->main, sizeof st->main, "Pick a game to start.");
		snprintf(st->detail, sizeof st->detail,
		         "New game: play now, against an opponent at your level.\nQuick match: whoever is online.\nInvite a friend: they play you in their browser.\nPlay by code: another console, or a terminal.");
		return;
	}
	if (g->status == SNC_WAITING) {
		if (c->mode == 'i') {
			snprintf(st->main, sizeof st->main, "Waiting for your friend...");
			snprintf(st->detail, sizeof st->detail, "Scan the code with a phone, or open the link:\n%s", g->invite_url);
		} else if (c->mode == 'c') {
			snprintf(st->main, sizeof st->main, "Waiting for a player...");
			snprintf(st->detail, sizeof st->detail, "Your code: %s\nOn another console: Play by code, then Join.\nIn a terminal: npx smashandclash duel join %s", g->code, g->code);
		} else {
			snprintf(st->main, sizeof st->main, "Looking for an opponent...");
			snprintf(st->detail, sizeof st->detail, "You are in the quick-match queue. Cancel to leave it.");
		}
		return;
	}
	if (snc_client_over(c)) {
		int you = g->score_you, them = g->score_opp;
		if (g->status == SNC_ABANDONED) snprintf(st->main, sizeof st->main, "Game called off.");
		else if (c->resigned) snprintf(st->main, sizeof st->main, "You resigned.");
		else if (g->winner == g->seat) snprintf(st->main, sizeof st->main, "You win, %d-%d!", you, them);
		else if (g->winner == 'd') snprintf(st->main, sizeof st->main, "A draw, %d-%d.", you, them);
		else snprintf(st->main, sizeof st->main, "%s wins, %d-%d.", opp, them, you);
		size_t n = 0;
		if (c->review.ok) {
			int mine = g->seat == 'B' ? 1 : 0;
			n += (size_t)snprintf(st->detail, sizeof st->detail, "Accuracy: you %d%%, %s %d%%.", c->review.accuracy[mine], opp, c->review.accuracy[1 - mine]);
		}
		if (c->rated && n < sizeof st->detail)
			snprintf(st->detail + n, sizeof st->detail - n, "%sRating %d \xE2\x86\x92 %d.", n ? "\n" : "", c->rating_before, c->rec.rating);
		return;
	}
	if (!g->your_turn) {
		snprintf(st->main, sizeof st->main, "%s is playing...", opp);
		return;
	}
	if (g->has_hop) {
		const snc_tile *t = g->hop_from >= 0 && g->hop_from < 15 ? &g->board[(int)g->hop_from] : NULL;
		const char *piece = "chess piece";
		for (int i = 0; i < g->chess_n; i++)
			if (g->chess[i].cell == g->hop_from) piece = g->chess[i].piece;
		char from[3];
		snc_cell_name(g->hop_from, from);
		snprintf(st->main, sizeof st->main, "Hop! Pick a green tile.");
		snprintf(st->detail, sizeof st->detail, "%s landed on a %s tile: it may hop like a %s and attack again.", t && t->card[0] ? t->card : "Your card", piece,
		         piece);
		if (snc_hop_stay(g) >= 0) {
			snprintf(st->action, sizeof st->action, "Stay on %s", from);
			st->action_id = ACT_STAY;
		}
		return;
	}
	const snc_hcard *card = c->pick.selected >= 0 && c->pick.selected < g->hand_n ? &g->hand[c->pick.selected] : NULL;
	if (!card) {
		snprintf(st->main, sizeof st->main, "Your turn. Pick a card.");
		return;
	}
	if (!card->effect) {
		snprintf(st->main, sizeof st->main, "Place %s on a green tile.", card->card);
		snprintf(st->detail, sizeof st->detail, "Pick the card again to put it back.");
		return;
	}
	if (snc_immediate(g, &c->pick) >= 0) {
		snprintf(st->main, sizeof st->main, "Pick %s again to play it.", card->card);
		snprintf(st->detail, sizeof st->detail, "%s", card->does);
		snprintf(st->action, sizeof st->action, "Play %s", card->card);
		st->action_id = ACT_PLAY_EFFECT;
		return;
	}
	if (c->pick.path_n) snprintf(st->main, sizeof st->main, "Now pick an empty tile for it.");
	else snprintf(st->main, sizeof st->main, "%s Pick a green tile.", card->card);
	snprintf(st->detail, sizeof st->detail, "%s", card->does);
}
