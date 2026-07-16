#ifndef AMALGAMATED_BUILD
#include "chess_board.h"

#include <string.h>

// Panel pixels are byte-swapped RGB565: display.c defines TFT_RED as 0x00F8
// (not the textbook 0xF800), TFT_BLUE as 0x1F00, and so on. Build colours in
// the standard layout then swap, so the constants below read normally.
#define RGB(r, g, b)                                                                                                   \
    ((uint16_t)((((uint16_t)(((r) & 0xF8) << 8) | (uint16_t)(((g) & 0xFC) << 3) | (uint16_t)((b) >> 3)) >> 8)          \
        | (((uint16_t)(((r) & 0xF8) << 8) | (uint16_t)(((g) & 0xFC) << 3) | (uint16_t)((b) >> 3)) << 8)))

// Board palette. Warm cream/brown reads well on the Jade panel and keeps both
// piece colours legible against both squares.
#define COL_LIGHT RGB(232, 208, 170)
#define COL_DARK RGB(150, 100, 62)
#define COL_WHITE_FILL RGB(250, 250, 245)
#define COL_WHITE_LINE RGB(30, 25, 20)
#define COL_BLACK_FILL RGB(40, 36, 34)
#define COL_BLACK_LINE RGB(235, 235, 230)

// Highlights. Selection is a strong green, the cycled destination a brighter
// green, the previous move a muted blue, and check an unmissable red.
#define COL_SEL RGB(90, 160, 70)
#define COL_DEST RGB(130, 200, 90)
#define COL_LAST RGB(90, 120, 165)
#define COL_CHECK RGB(200, 60, 50)

// Piece sprites, 20x20. '.' transparent, '#' body fill, 'o' outline.
// White pieces draw fill light with a dark outline; black pieces invert this,
// so both stay legible on light and dark squares alike.
// Dimensions are asserted at runtime by chb_art_valid() and covered by
// render_test.c -- do not hand-count these.
// clang-format off
static const char* const art_pawn[20] = {
    "....................",
    "....................",
    "....................",
    "........oooo........",
    ".......o####o.......",
    ".......o####o.......",
    ".......o####o.......",
    "........o##o........",
    ".......oo##oo.......",
    "......o######o......",
    "......o######o......",
    ".......o####o.......",
    ".......o####o.......",
    "......o######o......",
    ".....o########o.....",
    "....o##########o....",
    "....o##########o....",
    "....oooooooooooo....",
    "....................",
    "....................",
};

// Facing left. The hardest piece to read at this size: the cues that carry it
// are the ear, the eye notch, and the snout jutting past the head at mid-height.
static const char* const art_knight[20] = {
    "....................",
    "....................",
    "..........oo........",
    ".........o##oo......",
    "........o#####o.....",
    ".......o#######o....",
    "......o#########o...",
    ".....o#o#########o..",
    "....o##oo########o..",
    "...o#############o..",
    "..o##############o..",
    "..o#####oo#######o..",
    "..oo###oo########o..",
    "...ooo###########o..",
    "....o############o..",
    "....o############o..",
    "...o#############o..",
    "...ooooooooooooooo..",
    "....................",
    "....................",
};

static const char* const art_bishop[20] = {
    "....................",
    "....................",
    ".........oo.........",
    "........o##o........",
    "........o##o........",
    ".......o####o.......",
    "......o##oo##o......",
    "......o##oo##o......",
    "......o######o......",
    "......o######o......",
    ".......o####o.......",
    ".......o####o.......",
    "......o######o......",
    ".....o########o.....",
    "....o##########o....",
    "....o##########o....",
    "....oooooooooooo....",
    "....................",
    "....................",
    "....................",
};

static const char* const art_rook[20] = {
    "....................",
    "....................",
    "...oooooooooooooo...",
    "...o##o##oo##o##o...",
    "...o##o##oo##o##o...",
    "...o############o...",
    "...o############o...",
    "....o##########o....",
    ".....o########o.....",
    ".....o########o.....",
    ".....o########o.....",
    ".....o########o.....",
    ".....o########o.....",
    "....o##########o....",
    "...o############o...",
    "...o############o...",
    "...oooooooooooooo...",
    "....................",
    "....................",
    "....................",
};

static const char* const art_queen[20] = {
    "....................",
    "....................",
    "...o....o..o....o...",
    "...oo..oo..oo..oo...",
    "...o#oo##oo##oo#o...",
    "...o############o...",
    "...o############o...",
    "....o##########o....",
    "....o##########o....",
    ".....o########o.....",
    ".....o########o.....",
    "......o######o......",
    "......o######o......",
    ".....o########o.....",
    "....o##########o....",
    "...o############o...",
    "...oooooooooooooo...",
    "....................",
    "....................",
    "....................",
};

static const char* const art_king[20] = {
    "....................",
    "........oooo........",
    "........o##o........",
    ".....oooo##oooo.....",
    ".....o########o.....",
    ".....oooo##oooo.....",
    "........o##o........",
    ".......oo##oo.......",
    "......o######o......",
    ".....o########o.....",
    "....o##########o....",
    "....o##########o....",
    "....o##########o....",
    ".....o########o.....",
    "....o##########o....",
    "...o############o...",
    "...o############o...",
    "...oooooooooooooo...",
    "....................",
    "....................",
};
// clang-format on

static const char* const* art_for(uint8_t type)
{
    switch (type) {
    case CH_PAWN:
        return art_pawn;
    case CH_KNIGHT:
        return art_knight;
    case CH_BISHOP:
        return art_bishop;
    case CH_ROOK:
        return art_rook;
    case CH_QUEEN:
        return art_queen;
    default:
        return art_king;
    }
}

bool chb_art_valid(void)
{
    static const uint8_t types[6] = { CH_PAWN, CH_KNIGHT, CH_BISHOP, CH_ROOK, CH_QUEEN, CH_KING };
    for (int t = 0; t < 6; ++t) {
        const char* const* art = art_for(types[t]);
        for (int r = 0; r < CHB_SQUARE_PX; ++r) {
            if (strlen(art[r]) != CHB_SQUARE_PX) {
                return false;
            }
            for (int c = 0; c < CHB_SQUARE_PX; ++c) {
                const char ch = art[r][c];
                if (ch != '.' && ch != '#' && ch != 'o') {
                    return false;
                }
            }
        }
    }
    return true;
}

void chb_view_init(chb_view_t* view)
{
    memset(view, 0, sizeof(*view));
    ch_init(&view->pos);
    view->sel_from = CHB_NO_SQ;
    view->sel_to = CHB_NO_SQ;
    view->last_from = CHB_NO_SQ;
    view->last_to = CHB_NO_SQ;
    view->flipped = false;
}

// Map a board square to its top-left pixel. Rank 7 (black's back rank) is drawn
// at the top when not flipped.
static void square_origin(const chb_view_t* view, uint8_t sq, int* x, int* y)
{
    int file = CH_FILE(sq);
    int rank = CH_RANK(sq);
    if (view->flipped) {
        file = 7 - file;
    } else {
        rank = 7 - rank;
    }
    *x = file * CHB_SQUARE_PX;
    *y = rank * CHB_SQUARE_PX;
}

uint8_t chb_square_at(const chb_view_t* view, int x, int y)
{
    if (x < 0 || y < 0 || x >= CHB_BOARD_PX || y >= CHB_BOARD_PX) {
        return CHB_NO_SQ;
    }
    int file = x / CHB_SQUARE_PX;
    int rank = y / CHB_SQUARE_PX;
    if (view->flipped) {
        file = 7 - file;
    } else {
        rank = 7 - rank;
    }
    return (uint8_t)CH_SQ(file, rank);
}

static uint16_t square_colour(const chb_view_t* view, uint8_t sq)
{
    // Check takes priority: the player must never miss it.
    if (CH_TYPE(view->pos.board[sq]) == CH_KING && CH_COLOUR(view->pos.board[sq]) == view->pos.side
        && ch_in_check(&view->pos, view->pos.side)) {
        return COL_CHECK;
    }
    if (sq == view->sel_to) {
        return COL_DEST;
    }
    if (sq == view->sel_from) {
        return COL_SEL;
    }
    if (sq == view->last_from || sq == view->last_to) {
        return COL_LAST;
    }
    // Light squares are those where file+rank is odd (a1 is dark)
    return ((CH_FILE(sq) + CH_RANK(sq)) & 1) ? COL_LIGHT : COL_DARK;
}

void chb_render(const chb_view_t* view, uint16_t* buf)
{
    for (int rank = 0; rank < 8; ++rank) {
        for (int file = 0; file < 8; ++file) {
            const uint8_t sq = (uint8_t)CH_SQ(file, rank);
            int ox, oy;
            square_origin(view, sq, &ox, &oy);

            const uint16_t bg = square_colour(view, sq);
            for (int r = 0; r < CHB_SQUARE_PX; ++r) {
                uint16_t* row = buf + (size_t)(oy + r) * CHB_BOARD_PX + ox;
                for (int c = 0; c < CHB_SQUARE_PX; ++c) {
                    row[c] = bg;
                }
            }

            const uint8_t pc = view->pos.board[sq];
            if (pc == CH_EMPTY) {
                continue;
            }

            const bool white = CH_COLOUR(pc) == CH_WHITE;
            const uint16_t fill = white ? COL_WHITE_FILL : COL_BLACK_FILL;
            const uint16_t line = white ? COL_WHITE_LINE : COL_BLACK_LINE;
            const char* const* art = art_for(CH_TYPE(pc));

            for (int r = 0; r < CHB_SQUARE_PX; ++r) {
                uint16_t* row = buf + (size_t)(oy + r) * CHB_BOARD_PX + ox;
                for (int c = 0; c < CHB_SQUARE_PX; ++c) {
                    switch (art[r][c]) {
                    case '#':
                        row[c] = fill;
                        break;
                    case 'o':
                        row[c] = line;
                        break;
                    default:
                        break; // transparent
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// SAN
// ---------------------------------------------------------------------------

static char piece_letter(uint8_t type)
{
    switch (type) {
    case CH_KNIGHT:
        return 'N';
    case CH_BISHOP:
        return 'B';
    case CH_ROOK:
        return 'R';
    case CH_QUEEN:
        return 'Q';
    case CH_KING:
        return 'K';
    default:
        return '\0'; // pawns carry no letter
    }
}

void chb_move_san(const ch_pos_t* pos, const ch_move_t* move, char* out, size_t len)
{
    char buf[CHB_SAN_LEN * 2];
    size_t n = 0;

    if (move->flags & CH_MF_CASTLE) {
        const char* s = (CH_FILE(move->to) == 6) ? "O-O" : "O-O-O";
        while (*s) {
            buf[n++] = *s++;
        }
    } else {
        const uint8_t pc = pos->board[move->from];
        const uint8_t type = CH_TYPE(pc);
        const char letter = piece_letter(type);

        if (letter) {
            buf[n++] = letter;

            // Disambiguate against any other same-type piece that could also
            // land on `to`: by file if that suffices, else by rank, else both.
            ch_move_t moves[CH_MAX_MOVES];
            const int nmoves = ch_gen_legal(pos, moves);
            bool need_file = false, need_rank = false, ambiguous = false;
            for (int i = 0; i < nmoves; ++i) {
                if (moves[i].to != move->to || moves[i].from == move->from) {
                    continue;
                }
                if (pos->board[moves[i].from] != pc) {
                    continue;
                }
                ambiguous = true;
                if (CH_FILE(moves[i].from) == CH_FILE(move->from)) {
                    need_rank = true;
                }
                if (CH_RANK(moves[i].from) == CH_RANK(move->from)) {
                    need_file = true;
                }
            }
            if (ambiguous) {
                if (!need_file && !need_rank) {
                    need_file = true; // file alone distinguishes them
                }
                if (need_file) {
                    buf[n++] = (char)('a' + CH_FILE(move->from));
                }
                if (need_rank) {
                    buf[n++] = (char)('1' + CH_RANK(move->from));
                }
            }
        } else if (move->flags & CH_MF_CAPTURE) {
            // Pawn captures name their origin file
            buf[n++] = (char)('a' + CH_FILE(move->from));
        }

        if (move->flags & CH_MF_CAPTURE) {
            buf[n++] = 'x';
        }
        buf[n++] = (char)('a' + CH_FILE(move->to));
        buf[n++] = (char)('1' + CH_RANK(move->to));

        if (move->flags & CH_MF_PROMO) {
            buf[n++] = '=';
            buf[n++] = piece_letter(move->promo);
        }
    }

    // Check and mate suffixes require looking at the resulting position
    ch_pos_t work = *pos;
    ch_undo_t undo;
    ch_make(&work, move, &undo);
    if (ch_in_check(&work, work.side)) {
        ch_move_t replies[CH_MAX_MOVES];
        buf[n++] = ch_gen_legal(&work, replies) ? '+' : '#';
    }

    buf[n] = '\0';
    if (len) {
        const size_t copy = (n < len - 1) ? n : len - 1;
        memcpy(out, buf, copy);
        out[copy] = '\0';
    }
}

#endif // AMALGAMATED_BUILD
