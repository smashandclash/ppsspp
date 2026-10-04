// The whole client, minus the drawing: screens, the game in progress, what is picked,
// notices, the player's record, and the network job the UI thread hands to the
// network thread. Every platform draws this state its own way and turns its own
// input (touch, buttons) into the actions below.
//
// Threads: the UI thread calls snc_client_tick() every frame and the actions; the
// network thread loops on snc_client_net_step(), which runs at most one job.
#ifndef SNC_CLIENT_H
#define SNC_CLIENT_H

#include <stdint.h>

#include "snc_api.h"
#include "snc_game.h"

typedef enum { SC_LOBBY, SC_GAME, SC_RULES, SC_CODE } sc_screen;

typedef enum {
	JOB_NONE,
	JOB_START,     // mode: 'h' house, 'q' quick match, 'i' invite a friend, 'c' host by code
	JOB_JOIN,      // join a duel by code
	JOB_RESUME,    // pick up the saved game
	JOB_PLAY,
	JOB_WAIT,
	JOB_RESIGN,
	JOB_REVIEW,
	JOB_RULES,
	JOB_OPPHAND,
} snc_job_type;

typedef struct {
	volatile int state;  // 0 idle, 1 queued, 2 running, 3 done
	snc_job_type type;
	char mode;
	char arg[48];        // a move name, or a duel code
	char name[48];
	char ruleset;
	int strength;
	snc_game game;       // in: the game to act on; out: the game after
	int result;          // 0, or the error
	char err[200];
	snc_review_t review;
	int opp_hand;
	char rules[2200];
} snc_job;

// What survives a restart (each platform stores it its own way).
typedef struct {
	int rating, played, won;
	char ruleset;        // 'm' / 'c'
	char id[32], token[64], mode, invite[200], code[8];  // the game in progress
	int strength;
	int champ;           // the champion you play as (a card id, 1..46; 0 = not chosen)
} snc_record;

typedef struct {
	char main[160];
	char detail[400];
	char action[48];  // a contextual button ("Stay on C2", "Play Flip!"), or ""
	int action_id;    // ACT_*
} snc_status_t;

enum { ACT_NONE, ACT_STAY, ACT_PLAY_EFFECT };

typedef struct {
	snc_record rec;
	int rec_dirty;       // the platform saves the record when set (and clears it)
	char name[48];       // the player's name (the console's nickname)
	const char *platform;  // "Nintendo DS" / "PSP": for the copy
	sc_screen screen;

	snc_game game;
	int have_game;
	char mode;           // the game's mode ('h', 'q', 'i', 'c', 'j')
	int strength;        // the house opponent's rating

	snc_pick pick;
	int pending_cell;    // a placement on its way (-1 = none)
	char pending_card[24];
	uint16_t flash;      // tiles that changed with the last move
	char busy[96];       // what we are waiting on; "" = nothing
	char notice[220];
	uint32_t notice_until;
	int notice_bad;
	int armed;           // a control waiting for its confirming second press (ARM_*)
	uint32_t armed_until;
	snc_review_t review;
	int resigned;
	int finished;        // the end of this game has been handled
	int rated;           // the game moved your rating (a game at your level)
	int rating_before;
	int opp_hand;
	int rules_ready;
	char rules[2200];
	char code[8];        // the duel code being typed
	int code_len;

	snc_job job;
	struct { snc_job_type type; char mode; } pend[4];  // jobs to start, in order, once the current one returns
	int pend_n;
	int rules_asked;
	uint32_t retry_at;   // after a failed wait, when to wait again
	int wait_failures;
} snc_client;

enum { ARM_NONE, ARM_RESIGN, ARM_NEW_HOUSE, ARM_NEW_QUICK, ARM_NEW_INVITE, ARM_NEW_CODE, ARM_JOIN };

void snc_client_init(snc_client *c, const char *name, const char *platform, const char *record_text);
// After the network is up: resume the saved game, if any.
void snc_client_start(snc_client *c);
void snc_client_tick(snc_client *c);

// The network thread's loop body: runs the queued job, if any. Returns 1 if it ran one.
int snc_client_net_step(snc_client *c, snc_api *api);

/* actions (UI thread) */
void snc_act_start(snc_client *c, char mode);  // 'h' new game, 'q' quick match, 'i' invite, 'c' host by code
void snc_act_join(snc_client *c);              // join with c->code
void snc_act_resign(snc_client *c);            // resign (asks twice), or call off a waiting game
void snc_act_hand(snc_client *c, int i);       // pick a hand card (again: put it back / play it)
void snc_act_cell(snc_client *c, int cell);    // pick a board tile
void snc_act_action(snc_client *c);            // the status's contextual button
void snc_act_lobby(snc_client *c);             // leave a finished game
void snc_act_toggle_rules(snc_client *c);      // Mutators <-> Classic
void snc_act_how_to_play(snc_client *c);
void snc_act_code_screen(snc_client *c);
void snc_act_back(snc_client *c);

void snc_client_status(const snc_client *c, snc_status_t *st);
const char *snc_client_notice(const snc_client *c);  // the live notice, or NULL
int snc_client_active(const snc_client *c);          // a game not over yet
int snc_client_over(const snc_client *c);

// The record as text (key=value lines), for the platform to save.
void snc_record_text(const snc_record *r, char *out, size_t cap);

#endif
