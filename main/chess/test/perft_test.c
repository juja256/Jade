// Host-side perft test for the chess engine.
//
// Perft counts leaf nodes of the move tree at a fixed depth. The reference
// counts below are the standard published values, so any mismatch is a
// move-generation bug in engine.c -- castling, en passant, promotion and pin
// handling are all exercised.
//
// Builds and runs with no ESP-IDF, no firmware and no device:
//
//     cc -O2 -o perft_test main/chess/test/perft_test.c main/chess/engine.c -Imain/chess
//     ./perft_test
//
// Pass --divide "<fen>" <depth> to print per-move counts, which is how you
// localise a movegen bug when a count disagrees.

#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

typedef struct {
    const char* name;
    const char* fen;
    int depth;
    uint64_t expected;
} perft_case_t;

// Standard perft suite (chessprogramming.org). Depths chosen to stay decisive
// while keeping the whole run to a few seconds.
static const perft_case_t cases[] = {
    { "startpos", START_FEN, 1, 20 },
    { "startpos", START_FEN, 2, 400 },
    { "startpos", START_FEN, 3, 8902 },
    { "startpos", START_FEN, 4, 197281 },
    { "startpos", START_FEN, 5, 4865609 },

    // "Kiwipete": dense with castling, pins and captures
    { "kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 1, 48 },
    { "kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 2, 2039 },
    { "kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97862 },
    { "kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603 },

    // Position 3: en-passant and promotion heavy endgame
    { "position3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 1, 14 },
    { "position3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 2, 191 },
    { "position3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 3, 2812 },
    { "position3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4, 43238 },
    { "position3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624 },

    // Position 4: promotions and an awkward pin
    { "position4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 1, 6 },
    { "position4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 2, 264 },
    { "position4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 3, 9467 },
    { "position4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333 },

    // Position 5: catches castling-rights bugs specifically
    { "position5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 1, 44 },
    { "position5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 2, 1486 },
    { "position5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3, 62379 },
    { "position5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487 },

    // Position 6
    { "position6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 1, 46 },
    { "position6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 2, 2079 },
    { "position6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 3, 89890 },
    { "position6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594 },
};

static void move_str(const ch_move_t* m, char* buf)
{
    buf[0] = (char)('a' + CH_FILE(m->from));
    buf[1] = (char)('1' + CH_RANK(m->from));
    buf[2] = (char)('a' + CH_FILE(m->to));
    buf[3] = (char)('1' + CH_RANK(m->to));
    if (m->flags & CH_MF_PROMO) {
        buf[4] = "..nbrq"[m->promo];
        buf[5] = '\0';
    } else {
        buf[4] = '\0';
    }
}

// Per-move node counts: compare against a known-good engine to find which
// move's subtree diverges, then recurse into it.
static void divide(ch_pos_t* pos, int depth)
{
    ch_move_t moves[CH_MAX_MOVES];
    const int n = ch_gen_legal(pos, moves);
    uint64_t total = 0;

    for (int i = 0; i < n; ++i) {
        ch_undo_t undo;
        ch_make(pos, &moves[i], &undo);
        const uint64_t nodes = ch_perft(pos, depth - 1);
        ch_unmake(pos, &undo);

        char buf[8];
        move_str(&moves[i], buf);
        printf("%-6s %llu\n", buf, (unsigned long long)nodes);
        total += nodes;
    }
    printf("\nmoves %d\nnodes %llu\n", n, (unsigned long long)total);
}

int main(int argc, char** argv)
{
    if (argc == 4 && !strcmp(argv[1], "--divide")) {
        ch_pos_t pos;
        if (!ch_from_fen(&pos, argv[2])) {
            fprintf(stderr, "bad FEN\n");
            return 2;
        }
        divide(&pos, atoi(argv[3]));
        return 0;
    }

    int failed = 0;
    const char* last = NULL;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const perft_case_t* c = &cases[i];
        if (last != c->name) {
            printf("\n%s\n", c->name);
            last = c->name;
        }

        ch_pos_t pos;
        if (!ch_from_fen(&pos, c->fen)) {
            printf("  depth %d  FEN PARSE FAILED\n", c->depth);
            ++failed;
            continue;
        }

        const uint64_t got = ch_perft(&pos, c->depth);
        if (got == c->expected) {
            printf("  depth %d  %10llu  ok\n", c->depth, (unsigned long long)got);
        } else {
            printf("  depth %d  %10llu  FAIL (expected %llu, off by %+lld)\n", c->depth, (unsigned long long)got,
                (unsigned long long)c->expected, (long long)got - (long long)c->expected);
            ++failed;
        }
    }

    if (failed) {
        printf("\n%d perft case(s) FAILED\n", failed);
        printf("Localise with: %s --divide \"<fen>\" <depth>\n", argv[0]);
        return 1;
    }
    printf("\nall %zu perft cases passed\n", sizeof(cases) / sizeof(cases[0]));
    return 0;
}
