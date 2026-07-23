#ifndef CHESS_ENGINE_H_
#define CHESS_ENGINE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Chess engine using 0x88 board representation.
//
// This unit deliberately depends on nothing outside the C standard library so
// that it can be compiled and perft-tested on the host with no ESP-IDF, no
// firmware and no device. See main/chess/test/.

// Piece types (low 3 bits of a square value)
#define CH_EMPTY 0
#define CH_PAWN 1
#define CH_KNIGHT 2
#define CH_BISHOP 3
#define CH_ROOK 4
#define CH_QUEEN 5
#define CH_KING 6

// Colours (bit flags above the type bits)
#define CH_WHITE 8
#define CH_BLACK 16

#define CH_TYPE(p) ((uint8_t)((p) & 7))
#define CH_COLOUR(p) ((uint8_t)((p) & (CH_WHITE | CH_BLACK)))
#define CH_IS_EMPTY(p) ((p) == CH_EMPTY)

// Colour index: white -> 0, black -> 1
#define CH_CIDX(c) ((c) == CH_WHITE ? 0 : 1)
#define CH_OPP(c) ((uint8_t)((c) == CH_WHITE ? CH_BLACK : CH_WHITE))

// 0x88: a square is on the board iff no bits outside the 0x77 mask are set
#define CH_ONBOARD(sq) (((sq) & 0x88) == 0)
#define CH_RANK(sq) ((sq) >> 4)
#define CH_FILE(sq) ((sq) & 7)
#define CH_SQ(file, rank) (((rank) << 4) | (file))

// Castling rights bits
#define CH_CASTLE_WK 1
#define CH_CASTLE_WQ 2
#define CH_CASTLE_BK 4
#define CH_CASTLE_BQ 8

#define CH_NO_EP (-1)

// A position can have at most ~218 legal moves; 256 is a safe bound.
#define CH_MAX_MOVES 256

// Maximum plies the quiescence search may extend beyond the nominal depth.
// Quiescence is otherwise unbounded, and every frame holds a CH_MAX_MOVES
// array, so on a fixed FreeRTOS stack an unbounded extension is a real
// overflow risk rather than a theoretical one. Callers size their stack from
// CH_QUIESCE_MAX + their search depth.
#define CH_QUIESCE_MAX 6

// Move flags
#define CH_MF_CAPTURE 1
#define CH_MF_EP 2
#define CH_MF_CASTLE 4
#define CH_MF_DOUBLE 8
#define CH_MF_PROMO 16

typedef struct {
    uint8_t from;
    uint8_t to;
    uint8_t promo; // CH_QUEEN/CH_ROOK/CH_BISHOP/CH_KNIGHT, or 0
    uint8_t flags;
} ch_move_t;

typedef struct {
    uint8_t board[128];
    uint8_t side; // side to move: CH_WHITE or CH_BLACK
    uint8_t castle; // CH_CASTLE_* bitmask
    int8_t ep; // en-passant target square, or CH_NO_EP
    uint16_t halfmove; // halfmove clock (fifty-move rule)
    uint16_t fullmove;
    uint8_t king_sq[2]; // indexed by CH_CIDX()
    uint64_t hash; // Zobrist key; maintained by ch_make/ch_unmake
} ch_pos_t;

// Information required to undo a move
typedef struct {
    ch_move_t move;
    uint8_t captured; // piece removed (for ep this is the pawn, not board[to])
    uint8_t castle;
    int8_t ep;
    uint16_t halfmove;
    uint16_t fullmove;
    uint64_t hash; // position hash before the move
} ch_undo_t;

typedef enum {
    CH_ONGOING = 0,
    CH_CHECKMATE,
    CH_STALEMATE,
    CH_DRAW_FIFTY,
    CH_DRAW_MATERIAL,
} ch_result_t;

// Set up the standard starting position.
void ch_init(ch_pos_t* pos);

// Parse a FEN string. Returns false on malformed input, leaving pos undefined.
// Used by the perft tests; not needed by the firmware itself.
bool ch_from_fen(ch_pos_t* pos, const char* fen);

// True if `sq` is attacked by any piece of colour `by`.
bool ch_attacked(const ch_pos_t* pos, uint8_t sq, uint8_t by);

// True if `side`'s king is currently in check.
bool ch_in_check(const ch_pos_t* pos, uint8_t side);

// Generate all *legal* moves for the side to move into `out`, returning the
// count. `out` must hold at least CH_MAX_MOVES entries.
//
// This is the single source of truth for legality, and is what the UI cycles
// over. Moves leaving one's own king in check are already excluded.
int ch_gen_legal(const ch_pos_t* pos, ch_move_t* out);

// Apply/undo a move. `undo` must be passed back unmodified to ch_unmake().
void ch_make(ch_pos_t* pos, const ch_move_t* move, ch_undo_t* undo);
void ch_unmake(ch_pos_t* pos, const ch_undo_t* undo);

// Node count at the given depth. The canonical move-generation correctness
// test: mismatched perft means a movegen bug, full stop.
uint64_t ch_perft(ch_pos_t* pos, int depth);

// Game state for the side to move.
ch_result_t ch_result(const ch_pos_t* pos);

// Compute the Zobrist key from scratch. Normally you use pos->hash, which is
// kept up to date; this exists for initialisation and for tests.
uint64_t ch_zobrist(const ch_pos_t* pos);

#define CH_TT_NONE 0
#define CH_TT_EXACT 1
#define CH_TT_LOWER 2
#define CH_TT_UPPER 3

typedef struct {
    uint64_t key;
    ch_move_t move;
    int16_t score;
    uint8_t depth;
    uint8_t flag; // CH_TT_*
} ch_tt_entry_t;

typedef struct {
    ch_tt_entry_t* entries;
    size_t count;
} ch_tt_t;

// The engine never allocates; the caller provides a buffer of at least
// ch_tt_sizeof(count) bytes (PSRAM on device). ch_tt_init clears it.
size_t ch_tt_sizeof(size_t count);
void ch_tt_init(ch_tt_t* tt, void* buffer, size_t count);
void ch_tt_clear(ch_tt_t* tt);
bool ch_tt_probe(const ch_tt_t* tt, uint64_t key, ch_tt_entry_t* out);
void ch_tt_store(ch_tt_t* tt, uint64_t key, int depth, int score, uint8_t flag, ch_move_t move);

// Best root score at the given depth (iterative deepening). Deterministic.
int ch_search_bestscore(ch_pos_t* pos, int depth, ch_tt_t* tt);

// Search for a move. Among root moves scoring within `margin` centipawns of the
// best, picks one at random via `rng_state` (xorshift32 state, may be NULL);
// margin 0 or rng NULL gives the strict best move. `tt` may be NULL.
// Returns false when there is no legal move.
bool ch_search_ex(ch_pos_t* pos, int depth, int margin, uint32_t* rng_state, ch_tt_t* tt, ch_move_t* best);

// Strict best-move search (margin 0, no randomness, no table).
bool ch_search(ch_pos_t* pos, int depth, ch_move_t* best);

// Number of search nodes visited by the last ch_search*/ch_search_bestscore
// call. Reset at the start of each search. Useful for tuning and as a
// pruning-regression guard -- a search that stops pruning shows up here.
extern uint64_t ch_search_nodes;

// If the position is in the opening book, write the booked move to `out` and
// return true; the caller can play it instantly instead of searching. The book
// is built lazily on first call. Returns false once the game leaves book.
bool ch_book_move(const ch_pos_t* pos, ch_move_t* out);

#endif /* CHESS_ENGINE_H_ */
