// The client's rules of thumb, shared by every platform. See snc_game.h.
#include "snc_game.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char ARROW[] = "\xE2\x86\x92";  // "→" in UTF-8

int snc_parse_move(const char *name, snc_move *m) {
	memset(m, 0, sizeof *m);
	size_t len = strlen(name);
	// "Pengu@C2" / "Teddy!C2"
	if (len >= 4 && (name[len - 3] == '@' || name[len - 3] == '!') && snc_cell(name + len - 2) >= 0) {
		size_t k = len - 3 < sizeof m->key - 1 ? len - 3 : sizeof m->key - 1;
		memcpy(m->key, name, k);
		m->cells[0] = (char)snc_cell(name + len - 2);
		m->ncells = 1;
		return 1;
	}
	// "hop→E3" / "hop: stay"
	if (!strncmp(name, "hop", 3)) {
		strcpy(m->key, "hop");
		if (!strncmp(name + 3, ARROW, 3) && snc_cell(name + 6) >= 0) {
			m->cells[0] = (char)snc_cell(name + 6);
			m->ncells = 1;
		}
		return 1;
	}
	// "BOULDER(D2)" / "RECRUIT(D3→B1)"
	const char *open = strchr(name, '(');
	if (open && len && name[len - 1] == ')') {
		size_t k = (size_t)(open - name);
		if (k >= sizeof m->key) k = sizeof m->key - 1;
		memcpy(m->key, name, k);
		const char *p = open + 1;
		while (*p && *p != ')' && m->ncells < 2) {
			int c = snc_cell(p);
			if (c < 0) break;
			m->cells[m->ncells++] = (char)c;
			p += 2;
			if (!strncmp(p, ARROW, 3)) p += 3;
		}
		return 1;
	}
	snprintf(m->key, sizeof m->key, "%s", name);  // "FLIP", "SWAP"
	return 1;
}

void snc_hand_key(const snc_hcard *c, char *key, size_t cap) {
	size_t n = 0;
	if (!c->effect) {
		snprintf(key, cap, "%s", c->card);
		return;
	}
	for (const char *s = c->card; *s && *s != '!' && n + 1 < cap; s++) key[n++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
	key[n] = 0;
}

int snc_playable(const snc_game *g, int i) {
	if (i < 0 || i >= g->hand_n) return 0;
	char key[24];
	snc_hand_key(&g->hand[i], key, sizeof key);
	for (int k = 0; k < g->moves_n; k++) {
		snc_move m;
		snc_parse_move(g->moves[k], &m);
		if (!strcmp(m.key, key)) return 1;
	}
	return 0;
}

// Whether move k belongs to the pick's card (or is a hop, during one).
static int candidate(const snc_game *g, const snc_pick *p, int k, snc_move *m) {
	snc_parse_move(g->moves[k], m);
	if (g->has_hop) return !strcmp(m->key, "hop");
	if (p->selected < 0 || p->selected >= g->hand_n) return 0;
	char key[24];
	snc_hand_key(&g->hand[p->selected], key, sizeof key);
	return !strcmp(m->key, key);
}

static int follows(const snc_move *m, const snc_pick *p) {
	if (m->ncells <= p->path_n) return 0;
	for (int i = 0; i < p->path_n; i++)
		if (m->cells[i] != p->path[i]) return 0;
	return 1;
}

uint16_t snc_targets(const snc_game *g, const snc_pick *p) {
	uint16_t out = 0;
	if (!g->your_turn) return 0;
	for (int k = 0; k < g->moves_n; k++) {
		snc_move m;
		if (candidate(g, p, k, &m) && follows(&m, p)) out |= (uint16_t)(1u << m.cells[p->path_n]);
	}
	return out;
}

int snc_immediate(const snc_game *g, const snc_pick *p) {
	if (!g->your_turn || g->has_hop) return -1;
	for (int k = 0; k < g->moves_n; k++) {
		snc_move m;
		if (candidate(g, p, k, &m) && m.ncells == 0) return k;
	}
	return -1;
}

int snc_complete(const snc_game *g, const snc_pick *p) {
	if (!g->your_turn) return -1;
	for (int k = 0; k < g->moves_n; k++) {
		snc_move m;
		if (!candidate(g, p, k, &m) || m.ncells != p->path_n || m.ncells == 0) continue;
		if (!memcmp(m.cells, p->path, (size_t)p->path_n)) return k;
	}
	return -1;
}

int snc_hop_stay(const snc_game *g) {
	if (!g->has_hop) return -1;
	for (int k = 0; k < g->moves_n; k++)
		if (!strncmp(g->moves[k], "hop:", 4)) return k;
	return -1;
}

/* --------------------------------- personas ---------------------------------- */

static const char *const ADJECTIVES[] = {"Brave", "Swift", "Sneaky", "Mighty", "Wobbly", "Spicy", "Turbo", "Cosmic", "Jolly", "Rapid", "Zesty",
                                         "Lucky", "Cheeky", "Bouncy", "Fuzzy", "Nifty", "Bold", "Snappy", "Plucky", "Stormy", "Frosty", "Toasty"};
static const char *const NOUNS[] = {"Otter", "Tiger", "Pengu", "Yeti", "Falcon", "Walrus", "Comet", "Ninja", "Badger", "Phoenix", "Koala",
                                    "Bison", "Hawk", "Panda", "Wombat", "Moose", "Lynx", "Heron", "Gecko", "Puffin", "Quokka", "Ibex"};
static const char *const FIRST_NAMES[] = {"arjun", "priya", "rohan", "aisha", "kabir", "meera", "ishan", "tara", "zoya",
                                          "sana", "kenji", "yuki", "mei", "jisoo", "hana", "sora", "leo", "maya",
                                          "nina", "omar", "theo", "aria", "finn", "luca", "noor", "ivy"};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

// The same generator as the JavaScript clients (FNV-1a seed, mulberry32), so a game
// shows the same name everywhere.
typedef struct {
	uint32_t a;
} rng_t;

static double rng_next(rng_t *r) {
	r->a += 0x6d2b79f5u;
	uint32_t t = (r->a ^ (r->a >> 15)) * (1u | r->a);
	t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
	return (double)(t ^ (t >> 14)) / 4294967296.0;
}

static const char *pick(rng_t *r, const char *const *xs, int n) {
	int i = (int)floor(rng_next(r) * n);
	return xs[i < n - 1 ? i : n - 1];
}

void snc_persona(const char *game_id, char *out, size_t cap) {
	rng_t r = {2166136261u};
	for (const unsigned char *s = (const unsigned char *)game_id; *s; s++) r.a = (r.a ^ *s) * 16777619u;
	double roll = rng_next(&r);
	if (roll < 0.45) {
		const char *adj = pick(&r, ADJECTIVES, COUNT(ADJECTIVES));
		const char *noun = pick(&r, NOUNS, COUNT(NOUNS));
		snprintf(out, cap, "%s%s%d", adj, noun, (int)floor(rng_next(&r) * 1000));
		return;
	}
	const char *first = pick(&r, FIRST_NAMES, COUNT(FIRST_NAMES));
	if (roll < 0.7) snprintf(out, cap, "%s", first);
	else if (roll < 0.85) snprintf(out, cap, "%s%d", first, (int)floor(rng_next(&r) * 90) + 10);
	else snprintf(out, cap, "%s_%c", first, (char)(97 + (int)floor(rng_next(&r) * 26)));
}

void snc_opponent_name(const snc_game *g, char *out, size_t cap) {
	int other = g->seat == 'B' ? 0 : 1;
	if (g->kinds[other] == 'h') snc_persona(g->id, out, cap);
	else if (g->players[other][0]) snprintf(out, cap, "%s", g->players[other]);
	else snprintf(out, cap, "Opponent");
}

int snc_elo(int rating, int opp, double score) {
	double expected = 1.0 / (1.0 + pow(10.0, (opp - rating) / 400.0));
	return (int)lround(rating + 32.0 * (score - expected));
}
