// Drawing the game's pieces in the Arena Pop look: the blue world, glass plates, candy
// buttons with a lip, cards with crisp side values, the focus ring. Each platform's
// view lays these out for its own screens; the sizes come from its generated assets.h.
#ifndef SNC_DRAW_H
#define SNC_DRAW_H

#include "snc_gfx.h"

// Arena Pop · Blue tokens (src/ui/arena/tokens.css in the web edition).
#define C_WORLD_TOP 0x0ABEFF
#define C_WORLD_BOT 0x016DCB
#define C_INK 0x0D1A4A
#define C_INK2 0x22357A
#define C_INK3 0x4A5E9C
#define C_INK_DEEP 0x06102F
#define C_SUGAR 0xFFFFFF
#define C_SUGAR2 0xEEF6FF
#define C_SUGAR3 0xDCEBFF
#define C_SUGAR_EDGE 0xB4D0F0
#define C_SUN1 0xFFE872
#define C_SUN2 0xFFC21A
#define C_SUN_LIP 0xD98A00
#define C_MINT1 0x8BF7CF
#define C_MINT2 0x22D39A
#define C_MINT_LIP 0x0E9A6C
#define C_CHERRY1 0xFF5A5A
#define C_CHERRY2 0xE02030
#define C_CHERRY_LIP 0x9E0F18
#define C_GRAPE1 0x8A4BFF
#define C_GRAPE2 0x5B25D6
#define C_GRAPE_LIP 0x3B139E
#define C_SKY1 0x5ED3FF
#define C_SKY2 0x1398F0
#define C_SKY_LIP 0x0A62B8
#define C_GUM2 0xFF3E9A
#define C_YOU 0x2173E8
#define C_YOU_HI 0x5FA6FF
#define C_YOU_LIP 0x134A9F
#define C_OPP 0xF4581F
#define C_OPP_HI 0xFF9A62
#define C_OPP_LIP 0xA8380C

typedef enum { FILL_SUN, FILL_SUGAR, FILL_MINT, FILL_CHERRY, FILL_GRAPE, FILL_SKY } sd_fill;
enum { SD_FOCUS = 1, SD_OFF = 2 };
enum { CARD_DIM = 1, CARD_GHOST = 2, CARD_ROT = 4, CARD_SELECTED = 8, CARD_FROZEN = 16, CARD_FLASH = 32 };

void sd_world(snc_surf *s);
void sd_glass(snc_surf *s, int x, int y, int w, int h, int r, int alpha);
void sd_panel(snc_surf *s, int x, int y, int w, int h, int r);  // a sugar card with its lip
void sd_button(snc_surf *s, int x, int y, int w, int h, const snc_font *f, const char *label, sd_fill fill, int state);
// A two-line button: the label and a caption under it.
void sd_button2(snc_surf *s, int x, int y, int w, int h, const snc_font *f, const char *label, const snc_font *fc, const char *caption,
                sd_fill fill, int state);
void sd_focus(snc_surf *s, int x, int y, int w, int h, int r);

// A card in the small size (board and hand). vals: the numbers shown at the screen's
// top, right, bottom and left (-1 = none). owner: 'y', 'o' or 0 (no frame).
// card: the index in snc_cards (from sd_tile_card / sd_hand_card / sd_move_card), or -1
// for a card the client does not know (then name is written on a plain card).
void sd_card(snc_surf *s, int x, int y, int card, const char *name, int owner, const int vals[4], int flags, const snc_font *digits);
void sd_card_large(snc_surf *s, int x, int y, int card);
void sd_card_back(snc_surf *s, int x, int y);
void sd_logo_l(snc_surf *s, int x, int y);
void sd_logo_s(snc_surf *s, int x, int y);
void sd_icon(snc_surf *s, int x, int y, const char *name, uint32_t rgb);  // "knight", "frozen", ...
int sd_icon_w(const char *name);
int sd_icon_h(const char *name);
uint32_t sd_power_color(const char *name);  // a power tile's colour name -> RGB

// Which card (index in snc_cards, or -1): the one on a board tile, a hand card, or the
// one a move name is about ("Pengu@C2" -> Pengu, "BOULDER(D2)" -> Boulder!, "hop→E3" ->
// the card that hopped). Cards that share a name are told apart by their sides.
int sd_tile_card(const void *game, int cell);
int sd_hand_card(const void *hand_card);
int sd_move_card(const void *game, const char *move);

#endif
