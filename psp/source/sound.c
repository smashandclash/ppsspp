// The PSP's sound. See sound.h.
//
// The game thread only says what should play; the audio thread owns the voices. A request
// is a small struct the audio thread picks up at the start of its next buffer (about 23 ms).
#include "sound.h"

#include <pspaudio.h>
#include <pspkernel.h>
#include <string.h>

#include "audio.h"

#define FRAMES 1024           // per buffer, at 44100 Hz
#define MUSIC_GAIN 150        // of 256
#define SFX_GAIN 230
#define VOICES 4

typedef struct {
	const int16_t *data;
	uint32_t len;   // samples at 22050 Hz
	uint32_t pos;   // in output frames (two per source sample)
	int loop, gain, on;
} voice_t;

static voice_t music, fx[VOICES];
static volatile int music_req = -1;           // a snc_music to switch to, or -1
static volatile unsigned fx_req;              // effects to start
static snc_music playing = MUS_NONE;
static int16_t out[2][FRAMES * 2] __attribute__((aligned(64)));

static const struct { const uint8_t *data; uint32_t samples; } SFX[SFX_COUNT] = {
	[SFX_MOVE] = {snd_sfx_move, SND_SFX_MOVE_SAMPLES},          [SFX_SELECT] = {snd_sfx_select, SND_SFX_SELECT_SAMPLES},
	[SFX_BACK] = {snd_sfx_back, SND_SFX_BACK_SAMPLES},          [SFX_PLACE] = {snd_sfx_place, SND_SFX_PLACE_SAMPLES},
	[SFX_CAPTURE] = {snd_sfx_capture, SND_SFX_CAPTURE_SAMPLES}, [SFX_TURN] = {snd_sfx_turn, SND_SFX_TURN_SAMPLES},
	[SFX_ERROR] = {snd_sfx_error, SND_SFX_ERROR_SAMPLES},       [SFX_START] = {snd_sfx_start, SND_SFX_START_SAMPLES},
};

static void set(voice_t *v, const uint8_t *data, uint32_t samples, int loop, int gain) {
	v->on = 0;
	v->data = (const int16_t *)data;
	v->len = samples;
	v->pos = 0;
	v->loop = loop;
	v->gain = gain;
	v->on = data != NULL && samples > 1;
}

static void take_requests(void) {
	int m = music_req;
	if (m >= 0) {
		music_req = -1;
		switch (m) {
		case MUS_LOBBY: set(&music, snd_lobby, SND_LOBBY_SAMPLES, 1, MUSIC_GAIN); break;
		case MUS_GAME: set(&music, snd_game, SND_GAME_SAMPLES, 1, MUSIC_GAIN); break;
		case MUS_WIN: set(&music, snd_win, SND_WIN_SAMPLES, 0, MUSIC_GAIN + 40); break;
		case MUS_LOSE: set(&music, snd_lose, SND_LOSE_SAMPLES, 0, MUSIC_GAIN + 40); break;
		default: music.on = 0; break;
		}
	}
	unsigned req = fx_req;
	if (req) {
		fx_req = 0;
		for (int i = 0; i < SFX_COUNT; i++) {
			if (!(req & (1u << i))) continue;
			int slot = 0;  // a free voice, or the one nearest its end
			for (int k = 0; k < VOICES; k++) {
				if (!fx[k].on) { slot = k; break; }
				if (fx[k].pos * 1.0 / fx[k].len > fx[slot].pos * 1.0 / fx[slot].len) slot = k;
			}
			set(&fx[slot], SFX[i].data, SFX[i].samples, 0, SFX_GAIN);
		}
	}
}

// Adds a voice into the mix: each 22050 Hz sample makes two 44100 Hz frames (the second
// halfway to the next sample).
static void mix(voice_t *v, int32_t *acc) {
	if (!v->on) return;
	for (int f = 0; f < FRAMES; f++) {
		uint32_t i = v->pos >> 1;
		if (i + 1 >= v->len) {
			if (!v->loop) {
				v->on = 0;
				return;
			}
			v->pos = 0;
			i = 0;
		}
		int32_t s = v->data[i];
		if (v->pos & 1) s = (s + v->data[i + 1]) >> 1;
		acc[f] += s * v->gain;
		v->pos++;
	}
}

static int audio_thread(SceSize args, void *argp) {
	(void)args, (void)argp;
	int ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, FRAMES, PSP_AUDIO_FORMAT_STEREO);
	if (ch < 0) return 0;
	static int32_t acc[FRAMES];
	for (int b = 0;; b ^= 1) {
		take_requests();
		memset(acc, 0, sizeof acc);
		mix(&music, acc);
		for (int k = 0; k < VOICES; k++) mix(&fx[k], acc);
		for (int f = 0; f < FRAMES; f++) {
			int32_t s = acc[f] >> 8;
			if (s > 32767) s = 32767;
			if (s < -32768) s = -32768;
			out[b][2 * f] = out[b][2 * f + 1] = (int16_t)s;
		}
		sceAudioOutputBlocking(ch, PSP_AUDIO_VOLUME_MAX, out[b]);  // waits while the last buffer plays
	}
	return 0;
}

void psp_sound_init(void) {
	int th = sceKernelCreateThread("audio", audio_thread, 0x12, 0x8000, PSP_THREAD_ATTR_USER, NULL);
	if (th >= 0) sceKernelStartThread(th, 0, NULL);
}

void psp_sound_music(snc_music m) {
	if (m == playing) return;
	playing = m;
	music_req = m;
}

void psp_sound_sfx(unsigned mask) {
	fx_req |= mask;
}
