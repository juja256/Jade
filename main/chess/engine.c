// The AMALGAMATED_BUILD guard wraps the whole file, as everywhere else in
// main/: under CONFIG_AMALGAMATED_BUILD every .c is also compiled standalone,
// and would clash with the copy amalgamated.c pulls in. amalgamated.c #undefs
// the macro before including us, so exactly one copy is real.
// AMALGAMATED_BUILD is never defined on the host, so the perft tests are
// unaffected.
#ifndef AMALGAMATED_BUILD
#include "engine.h"

#include <string.h>

// Zobrist hashing. Keys are generated once from a fixed seed; they need only be
// consistent within a single process, not across builds.
static uint64_t zob_piece[12][64];
static uint64_t zob_side;
static uint64_t zob_castle[16];
static uint64_t zob_ep[8];
static bool zob_ready = false;

static uint64_t zob_next(uint64_t* s) {
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x;
    return x;
}

static void zob_ensure(void) {
    if (zob_ready) return;
    uint64_t s = 0x9E3779B97F4A7C15ULL;
    for (int p = 0; p < 12; ++p)
        for (int q = 0; q < 64; ++q) zob_piece[p][q] = zob_next(&s);
    zob_side = zob_next(&s);
    for (int i = 0; i < 16; ++i) zob_castle[i] = zob_next(&s);
    for (int i = 0; i < 8; ++i) zob_ep[i] = zob_next(&s);
    zob_ready = true;
}

// 0x88 square -> 0..63
static inline int zob_sq(uint8_t sq) { return (CH_RANK(sq) << 3) | CH_FILE(sq); }
// piece byte -> 0..11 (type 1..6, colour)
static inline int zob_pidx(uint8_t pc) {
    return (CH_TYPE(pc) - 1) + (CH_COLOUR(pc) == CH_BLACK ? 6 : 0);
}
static inline uint64_t zob_pc(uint8_t pc, uint8_t sq) { return zob_piece[zob_pidx(pc)][zob_sq(sq)]; }

uint64_t ch_zobrist(const ch_pos_t* pos) {
    zob_ensure();
    uint64_t h = 0;
    for (int sq = 0; sq < 128; ++sq) {
        if (!CH_ONBOARD(sq)) continue;
        const uint8_t pc = pos->board[sq];
        if (pc != CH_EMPTY) h ^= zob_pc(pc, (uint8_t)sq);
    }
    if (pos->side == CH_BLACK) h ^= zob_side;
    h ^= zob_castle[pos->castle & 0x0F];
    if (pos->ep != CH_NO_EP) h ^= zob_ep[CH_FILE(pos->ep)];
    return h;
}

// 0x88 move offsets. Square arithmetic is done in int to avoid uint8_t wrap;
// CH_ONBOARD() then rejects anything that stepped off the board.
static const int knight_off[8] = { -33, -31, -18, -14, 14, 18, 31, 33 };
static const int king_off[8] = { -17, -16, -15, -1, 1, 15, 16, 17 };
static const int bishop_off[4] = { -17, -15, 15, 17 };
static const int rook_off[4] = { -16, -1, 1, 16 };

// Castling rights to clear when a piece moves from, or lands on, a square.
// Indexing by both `from` and `to` handles king moves, rook moves and -- the
// classic bug -- a rook being captured on its home square, in one expression.
static const uint8_t castle_mask[128] = {
    [0x00] = CH_CASTLE_WQ,
    [0x04] = CH_CASTLE_WK | CH_CASTLE_WQ,
    [0x07] = CH_CASTLE_WK,
    [0x70] = CH_CASTLE_BQ,
    [0x74] = CH_CASTLE_BK | CH_CASTLE_BQ,
    [0x77] = CH_CASTLE_BK,
};

void ch_init(ch_pos_t* pos)
{
    static const uint8_t back[8] = { CH_ROOK, CH_KNIGHT, CH_BISHOP, CH_QUEEN, CH_KING, CH_BISHOP, CH_KNIGHT, CH_ROOK };

    memset(pos, 0, sizeof(*pos));
    for (int f = 0; f < 8; ++f) {
        pos->board[CH_SQ(f, 0)] = back[f] | CH_WHITE;
        pos->board[CH_SQ(f, 1)] = CH_PAWN | CH_WHITE;
        pos->board[CH_SQ(f, 6)] = CH_PAWN | CH_BLACK;
        pos->board[CH_SQ(f, 7)] = back[f] | CH_BLACK;
    }
    pos->side = CH_WHITE;
    pos->castle = CH_CASTLE_WK | CH_CASTLE_WQ | CH_CASTLE_BK | CH_CASTLE_BQ;
    pos->ep = CH_NO_EP;
    pos->halfmove = 0;
    pos->fullmove = 1;
    pos->king_sq[CH_CIDX(CH_WHITE)] = CH_SQ(4, 0);
    pos->king_sq[CH_CIDX(CH_BLACK)] = CH_SQ(4, 7);
    pos->hash = ch_zobrist(pos);
}

bool ch_from_fen(ch_pos_t* pos, const char* fen)
{
    memset(pos, 0, sizeof(*pos));
    pos->ep = CH_NO_EP;

    const char* p = fen;
    int rank = 7;
    int file = 0;

    for (; *p && *p != ' '; ++p) {
        if (*p == '/') {
            if (file != 8) {
                return false;
            }
            --rank;
            file = 0;
            if (rank < 0) {
                return false;
            }
            continue;
        }
        if (*p >= '1' && *p <= '8') {
            file += *p - '0';
            if (file > 8) {
                return false;
            }
            continue;
        }

        uint8_t type;
        switch (*p | 0x20) {
        case 'p':
            type = CH_PAWN;
            break;
        case 'n':
            type = CH_KNIGHT;
            break;
        case 'b':
            type = CH_BISHOP;
            break;
        case 'r':
            type = CH_ROOK;
            break;
        case 'q':
            type = CH_QUEEN;
            break;
        case 'k':
            type = CH_KING;
            break;
        default:
            return false;
        }
        if (file > 7 || rank < 0) {
            return false;
        }

        // Upper case is white in FEN
        const uint8_t colour = (*p >= 'A' && *p <= 'Z') ? CH_WHITE : CH_BLACK;
        const uint8_t sq = CH_SQ(file, rank);
        pos->board[sq] = type | colour;
        if (type == CH_KING) {
            pos->king_sq[CH_CIDX(colour)] = sq;
        }
        ++file;
    }
    if (rank != 0 || file != 8) {
        return false;
    }

    while (*p == ' ') {
        ++p;
    }
    if (*p == 'w') {
        pos->side = CH_WHITE;
    } else if (*p == 'b') {
        pos->side = CH_BLACK;
    } else {
        return false;
    }
    ++p;

    while (*p == ' ') {
        ++p;
    }
    if (*p == '-') {
        ++p;
    } else {
        for (; *p && *p != ' '; ++p) {
            switch (*p) {
            case 'K':
                pos->castle |= CH_CASTLE_WK;
                break;
            case 'Q':
                pos->castle |= CH_CASTLE_WQ;
                break;
            case 'k':
                pos->castle |= CH_CASTLE_BK;
                break;
            case 'q':
                pos->castle |= CH_CASTLE_BQ;
                break;
            default:
                return false;
            }
        }
    }

    while (*p == ' ') {
        ++p;
    }
    if (*p == '-') {
        ++p;
    } else if (p[0] >= 'a' && p[0] <= 'h' && p[1] >= '1' && p[1] <= '8') {
        pos->ep = (int8_t)CH_SQ(p[0] - 'a', p[1] - '1');
        p += 2;
    } else if (*p) {
        return false;
    }

    // Halfmove and fullmove counters are optional in the perft suites
    pos->halfmove = 0;
    pos->fullmove = 1;
    while (*p == ' ') {
        ++p;
    }
    if (*p >= '0' && *p <= '9') {
        unsigned v = 0;
        for (; *p >= '0' && *p <= '9'; ++p) {
            v = v * 10 + (unsigned)(*p - '0');
        }
        pos->halfmove = (uint16_t)v;
        while (*p == ' ') {
            ++p;
        }
        if (*p >= '0' && *p <= '9') {
            v = 0;
            for (; *p >= '0' && *p <= '9'; ++p) {
                v = v * 10 + (unsigned)(*p - '0');
            }
            pos->fullmove = (uint16_t)v;
        }
    }

    pos->hash = ch_zobrist(pos);
    return true;
}

bool ch_attacked(const ch_pos_t* pos, uint8_t sq, uint8_t by)
{
    // Pawns. A white pawn on sq-17/sq-15 attacks sq; a black pawn on
    // sq+17/sq+15 attacks sq.
    const int pawn_from[2] = { (by == CH_WHITE) ? -17 : 17, (by == CH_WHITE) ? -15 : 15 };
    for (int i = 0; i < 2; ++i) {
        const int from = (int)sq + pawn_from[i];
        if (CH_ONBOARD(from) && pos->board[from] == (CH_PAWN | by)) {
            return true;
        }
    }

    for (int i = 0; i < 8; ++i) {
        const int from = (int)sq + knight_off[i];
        if (CH_ONBOARD(from) && pos->board[from] == (CH_KNIGHT | by)) {
            return true;
        }
    }

    for (int i = 0; i < 8; ++i) {
        const int from = (int)sq + king_off[i];
        if (CH_ONBOARD(from) && pos->board[from] == (CH_KING | by)) {
            return true;
        }
    }

    for (int i = 0; i < 4; ++i) {
        for (int from = (int)sq + bishop_off[i]; CH_ONBOARD(from); from += bishop_off[i]) {
            const uint8_t pc = pos->board[from];
            if (pc != CH_EMPTY) {
                if (CH_COLOUR(pc) == by && (CH_TYPE(pc) == CH_BISHOP || CH_TYPE(pc) == CH_QUEEN)) {
                    return true;
                }
                break;
            }
        }
    }

    for (int i = 0; i < 4; ++i) {
        for (int from = (int)sq + rook_off[i]; CH_ONBOARD(from); from += rook_off[i]) {
            const uint8_t pc = pos->board[from];
            if (pc != CH_EMPTY) {
                if (CH_COLOUR(pc) == by && (CH_TYPE(pc) == CH_ROOK || CH_TYPE(pc) == CH_QUEEN)) {
                    return true;
                }
                break;
            }
        }
    }

    return false;
}

bool ch_in_check(const ch_pos_t* pos, uint8_t side)
{
    return ch_attacked(pos, pos->king_sq[CH_CIDX(side)], CH_OPP(side));
}

static void add_move(ch_move_t* out, int* n, uint8_t from, uint8_t to, uint8_t promo, uint8_t flags)
{
    out[*n].from = from;
    out[*n].to = to;
    out[*n].promo = promo;
    out[*n].flags = flags;
    ++*n;
}

// Emit a pawn move, expanding to four moves when it lands on the back rank.
static void add_pawn_move(ch_move_t* out, int* n, uint8_t from, uint8_t to, uint8_t flags, uint8_t colour)
{
    const int last_rank = (colour == CH_WHITE) ? 7 : 0;
    if (CH_RANK(to) == last_rank) {
        add_move(out, n, from, to, CH_QUEEN, flags | CH_MF_PROMO);
        add_move(out, n, from, to, CH_ROOK, flags | CH_MF_PROMO);
        add_move(out, n, from, to, CH_BISHOP, flags | CH_MF_PROMO);
        add_move(out, n, from, to, CH_KNIGHT, flags | CH_MF_PROMO);
    } else {
        add_move(out, n, from, to, 0, flags);
    }
}

static int gen_pseudo(const ch_pos_t* pos, ch_move_t* out)
{
    const uint8_t us = pos->side;
    const uint8_t them = CH_OPP(us);
    int n = 0;

    for (int sq = 0; sq < 128; ++sq) {
        if (!CH_ONBOARD(sq)) {
            continue;
        }
        const uint8_t pc = pos->board[sq];
        if (pc == CH_EMPTY || CH_COLOUR(pc) != us) {
            continue;
        }
        const uint8_t type = CH_TYPE(pc);

        if (type == CH_PAWN) {
            const int fwd = (us == CH_WHITE) ? 16 : -16;
            const int start_rank = (us == CH_WHITE) ? 1 : 6;

            const int one = sq + fwd;
            if (CH_ONBOARD(one) && pos->board[one] == CH_EMPTY) {
                add_pawn_move(out, &n, (uint8_t)sq, (uint8_t)one, 0, us);

                const int two = sq + 2 * fwd;
                if (CH_RANK(sq) == start_rank && CH_ONBOARD(two) && pos->board[two] == CH_EMPTY) {
                    add_move(out, &n, (uint8_t)sq, (uint8_t)two, 0, CH_MF_DOUBLE);
                }
            }

            const int cap[2] = { sq + fwd - 1, sq + fwd + 1 };
            for (int i = 0; i < 2; ++i) {
                if (!CH_ONBOARD(cap[i])) {
                    continue;
                }
                const uint8_t target = pos->board[cap[i]];
                if (target != CH_EMPTY && CH_COLOUR(target) == them) {
                    add_pawn_move(out, &n, (uint8_t)sq, (uint8_t)cap[i], CH_MF_CAPTURE, us);
                } else if (pos->ep != CH_NO_EP && cap[i] == pos->ep) {
                    add_move(out, &n, (uint8_t)sq, (uint8_t)cap[i], 0, CH_MF_CAPTURE | CH_MF_EP);
                }
            }
            continue;
        }

        if (type == CH_KNIGHT || type == CH_KING) {
            const int* offs = (type == CH_KNIGHT) ? knight_off : king_off;
            for (int i = 0; i < 8; ++i) {
                const int to = sq + offs[i];
                if (!CH_ONBOARD(to)) {
                    continue;
                }
                const uint8_t target = pos->board[to];
                if (target == CH_EMPTY) {
                    add_move(out, &n, (uint8_t)sq, (uint8_t)to, 0, 0);
                } else if (CH_COLOUR(target) == them) {
                    add_move(out, &n, (uint8_t)sq, (uint8_t)to, 0, CH_MF_CAPTURE);
                }
            }
            continue;
        }

        // Sliders
        const int* offs;
        int noffs;
        if (type == CH_BISHOP) {
            offs = bishop_off;
            noffs = 4;
        } else if (type == CH_ROOK) {
            offs = rook_off;
            noffs = 4;
        } else {
            offs = king_off; // queen: all eight directions
            noffs = 8;
        }
        for (int i = 0; i < noffs; ++i) {
            for (int to = sq + offs[i]; CH_ONBOARD(to); to += offs[i]) {
                const uint8_t target = pos->board[to];
                if (target == CH_EMPTY) {
                    add_move(out, &n, (uint8_t)sq, (uint8_t)to, 0, 0);
                    continue;
                }
                if (CH_COLOUR(target) == them) {
                    add_move(out, &n, (uint8_t)sq, (uint8_t)to, 0, CH_MF_CAPTURE);
                }
                break;
            }
        }
    }

    // Castling. The king must not start in check, pass through an attacked
    // square, or land on one; the squares between must be empty. The final
    // "lands on" test is left to the legality filter in ch_gen_legal().
    const int home = (us == CH_WHITE) ? 0 : 7;
    const uint8_t k_bit = (us == CH_WHITE) ? CH_CASTLE_WK : CH_CASTLE_BK;
    const uint8_t q_bit = (us == CH_WHITE) ? CH_CASTLE_WQ : CH_CASTLE_BQ;
    const uint8_t e = CH_SQ(4, home);

    if ((pos->castle & k_bit) && pos->board[CH_SQ(5, home)] == CH_EMPTY && pos->board[CH_SQ(6, home)] == CH_EMPTY
        && !ch_attacked(pos, e, them) && !ch_attacked(pos, CH_SQ(5, home), them)) {
        add_move(out, &n, e, CH_SQ(6, home), 0, CH_MF_CASTLE);
    }
    if ((pos->castle & q_bit) && pos->board[CH_SQ(3, home)] == CH_EMPTY && pos->board[CH_SQ(2, home)] == CH_EMPTY
        && pos->board[CH_SQ(1, home)] == CH_EMPTY && !ch_attacked(pos, e, them)
        && !ch_attacked(pos, CH_SQ(3, home), them)) {
        add_move(out, &n, e, CH_SQ(2, home), 0, CH_MF_CASTLE);
    }

    return n;
}

void ch_make(ch_pos_t* pos, const ch_move_t* move, ch_undo_t* undo)
{
    undo->move = *move;
    undo->castle = pos->castle;
    undo->ep = pos->ep;
    undo->halfmove = pos->halfmove;
    undo->fullmove = pos->fullmove;
    undo->hash = pos->hash;

    const uint8_t from = move->from;
    const uint8_t to = move->to;
    const uint8_t pc = pos->board[from];
    const uint8_t us = CH_COLOUR(pc);
    const uint8_t type = CH_TYPE(pc);

    uint64_t h = pos->hash;
    // Retire the old castling-rights and en-passant contributions; the new ones
    // are XORed back in once they are known, below.
    h ^= zob_castle[pos->castle & 0x0F];
    if (pos->ep != CH_NO_EP) h ^= zob_ep[CH_FILE(pos->ep)];

    h ^= zob_pc(pc, from); // piece leaves `from`

    if (move->flags & CH_MF_EP) {
        const uint8_t victim = (uint8_t)(CH_SQ(CH_FILE(to), CH_RANK(from)));
        undo->captured = pos->board[victim];
        h ^= zob_pc(undo->captured, victim);
        pos->board[victim] = CH_EMPTY;
    } else {
        undo->captured = pos->board[to];
        if (undo->captured != CH_EMPTY) h ^= zob_pc(undo->captured, to);
    }

    const uint8_t placed = (move->flags & CH_MF_PROMO) ? (uint8_t)(move->promo | us) : pc;
    h ^= zob_pc(placed, to); // piece arrives at `to`
    pos->board[to] = placed;
    pos->board[from] = CH_EMPTY;

    if (type == CH_KING) {
        pos->king_sq[CH_CIDX(us)] = to;
    }

    if (move->flags & CH_MF_CASTLE) {
        const int home = CH_RANK(from);
        if (CH_FILE(to) == 6) {
            const uint8_t rook = pos->board[CH_SQ(7, home)];
            h ^= zob_pc(rook, (uint8_t)CH_SQ(7, home));
            h ^= zob_pc(rook, (uint8_t)CH_SQ(5, home));
            pos->board[CH_SQ(5, home)] = rook;
            pos->board[CH_SQ(7, home)] = CH_EMPTY;
        } else {
            const uint8_t rook = pos->board[CH_SQ(0, home)];
            h ^= zob_pc(rook, (uint8_t)CH_SQ(0, home));
            h ^= zob_pc(rook, (uint8_t)CH_SQ(3, home));
            pos->board[CH_SQ(3, home)] = rook;
            pos->board[CH_SQ(0, home)] = CH_EMPTY;
        }
    }

    pos->castle &= (uint8_t)~(castle_mask[from] | castle_mask[to]);
    pos->ep = (move->flags & CH_MF_DOUBLE) ? (int8_t)((from + to) / 2) : CH_NO_EP;

    h ^= zob_castle[pos->castle & 0x0F]; // new castling rights
    if (pos->ep != CH_NO_EP) h ^= zob_ep[CH_FILE(pos->ep)];
    h ^= zob_side; // side to move always flips
    pos->hash = h;

    if (type == CH_PAWN || (move->flags & CH_MF_CAPTURE)) {
        pos->halfmove = 0;
    } else {
        ++pos->halfmove;
    }
    if (us == CH_BLACK) {
        ++pos->fullmove;
    }
    pos->side = CH_OPP(us);
}

void ch_unmake(ch_pos_t* pos, const ch_undo_t* undo)
{
    pos->hash = undo->hash;
    const ch_move_t* move = &undo->move;
    const uint8_t from = move->from;
    const uint8_t to = move->to;

    pos->side = CH_COLOUR(pos->board[to]);
    const uint8_t us = pos->side;

    pos->board[from] = (move->flags & CH_MF_PROMO) ? (uint8_t)(CH_PAWN | us) : pos->board[to];
    pos->board[to] = CH_EMPTY;

    if (move->flags & CH_MF_EP) {
        pos->board[CH_SQ(CH_FILE(to), CH_RANK(from))] = undo->captured;
    } else {
        pos->board[to] = undo->captured;
    }

    if (CH_TYPE(pos->board[from]) == CH_KING) {
        pos->king_sq[CH_CIDX(us)] = from;
    }

    if (move->flags & CH_MF_CASTLE) {
        const int home = CH_RANK(from);
        if (CH_FILE(to) == 6) {
            pos->board[CH_SQ(7, home)] = pos->board[CH_SQ(5, home)];
            pos->board[CH_SQ(5, home)] = CH_EMPTY;
        } else {
            pos->board[CH_SQ(0, home)] = pos->board[CH_SQ(3, home)];
            pos->board[CH_SQ(3, home)] = CH_EMPTY;
        }
    }

    pos->castle = undo->castle;
    pos->ep = undo->ep;
    pos->halfmove = undo->halfmove;
    pos->fullmove = undo->fullmove;
}

int ch_gen_legal(const ch_pos_t* pos, ch_move_t* out)
{
    ch_move_t pseudo[CH_MAX_MOVES];
    const int npseudo = gen_pseudo(pos, pseudo);
    const uint8_t us = pos->side;

    ch_pos_t work = *pos;
    int n = 0;
    for (int i = 0; i < npseudo; ++i) {
        ch_undo_t undo;
        ch_make(&work, &pseudo[i], &undo);
        if (!ch_attacked(&work, work.king_sq[CH_CIDX(us)], CH_OPP(us))) {
            out[n++] = pseudo[i];
        }
        ch_unmake(&work, &undo);
    }
    return n;
}

uint64_t ch_perft(ch_pos_t* pos, int depth)
{
    if (depth == 0) {
        return 1;
    }

    ch_move_t moves[CH_MAX_MOVES];
    const int n = ch_gen_legal(pos, moves);
    if (depth == 1) {
        return (uint64_t)n;
    }

    uint64_t nodes = 0;
    for (int i = 0; i < n; ++i) {
        ch_undo_t undo;
        ch_make(pos, &moves[i], &undo);
        nodes += ch_perft(pos, depth - 1);
        ch_unmake(pos, &undo);
    }
    return nodes;
}

// True when neither side has mating material (K vs K, K+minor vs K).
static bool insufficient_material(const ch_pos_t* pos)
{
    int minors = 0;
    for (int sq = 0; sq < 128; ++sq) {
        if (!CH_ONBOARD(sq)) {
            continue;
        }
        switch (CH_TYPE(pos->board[sq])) {
        case CH_EMPTY:
        case CH_KING:
            break;
        case CH_KNIGHT:
        case CH_BISHOP:
            if (++minors > 1) {
                return false;
            }
            break;
        default:
            return false; // pawn, rook or queen can mate
        }
    }
    return true;
}

ch_result_t ch_result(const ch_pos_t* pos)
{
    ch_move_t moves[CH_MAX_MOVES];
    if (ch_gen_legal(pos, moves) == 0) {
        return ch_in_check(pos, pos->side) ? CH_CHECKMATE : CH_STALEMATE;
    }
    if (pos->halfmove >= 100) {
        return CH_DRAW_FIFTY;
    }
    if (insufficient_material(pos)) {
        return CH_DRAW_MATERIAL;
    }
    return CH_ONGOING;
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

#define CH_INF 30000
#define CH_MATE 29000

// Mate scores (|score| within this of CH_MATE) are stored NODE-relative in the
// TT and converted back to ROOT-relative on probe, so a mate value reached via
// a transposition at a different ply is not misreported. Bound is well above
// any reachable ply (search depth + quiescence).
#define CH_MATE_THRESH (CH_MATE - 256)

static const int piece_value[7] = { 0, 100, 320, 330, 500, 900, 0 };

// Killer moves: up to two quiet moves per ply that caused a beta cutoff. Tried
// early in ordering (just below captures) so similar cutoffs recur cheaply.
// File-static because the search is single-threaded (one search at a time,
// serialised by the temporary-task mutex on device); reset at each search start.
// A slot is empty when from == to.
#define CH_MAX_PLY 40
static ch_move_t g_killers[CH_MAX_PLY][2];

static bool move_eq(const ch_move_t* a, const ch_move_t* b)
{
    return a->from == b->from && a->to == b->to && a->promo == b->promo;
}

// Piece-square tables, from white's point of view, indexed by rank*8+file with
// rank 0 = white's back rank. Black reads them mirrored.
// clang-format off
static const int8_t pst_pawn[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10,-20,-20, 10, 10,  5,
     5, -5,-10,  0,  0,-10, -5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5,  5, 10, 25, 25, 10,  5,  5,
    10, 10, 20, 30, 30, 20, 10, 10,
    50, 50, 50, 50, 50, 50, 50, 50,
     0,  0,  0,  0,  0,  0,  0,  0,
};
static const int8_t pst_knight[64] = {
   -50,-40,-30,-30,-30,-30,-40,-50,
   -40,-20,  0,  5,  5,  0,-20,-40,
   -30,  5, 10, 15, 15, 10,  5,-30,
   -30,  0, 15, 20, 20, 15,  0,-30,
   -30,  5, 15, 20, 20, 15,  5,-30,
   -30,  0, 10, 15, 15, 10,  0,-30,
   -40,-20,  0,  0,  0,  0,-20,-40,
   -50,-40,-30,-30,-30,-30,-40,-50,
};
static const int8_t pst_bishop[64] = {
   -20,-10,-10,-10,-10,-10,-10,-20,
   -10,  5,  0,  0,  0,  0,  5,-10,
   -10, 10, 10, 10, 10, 10, 10,-10,
   -10,  0, 10, 10, 10, 10,  0,-10,
   -10,  5,  5, 10, 10,  5,  5,-10,
   -10,  0,  5, 10, 10,  5,  0,-10,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -20,-10,-10,-10,-10,-10,-10,-20,
};
static const int8_t pst_rook[64] = {
     0,  0,  0,  5,  5,  0,  0,  0,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     5, 10, 10, 10, 10, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0,
};
static const int8_t pst_queen[64] = {
   -20,-10,-10, -5, -5,-10,-10,-20,
   -10,  0,  5,  0,  0,  0,  0,-10,
   -10,  5,  5,  5,  5,  5,  0,-10,
     0,  0,  5,  5,  5,  5,  0, -5,
    -5,  0,  5,  5,  5,  5,  0, -5,
   -10,  0,  5,  5,  5,  5,  0,-10,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -20,-10,-10, -5, -5,-10,-10,-20,
};
static const int8_t pst_king[64] = {
    20, 30, 10,  0,  0, 10, 30, 20,
    20, 20,  0,  0,  0,  0, 20, 20,
   -10,-20,-20,-20,-20,-20,-20,-10,
   -20,-30,-30,-40,-40,-30,-30,-20,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
};
// clang-format on

static const int8_t* pst_for(uint8_t type)
{
    switch (type) {
    case CH_PAWN:
        return pst_pawn;
    case CH_KNIGHT:
        return pst_knight;
    case CH_BISHOP:
        return pst_bishop;
    case CH_ROOK:
        return pst_rook;
    case CH_QUEEN:
        return pst_queen;
    default:
        return pst_king;
    }
}

// Score from the point of view of the side to move.
static int evaluate(const ch_pos_t* pos)
{
    int score = 0;
    for (int sq = 0; sq < 128; ++sq) {
        if (!CH_ONBOARD(sq)) {
            continue;
        }
        const uint8_t pc = pos->board[sq];
        if (pc == CH_EMPTY) {
            continue;
        }
        const uint8_t type = CH_TYPE(pc);
        const int file = CH_FILE(sq);
        const int rank = CH_RANK(sq);
        const bool white = CH_COLOUR(pc) == CH_WHITE;
        // Black mirrors the table vertically
        const int idx = white ? (rank * 8 + file) : ((7 - rank) * 8 + file);
        const int v = piece_value[type] + pst_for(type)[idx];
        score += white ? v : -v;
    }
    return (pos->side == CH_WHITE) ? score : -score;
}

// Order captures first, most-valuable-victim first. Cheap, and worth far more
// than its cost in a plain alpha-beta.
// `killers` is a 2-element array of quiet cutoff moves for this ply (may be
// NULL). Killer quiets sort just below promotions and above ordinary quiets.
static void sort_moves(const ch_pos_t* pos, ch_move_t* moves, int n, const ch_move_t* killers)
{
    int score[CH_MAX_MOVES];
    for (int i = 0; i < n; ++i) {
        if (moves[i].flags & CH_MF_CAPTURE) {
            const uint8_t victim = CH_TYPE(pos->board[moves[i].to]);
            const uint8_t attacker = CH_TYPE(pos->board[moves[i].from]);
            score[i] = 1000 + piece_value[victim] - piece_value[attacker] / 10;
        } else if (moves[i].flags & CH_MF_PROMO) {
            score[i] = 900;
        } else if (killers && killers[0].from != killers[0].to && move_eq(&moves[i], &killers[0])) {
            score[i] = 850;
        } else if (killers && killers[1].from != killers[1].to && move_eq(&moves[i], &killers[1])) {
            score[i] = 840;
        } else {
            score[i] = 0;
        }
    }
    // Insertion sort: n is small and this keeps the code obvious
    for (int i = 1; i < n; ++i) {
        const ch_move_t m = moves[i];
        const int s = score[i];
        int j = i - 1;
        for (; j >= 0 && score[j] < s; --j) {
            moves[j + 1] = moves[j];
            score[j + 1] = score[j];
        }
        moves[j + 1] = m;
        score[j + 1] = s;
    }
}

// Search only captures until the position is quiet, so the engine does not
// stop mid-exchange and misread a hanging piece as material won.
//
// `depth` bounds the extension: see CH_QUIESCE_MAX. Stopping early only costs
// accuracy in wild positions, whereas running out of stack on device is fatal.
static int quiesce(ch_pos_t* pos, int alpha, int beta, int depth)
{
    const int stand_pat = evaluate(pos);
    if (stand_pat >= beta) {
        return beta;
    }
    if (stand_pat > alpha) {
        alpha = stand_pat;
    }
    if (depth <= 0) {
        return alpha;
    }

    ch_move_t moves[CH_MAX_MOVES];
    const int n = ch_gen_legal(pos, moves);
    sort_moves(pos, moves, n, NULL); // quiescence: captures dominate, no killers

    for (int i = 0; i < n; ++i) {
        if (!(moves[i].flags & (CH_MF_CAPTURE | CH_MF_PROMO))) {
            continue;
        }
        ch_undo_t undo;
        ch_make(pos, &moves[i], &undo);
        const int score = -quiesce(pos, -beta, -alpha, depth - 1);
        ch_unmake(pos, &undo);

        if (score >= beta) {
            return beta;
        }
        if (score > alpha) {
            alpha = score;
        }
    }
    return alpha;
}

uint64_t ch_search_nodes = 0;

// True if `side` has any piece other than pawns and the king. Null-move pruning
// is unsafe without one (zugzwang), so this gates it.
static bool has_non_pawn_material(const ch_pos_t* pos, uint8_t side)
{
    for (int sq = 0; sq < 128; ++sq) {
        if (!CH_ONBOARD(sq)) {
            continue;
        }
        const uint8_t pc = pos->board[sq];
        if (pc == CH_EMPTY || CH_COLOUR(pc) != side) {
            continue;
        }
        const uint8_t t = CH_TYPE(pc);
        if (t != CH_PAWN && t != CH_KING) {
            return true;
        }
    }
    return false;
}

static int negamax(ch_pos_t* pos, int depth, int alpha, int beta, int ply, ch_tt_t* tt)
{
    ++ch_search_nodes;
    const int alpha_orig = alpha;

    ch_tt_entry_t hit;
    ch_move_t tt_move = { 0, 0, 0, 0 };
    if (tt && ch_tt_probe(tt, pos->hash, &hit)) {
        tt_move = hit.move;
        int tt_score = hit.score;
        if (tt_score > CH_MATE_THRESH) tt_score -= ply;
        else if (tt_score < -CH_MATE_THRESH) tt_score += ply;
        if (hit.depth >= depth) {
            if (hit.flag == CH_TT_EXACT) return tt_score;
            if (hit.flag == CH_TT_LOWER && tt_score > alpha) alpha = tt_score;
            else if (hit.flag == CH_TT_UPPER && tt_score < beta) beta = tt_score;
            if (alpha >= beta) return tt_score;
        }
    }

    if (depth <= 0) {
        return quiesce(pos, alpha, beta, CH_QUIESCE_MAX);
    }

    ch_move_t moves[CH_MAX_MOVES];
    const int n = ch_gen_legal(pos, moves);
    const bool in_check = ch_in_check(pos, pos->side);
    if (n == 0) {
        return in_check ? -CH_MATE + ply : 0;
    }
    if (pos->halfmove >= 100) {
        return 0;
    }

    // Null-move pruning: if handing the opponent a free move still leaves us at
    // or above beta (verified by a shallow reduced-depth search), the position
    // is too strong to be worth a full search -- prune. Skipped in check
    // (cannot legally pass out of check), at shallow depth, near mate scores,
    // and with no non-pawn material (zugzwang, where passing may be forced-good).
    // Not TT-gated, so the with/without-TT search still returns identical scores.
    if (depth >= 3 && !in_check && beta < CH_MATE_THRESH && has_non_pawn_material(pos, pos->side)) {
        const int R = 2;
        const int8_t saved_ep = pos->ep;
        const uint64_t saved_hash = pos->hash;
        pos->hash ^= zob_side;
        if (pos->ep != CH_NO_EP) {
            pos->hash ^= zob_ep[CH_FILE(pos->ep)];
        }
        pos->ep = CH_NO_EP;
        pos->side = CH_OPP(pos->side);
        const int null_score = -negamax(pos, depth - 1 - R, -beta, -beta + 1, ply + 1, tt);
        pos->side = CH_OPP(pos->side);
        pos->ep = saved_ep;
        pos->hash = saved_hash;
        if (null_score >= beta) {
            return beta;
        }
    }

    sort_moves(pos, moves, n, (ply < CH_MAX_PLY) ? g_killers[ply] : NULL);

    // If the TT suggested a move, search it first.
    if (tt_move.from != tt_move.to) {
        for (int i = 1; i < n; ++i) {
            if (moves[i].from == tt_move.from && moves[i].to == tt_move.to && moves[i].promo == tt_move.promo) {
                const ch_move_t tmp = moves[0]; moves[0] = moves[i]; moves[i] = tmp;
                break;
            }
        }
    }

    int best = -CH_INF;
    ch_move_t best_move = moves[0];
    for (int i = 0; i < n; ++i) {
        ch_undo_t undo;
        ch_make(pos, &moves[i], &undo);
        const int score = -negamax(pos, depth - 1, -beta, -alpha, ply + 1, tt);
        ch_unmake(pos, &undo);
        if (score > best) { best = score; best_move = moves[i]; }
        if (score > alpha) alpha = score;
        if (alpha >= beta) {
            // Remember a quiet cutoff move as a killer for this ply.
            if (!(moves[i].flags & (CH_MF_CAPTURE | CH_MF_PROMO)) && ply < CH_MAX_PLY
                && !move_eq(&moves[i], &g_killers[ply][0])) {
                g_killers[ply][1] = g_killers[ply][0];
                g_killers[ply][0] = moves[i];
            }
            break;
        }
    }

    if (tt) {
        const uint8_t flag = (best <= alpha_orig) ? CH_TT_UPPER : (best >= beta) ? CH_TT_LOWER : CH_TT_EXACT;
        int store_score = best;
        if (store_score > CH_MATE_THRESH) store_score += ply;
        else if (store_score < -CH_MATE_THRESH) store_score -= ply;
        ch_tt_store(tt, pos->hash, depth, store_score, flag, best_move);
    }
    return best;
}

static uint32_t xrng(uint32_t* s) {
    uint32_t x = *s ? *s : 0x1234567u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

// Root search to `depth` via iterative deepening, writing per-move scores into
// `scores` (parallel to `moves`) and returning the best score. `moves`/`scores`
// must hold CH_MAX_MOVES.
//
// The root window's lower bound is `best_so_far - margin`, so alpha-beta
// narrowing still prunes (each root move only has to beat the running best by
// `margin` to matter), while every move within `margin` of the best is scored
// exactly for the randomness pool in ch_search_ex. margin 0 is the standard
// efficient root search. A FULL window here instead would disable root pruning
// entirely and blow up the node count (~8x at depth 7). The root move list is
// not reordered, so moves[i] and scores[i] stay parallel for the caller.
static int search_root(ch_pos_t* pos, int depth, int margin, ch_tt_t* tt, ch_move_t* moves, int* scores, int* count) {
    const int n = ch_gen_legal(pos, moves);
    *count = n;
    if (n == 0) return 0;
    sort_moves(pos, moves, n, NULL); // root ordering comes from the TT across ID iterations

    int best = -CH_INF;
    for (int d = 1; d <= depth; ++d) {
        best = -CH_INF;
        for (int i = 0; i < n; ++i) {
            ch_undo_t undo;
            ch_make(pos, &moves[i], &undo);
            const int alpha = (best == -CH_INF) ? -CH_INF : best - margin;
            scores[i] = -negamax(pos, d - 1, -CH_INF, -alpha, 1, tt);
            ch_unmake(pos, &undo);
            if (scores[i] > best) best = scores[i];
        }
    }
    return best;
}

int ch_search_bestscore(ch_pos_t* pos, int depth, ch_tt_t* tt) {
    ch_search_nodes = 0;
    memset(g_killers, 0, sizeof(g_killers));
    ch_move_t moves[CH_MAX_MOVES];
    int scores[CH_MAX_MOVES];
    int n = 0;
    return search_root(pos, depth, 0, tt, moves, scores, &n);
}

bool ch_search_ex(ch_pos_t* pos, int depth, int margin, uint32_t* rng_state, ch_tt_t* tt, ch_move_t* best) {
    ch_search_nodes = 0;
    memset(g_killers, 0, sizeof(g_killers));
    ch_move_t moves[CH_MAX_MOVES];
    int scores[CH_MAX_MOVES];
    int n = 0;
    // For the randomness pool we need EXACT scores for every move within
    // `margin` of the best, and moves outside it to fail low *strictly* below
    // the pool threshold. A root window low bound of best-margin-1 does both:
    // an in-margin move (value >= best-margin) exceeds it and is scored exactly,
    // while an out-of-margin move (value < best-margin, i.e. <= best-margin-1 in
    // integer cp) fails low with a bound <= best-margin-1 < best-margin and is
    // excluded. margin 0 / no rng searches strict-best for tightest pruning.
    const int root_margin = (margin > 0 && rng_state) ? margin + 1 : 0;
    const int best_score = search_root(pos, depth, root_margin, tt, moves, scores, &n);
    if (n == 0) return false;

    if (margin > 0 && rng_state) {
        // Collect moves within `margin` of the best, pick one at random.
        int pool[CH_MAX_MOVES]; int m = 0;
        for (int i = 0; i < n; ++i) if (scores[i] >= best_score - margin) pool[m++] = i;
        *best = moves[pool[xrng(rng_state) % (uint32_t)m]];
        return true;
    }

    // Strict best move.
    int bi = 0;
    for (int i = 1; i < n; ++i) if (scores[i] > scores[bi]) bi = i;
    *best = moves[bi];
    return true;
}

bool ch_search(ch_pos_t* pos, int depth, ch_move_t* best) {
    return ch_search_ex(pos, depth, 0, NULL, NULL, best);
}

// Transposition table implementation
size_t ch_tt_sizeof(size_t count) { return count * sizeof(ch_tt_entry_t); }

void ch_tt_init(ch_tt_t* tt, void* buffer, size_t count) {
    tt->entries = (ch_tt_entry_t*)buffer;
    tt->count = count;
    ch_tt_clear(tt);
}

void ch_tt_clear(ch_tt_t* tt) {
    memset(tt->entries, 0, tt->count * sizeof(ch_tt_entry_t));
    // flag CH_TT_NONE == 0, so a zeroed table reads as empty
}

bool ch_tt_probe(const ch_tt_t* tt, uint64_t key, ch_tt_entry_t* out) {
    const ch_tt_entry_t* e = &tt->entries[key % tt->count];
    if (e->flag == CH_TT_NONE || e->key != key) return false;
    *out = *e;
    return true;
}

void ch_tt_store(ch_tt_t* tt, uint64_t key, int depth, int score, uint8_t flag, ch_move_t move) {
    ch_tt_entry_t* e = &tt->entries[key % tt->count];
    // Depth-preferred replacement: keep a deeper existing entry for the same key.
    if (e->flag != CH_TT_NONE && e->key == key && e->depth > depth) return;
    e->key = key;
    e->move = move;
    e->score = (int16_t)score;
    e->depth = (uint8_t)depth;
    e->flag = flag;
}

#endif // AMALGAMATED_BUILD
