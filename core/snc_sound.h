// What the game sounds like, the same on every console: which theme plays, and the sound
// effects the game's events make. Each platform plays them its own way (its own music and
// effects, made for it: tools/audio/).
#ifndef SNC_SOUND_H
#define SNC_SOUND_H

#include <stdint.h>

#include "snc_client.h"

typedef enum { MUS_NONE, MUS_LOBBY, MUS_GAME, MUS_WIN, MUS_LOSE } snc_music;  // WIN and LOSE play once

enum { SFX_MOVE, SFX_SELECT, SFX_BACK, SFX_PLACE, SFX_CAPTURE, SFX_TURN, SFX_ERROR, SFX_START, SFX_COUNT };

// The order effects win in when a console can play only one at a time.
extern const uint8_t SFX_PRIORITY[SFX_COUNT];

typedef struct {
	int init;
	int have_game, active, waiting, your_turn, move_count;
	uint32_t notice_until;
	snc_music jingle;   // the game-over jingle, once it has been chosen for this game
	char game_id[32];
} snc_sound;

// Once a frame, after snc_client_tick: the effects the game's events make now (bit i =
// SFX_i), and in *music the theme that should be playing. Nothing at all with the sound off.
unsigned snc_sound_update(snc_sound *s, const snc_client *c, snc_music *music);

#endif
