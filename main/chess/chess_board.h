#ifndef CHESS_BOARD_H_
#define CHESS_BOARD_H_

#include "engine.h"

#include <stddef.h>
#include <stdint.h>

// Board presentation: game state plus the rendering of it.
//
// Like engine.h this unit deliberately depends on nothing outside libc. It
// renders into a raw uint16_t buffer rather than a Jade `Picture` so that it
// stays testable on the host; chess_ui.c wraps the buffer in a Picture and
// hands it to gui_update_picture(). Jade's color_t is #defined to uint16_t
// (display.h), so no conversion is involved.

// The board is a fixed 160x160 regardless of display size, and is centred by
// the UI layer. See docs/superpowers/specs/2026-07-16-chess-app-design.md.
#define CHB_SQUARE_PX 20
#define CHB_BOARD_PX (8 * CHB_SQUARE_PX)
#define CHB_BUF_PIXELS (CHB_BOARD_PX * CHB_BOARD_PX)

#define CHB_NO_SQ 0xFF

// Longest SAN is a disambiguated promotion capture with mate, e.g. "gxh8=Q#",
// plus room to spare.
#define CHB_SAN_LEN 12

// What to draw: the position plus transient highlight state driven by the UI's
// legal-move cycling.
typedef struct {
    ch_pos_t pos;
    uint8_t sel_from; // piece the user has selected, or CHB_NO_SQ
    uint8_t sel_to; // destination currently being cycled, or CHB_NO_SQ
    uint8_t last_from; // previous move, faintly marked; CHB_NO_SQ if none
    uint8_t last_to;
    bool flipped; // draw from black's point of view
} chb_view_t;

// Start position, no highlights, white at the bottom.
void chb_view_init(chb_view_t* view);

// Render the board into `buf`, which must hold CHB_BUF_PIXELS pixels.
// Pixels are byte-swapped RGB565, matching the panel format used by the
// TFT_* constants in display.c.
void chb_render(const chb_view_t* view, uint16_t* buf);

// Format `move` in Standard Algebraic Notation, including disambiguation and
// check/mate suffixes. `move` must be legal in `pos`. Writes at most `len`
// bytes including the terminator.
void chb_move_san(const ch_pos_t* pos, const ch_move_t* move, char* out, size_t len);

// Square under a pixel, honouring `flipped`. Returns CHB_NO_SQ if outside the
// board. Unused by the button UI, but needed for touch boards and useful in
// tests.
uint8_t chb_square_at(const chb_view_t* view, int x, int y);

// True if every piece sprite is exactly CHB_SQUARE_PX square and uses only the
// legal art characters. The sprites are hand-authored, so this is checked
// rather than counted by eye.
bool chb_art_valid(void);

#endif /* CHESS_BOARD_H_ */
