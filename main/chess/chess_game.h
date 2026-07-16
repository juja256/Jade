#ifndef CHESS_GAME_H_
#define CHESS_GAME_H_

#include "chess_board.h"

// The legal-move cycling state machine.
//
// Split out from chess_ui.c so it depends on nothing but libc and is fully
// testable on the host: chess_ui.c is then thin glue that pumps prev/next/
// select into here and paints what comes out.
//
// Jade Plus offers exactly three inputs -- prev, next, select (input.c,
// Kconfig.projbuild:278,286,304) -- and long-press-select is deliberately
// disabled (selectbtn.inc, button_front_long()), so there is no cancel
// gesture. Every escape is therefore an entry in the ring, never a gesture.
//
// Rather than roam a cursor over 64 squares, the ring holds only legal
// choices, so illegal moves are unrepresentable and a move costs two or three
// clicks.

// Ring bound: at most 16 pieces + resign + exit, or a queen's 27 destinations
// + back. 40 leaves headroom; chg_ring_len() never exceeds it.
#define CHG_RING_MAX 40

// SAN strings kept for the side panel.
#define CHG_HISTORY_MAX 8

// Label long enough for "Knight" and for any SAN string.
#define CHG_LABEL_LEN 12

typedef enum {
    CHG_PIECE_SELECT = 0, // choosing which piece to move
    CHG_DEST_SELECT, // choosing where it goes
    CHG_PROMO_SELECT, // choosing a promotion piece
    CHG_ENGINE_THINK, // engine is searching; input ignored
    CHG_GAME_OVER,
} chg_state_t;

typedef enum {
    CHG_ENTRY_PIECE = 0, // a piece that has at least one legal move
    CHG_ENTRY_DEST, // a destination for the chosen piece
    CHG_ENTRY_PROMO, // a promotion choice
    CHG_ENTRY_BACK, // return to piece selection
    CHG_ENTRY_RESIGN,
    CHG_ENTRY_EXIT,
    CHG_ENTRY_NEW, // start a new game
} chg_entry_kind_t;

typedef struct {
    chg_entry_kind_t kind;
    uint8_t sq; // PIECE/DEST: the square concerned
    uint8_t promo; // PROMO: CH_QUEEN etc
    char label[CHG_LABEL_LEN]; // what the panel shows
} chg_entry_t;

// What the caller must do after chg_select(). Keeps the search -- which on
// device runs via run_in_temporary_task() -- outside this unit.
typedef enum {
    CHG_ACT_NONE = 0, // just repaint
    CHG_ACT_ENGINE, // search, then call chg_engine_played()
    CHG_ACT_EXIT, // leave the activity
} chg_action_t;

typedef struct {
    chb_view_t view; // position + highlights, ready to render
    chg_state_t state;
    uint8_t human; // CH_WHITE or CH_BLACK
    ch_result_t result; // valid once state == CHG_GAME_OVER
    bool resigned;

    chg_entry_t ring[CHG_RING_MAX];
    int ring_len;
    int ring_pos;

    uint8_t chosen_from; // piece picked in CHG_PIECE_SELECT
    uint8_t chosen_to; // destination picked in CHG_DEST_SELECT

    char history[CHG_HISTORY_MAX][CHG_LABEL_LEN];
    int history_len; // entries in use, oldest first
    uint16_t history_first_move; // fullmove number of history[0]
    bool history_first_black; // history[0] was a black move
} chg_game_t;

// Start a new game. Returns CHG_ACT_ENGINE when the engine moves first (that
// is, when the human is black).
chg_action_t chg_init(chg_game_t* game, uint8_t human_colour);

// Start from an arbitrary position rather than the opening. Same contract as
// chg_init(). Used by the tests, and the hook any future save/resume would
// need.
chg_action_t chg_set_position(chg_game_t* game, const ch_pos_t* pos, uint8_t human_colour);

// Cycle the ring. No-ops while the engine is thinking or the ring is empty.
void chg_prev(chg_game_t* game);
void chg_next(chg_game_t* game);

// Act on the current ring entry.
chg_action_t chg_select(chg_game_t* game);

// Hand back the engine's move. `move` must be legal in the current position.
// Valid only in CHG_ENGINE_THINK; a no-op returning CHG_ACT_NONE otherwise.
chg_action_t chg_engine_played(chg_game_t* game, const ch_move_t* move);

// Current ring entry, or NULL when the ring is empty.
const chg_entry_t* chg_current(const chg_game_t* game);

// One-line status for the panel, e.g. "Your move", "Thinking...", "Checkmate".
const char* chg_status(const chg_game_t* game);

#endif /* CHESS_GAME_H_ */
