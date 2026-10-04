// The PSP's screens, drawn by its GPU in the web edition's look.
#ifndef VIEW3D_H
#define VIEW3D_H

#include <stdint.h>

#include "snc_client.h"
#include "snc_ui.h"

void view3d_draw(view_t *v, snc_client *c, uint32_t now);
int view3d_in_intro(const snc_client *c, uint32_t now);  // the VS intro is playing
void view3d_skip_intro(void);
void view3d_champ_step(snc_client *c, int dir);          // the lobby's champion spotlight (L / R)

#endif
