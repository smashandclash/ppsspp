// The PSP's sound: a mixer on its own thread plays the theme and up to four effects at
// once, 16-bit at 22050 Hz, out at 44100 Hz stereo through one hardware channel.
#ifndef PSP_SOUND_H
#define PSP_SOUND_H

#include "snc_sound.h"

void psp_sound_init(void);
void psp_sound_music(snc_music m);  // starts it if it is not already playing
void psp_sound_sfx(unsigned fx);    // a mask of SFX_*

#endif
