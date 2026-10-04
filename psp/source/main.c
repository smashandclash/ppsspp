// Smash&Clash for the PSP: a client of the real game at smashandclash.in, written
// against the Smash&Clash API (the one @smashandclash/sdk wraps).
//
// Plays in PPSSPP (Settings > Networking > Enable networking/WLAN) and on a PSP with
// custom firmware and a Wi-Fi connection. The PSP's GPU draws it, in the web edition's
// look (view3d.c); a lower-priority thread does the networking.
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psppower.h>
#include <psputility.h>
#include <stdio.h>
#include <string.h>

#include "assets.h"
#include "gx.h"
#include "snc_client.h"
#include "snc_draw.h"
#include "snc_platform.h"
#include "view3d.h"

PSP_MODULE_INFO("Smash&Clash", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define VERSION "1.0.0"
#define USER_AGENT "smashandclash-psp/" VERSION " (PSP; +https://github.com/smashandclash/ppsspp)"
#define SDK_TAG "smashandclash-c/" VERSION " (psp)"

static snc_client client;
static snc_api api;
static view_t view;
static char record_path[256];
static volatile int net_ok = -1;

/* --------------------------------- the system --------------------------------- */

static int exit_cb(int a, int b, void *c) {
	(void)a, (void)b, (void)c;
	sceKernelExitGame();
	return 0;
}

static int callback_thread(SceSize args, void *argp) {
	(void)args, (void)argp;
	int cb = sceKernelCreateCallback("exit", exit_cb, NULL);
	sceKernelRegisterExitCallback(cb);
	sceKernelSleepThreadCB();
	return 0;
}

// Before the game: bringing the network up, or why it would not come up.
static void boot_screen(const char *title, const char *text, int bad) {
	gx_begin(0x2E9FEA);
	gx_rect(0, 0, 480, 272, gcol(0xA9E6FF, 255), gcol(0x2E9FEA, 255));
	gx_glow(120, 120, 200, 120, gcol(0xFFFFFF, 110));
	gx_sprite(TEX_LOGO, 0, 0, 256, 128, 12, 60, 220, 110, gcol(0xFFFFFF, 255), 0);
	gx_text_c(&tfont_label, 122, 196, "Played online at smashandclash.in", gcol(C_INK, 255));
	gx_rrect(250, 14, 216, 244, 12, gcol(C_INK, 225), gcol(C_INK, 225), 0);
	int lines = gx_wrap(&tfont_label, 262, 22, 192, tfont_label.line, title, gcol(bad ? 0xFF8CC8 : C_SUN1, 255), 3);
	gx_wrap(&tfont_small, 262, 30 + lines * tfont_label.line, 192, tfont_small.line, text, gcol(C_SUGAR, 255), 14);
	gx_end();
}

static const char NET_HELP[] =
	"PPSSPP: Settings > Networking > Enable networking/WLAN, then start the game again.\n"
	"A PSP: set up a Wi-Fi connection in the system settings (Network Settings > Infrastructure Mode). "
	"The game uses the first one.";

static int start_network(void) {
	if (sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON) < 0 || sceUtilityLoadNetModule(PSP_NET_MODULE_INET) < 0) return -1;
	if (sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024) < 0) return -1;
	if (sceNetInetInit() < 0 || sceNetApctlInit(0x8000, 48) < 0) return -1;
	sceNetResolverInit();
	return 0;
}

static int connect_ap(void) {
	if (sceNetApctlConnect(1) < 0) return -1;
	uint32_t until = snc_now_ms() + 30000;
	int last = -1, seen_progress = 0;
	while ((int32_t)(snc_now_ms() - until) < 0) {
		int state = 0;
		if (sceNetApctlGetState(&state) < 0) return -1;
		if (state == PSP_NET_APCTL_STATE_GOT_IP) return 0;
		if (state > PSP_NET_APCTL_STATE_DISCONNECTED) seen_progress = 1;
		else if (seen_progress) return -1;  // it tried, and dropped back
		if (state != last) {
			static const char *const what[] = {"Connecting...", "Scanning...", "Joining the access point...", "Getting an address..."};
			boot_screen(state >= 0 && state < 4 ? what[state] : "Connecting...", NET_HELP, 0);
			last = state;
		}
		sceKernelDelayThread(50 * 1000);
	}
	return -1;
}

static int net_thread(SceSize args, void *argp) {
	(void)args, (void)argp;
	net_ok = snc_api_init(&api, USER_AGENT, SDK_TAG) ? 1 : 0;
	for (;;) {
		if (!net_ok && snc_client_net_step(&client, &api)) continue;
		sceKernelDelayThread(10 * 1000);
	}
	return 0;
}

/* --------------------------------- the record --------------------------------- */

static void record_load(char *out, size_t cap) {
	out[0] = 0;
	FILE *f = fopen(record_path, "rb");
	if (!f) return;
	size_t n = fread(out, 1, cap - 1, f);
	out[n] = 0;
	fclose(f);
}

static void record_save(void) {
	client.rec_dirty = 0;
	char text[600];
	snc_record_text(&client.rec, text, sizeof text);
	FILE *f = fopen(record_path, "wb");
	if (!f) return;
	fwrite(text, 1, strlen(text), f);
	fclose(f);
}

/* ----------------------------------- input ------------------------------------ */

typedef struct {
	unsigned held, down, rep;
	uint32_t next_rep;
} pad_t;

static void read_pad(pad_t *p) {
	SceCtrlData d;
	sceCtrlReadBufferPositive(&d, 1);
	unsigned b = d.Buttons;
	// the stick works as a D-pad
	if (d.Lx < 50) b |= PSP_CTRL_LEFT;
	if (d.Lx > 205) b |= PSP_CTRL_RIGHT;
	if (d.Ly < 50) b |= PSP_CTRL_UP;
	if (d.Ly > 205) b |= PSP_CTRL_DOWN;
	p->down = b & ~p->held;
	unsigned dirs = PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT;
	p->rep = p->down;
	uint32_t now = snc_now_ms();
	if (p->down & dirs) p->next_rep = now + 320;
	else if ((b & dirs) && (int32_t)(now - p->next_rep) >= 0) {
		p->rep |= b & dirs;
		p->next_rep = now + 90;
	}
	p->held = b;
}

static void handle_input(const pad_t *p) {
	unsigned down = p->down, rep = p->rep;
	if (view3d_in_intro(&client, snc_now_ms())) {  // the VS intro: any of these skips it
		if (down & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE | PSP_CTRL_START)) view3d_skip_intro();
		return;
	}
	if (client.screen == SC_LOBBY || (client.screen == SC_GAME && !client.have_game)) {
		if (down & PSP_CTRL_LTRIGGER) view3d_champ_step(&client, -1);  // the champion spotlight
		if (down & PSP_CTRL_RTRIGGER) view3d_champ_step(&client, 1);
	}
	if (client.screen == SC_RULES) {
		if (rep & PSP_CTRL_UP) view_scroll(&view, -2);
		if (rep & PSP_CTRL_DOWN) view_scroll(&view, 2);
		if (rep & PSP_CTRL_LTRIGGER) view_scroll(&view, -10);
		if (rep & PSP_CTRL_RTRIGGER) view_scroll(&view, 10);
		if (down & (PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE | PSP_CTRL_CROSS)) snc_act_back(&client);
		return;
	}
	if (rep & PSP_CTRL_UP) view_nav(&view, 0, -1);
	if (rep & PSP_CTRL_DOWN) view_nav(&view, 0, 1);
	if (rep & PSP_CTRL_LEFT) view_nav(&view, -1, 0);
	if (rep & PSP_CTRL_RIGHT) view_nav(&view, 1, 0);
	if (down & PSP_CTRL_CROSS) view_activate(&view, &client);
	if (down & PSP_CTRL_CIRCLE) view_back(&view, &client);
	if (down & PSP_CTRL_SQUARE) snc_act_action(&client);
	if (down & PSP_CTRL_TRIANGLE) snc_act_how_to_play(&client);
	if (down & PSP_CTRL_LTRIGGER) view_step_hand(&view, &client, -1);
	if (down & PSP_CTRL_RTRIGGER) view_step_hand(&view, &client, 1);
	if ((down & PSP_CTRL_START) && client.screen == SC_GAME) snc_act_resign(&client);
	if ((down & PSP_CTRL_SELECT) && snc_client_over(&client)) view.show_replay = !view.show_replay;
}

static uint32_t signature(void) {
	const snc_client *c = &client;
	uint32_t h = 2166136261u;
	const int parts[] = {c->screen, c->job.state, c->busy[0], (int)c->notice_until, c->game.move_count, c->game.status, c->game.your_turn,
	                     c->pick.selected, c->pick.path_n, c->code_len, c->rules_ready, c->review.ok, c->have_game, c->opp_hand,
	                     c->armed, (int)c->flash, c->pending_cell, view.scroll, view.show_replay, view.focus_id, view.focus_arg,
	                     snc_client_notice(c) != NULL, c->rec.ruleset, view3d_in_intro(c, snc_now_ms())};
	for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) h = (h ^ (uint32_t)parts[i]) * 16777619u;
	return h;
}

// One line per change, on the debug output and in a tagged buffer in RAM: what an
// automated test reads (PPSSPP's libretro core does not pass the debug output on).
char snc_mailbox[1800] = "SNC-MAILBOX:";

static void log_state(void) {
	const snc_client *c = &client;
	const snc_game *g = &c->game;
	unsigned playable = 0;
	for (int i = 0; i < g->hand_n; i++)
		if (snc_playable(g, i)) playable |= 1u << i;
	snc_status_t st;
	snc_client_status(c, &st);
	char *line = snc_mailbox + 12;
	size_t cap = sizeof snc_mailbox - 12;
	int n = snprintf(line, cap, "code=%s scr=%d have=%d st=%d seat=%c turn=%d busy=%d hop=%d sel=%d path=%d hand=%d play=%x targets=%x action=%d over=%d focus=%d/%d intro=%d score=%d-%d moves=%d main=\"%s\" hits=",
	       g->code[0] ? g->code : "-", c->screen, c->have_game, g->status, g->seat ? g->seat : '-', g->your_turn, c->busy[0] != 0, g->has_hop,
	       c->pick.selected, c->pick.path_n, g->hand_n, playable, snc_targets(g, &c->pick), st.action_id, snc_client_over(c), view.focus_id,
	       view.focus_arg, view3d_in_intro(c, snc_now_ms()), g->score_you, g->score_opp, g->move_count, st.main);
	for (int i = 0; i < view.n && n > 0 && (size_t)n + 26 < cap; i++)  // the targets on screen: tests navigate by them
		n += snprintf(line + n, cap - (size_t)n, "%d:%d:%d:%d:%d:%d;", view.hit[i].id, view.hit[i].arg, view.hit[i].x, view.hit[i].y, view.hit[i].w,
		              view.hit[i].h);
	if (n > 0 && (size_t)n + 1 < cap) line[n] = '\n', line[n + 1] = 0;
	printf("[snc] %s", line);
}

/* ----------------------------------- main ------------------------------------- */

int main(int argc, char **argv) {
	int cbt = sceKernelCreateThread("callbacks", callback_thread, 0x11, 0xFA0, 0, NULL);
	if (cbt >= 0) sceKernelStartThread(cbt, 0, NULL);
	scePowerSetClockFrequency(333, 333, 166);

	gx_init();
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

	// the record lives next to the EBOOT
	snprintf(record_path, sizeof record_path, "%s", argc > 0 && argv[0] ? argv[0] : "ms0:/PSP/GAME/SmashAndClash/EBOOT.PBP");
	char *slash = strrchr(record_path, '/');
	if (slash) snprintf(slash + 1, sizeof record_path - (size_t)(slash + 1 - record_path), "record.txt");
	char record[600], name[128] = "";
	record_load(record, sizeof record);
	sceUtilityGetSystemParamString(PSP_SYSTEMPARAM_ID_STRING_NICKNAME, name, sizeof name);
	if (!name[0]) snprintf(name, sizeof name, "PSP player");
	snc_client_init(&client, name, "PSP", record);
	view_init(&view);

	boot_screen("Starting the network...", NET_HELP, 0);
	if (start_network() < 0 || connect_ap() < 0) {
		printf("[snc] no network\n");
		boot_screen("No network connection.", NET_HELP, 1);
		for (;;) sceDisplayWaitVblankStart();
	}
	printf("[snc] network up\n");
	boot_screen("Connecting to smashandclash.in...", "Setting up a secure connection.", 0);
	int nt = sceKernelCreateThread("snc_net", net_thread, 0x30, 256 * 1024, PSP_THREAD_ATTR_USER, NULL);
	sceKernelStartThread(nt, 0, NULL);
	while (net_ok < 0) sceKernelDelayThread(20 * 1000);
	if (net_ok > 0) {
		boot_screen("The secure connection could not start.", api.err, 1);
		for (;;) sceDisplayWaitVblankStart();
	}
	snc_client_start(&client);

	pad_t pad = {0};
	uint32_t last_sig = 0;
	for (;;) {  // every frame: the GPU redraws it all (the cards animate)
		read_pad(&pad);
		handle_input(&pad);
		snc_client_tick(&client);
		if (client.rec_dirty) record_save();
		gx_begin(0x016DCB);
		view3d_draw(&view, &client, snc_now_ms());
		gx_end();
		uint32_t sig = signature();
		if (sig != last_sig) log_state();  // after the draw: the targets it lists are the ones on screen
		last_sig = sig;
	}
	return 0;
}
