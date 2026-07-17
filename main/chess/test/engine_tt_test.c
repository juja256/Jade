// Host tests for Zobrist hashing and the transposition table.
//   cc -O2 -o engine_tt_test main/chess/test/engine_tt_test.c main/chess/engine.c -Imain/chess
//   ./engine_tt_test
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void check(bool ok, const char* what) {
    printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

static void test_hash_matches_recompute_after_moves(void) {
    ch_pos_t pos; ch_init(&pos);
    check(pos.hash == ch_zobrist(&pos), "startpos hash == recompute");

    // Play a short line, asserting the incremental hash matches recompute.
    const char* ucis[] = { "e2e4", "c7c5", "g1f3", "b8c6", "f1b5", "e7e6" };
    ch_undo_t undo[6];
    for (int i = 0; i < 6; ++i) {
        ch_move_t moves[CH_MAX_MOVES];
        const int n = ch_gen_legal(&pos, moves);
        const uint8_t from = CH_SQ(ucis[i][0]-'a', ucis[i][1]-'1');
        const uint8_t to   = CH_SQ(ucis[i][2]-'a', ucis[i][3]-'1');
        ch_move_t mv = {0}; bool found = false;
        for (int j = 0; j < n; ++j) if (moves[j].from==from && moves[j].to==to) { mv = moves[j]; found = true; break; }
        if (!found) { check(false, "line move is legal"); return; }
        ch_make(&pos, &mv, &undo[i]);
        char what[64]; snprintf(what, sizeof(what), "incremental hash == recompute after %s", ucis[i]);
        check(pos.hash == ch_zobrist(&pos), what);
    }
    // Unwind, asserting the hash is restored each step.
    for (int i = 5; i >= 0; --i) {
        ch_unmake(&pos, &undo[i]);
        check(pos.hash == ch_zobrist(&pos), "hash restored on unmake");
    }
}

static void test_transposition_equal(void) {
    // 1.e4 e5 2.Nf3  vs  1.Nf3 e5 2.e4  reach the same position.
    ch_pos_t a; ch_from_fen(&a, "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2");
    ch_pos_t b; ch_from_fen(&b, "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2");
    check(a.hash == b.hash, "identical FENs hash equal");

    ch_pos_t c; ch_from_fen(&c, "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq e3 1 2");
    check(a.hash != c.hash, "different ep target hashes differ");
}

// Walk the move tree to `depth`, asserting the incremental hash matches the
// from-scratch recompute after every make and every unmake. Exercises captures,
// en passant, castling and promotion, which the linear line above does not.
static bool walk_hash_ok(ch_pos_t* pos, int depth) {
    if (depth == 0) return true;
    ch_move_t moves[CH_MAX_MOVES];
    const int n = ch_gen_legal(pos, moves);
    for (int i = 0; i < n; ++i) {
        ch_undo_t u;
        ch_make(pos, &moves[i], &u);
        if (pos->hash != ch_zobrist(pos)) return false;
        if (!walk_hash_ok(pos, depth - 1)) return false;
        ch_unmake(pos, &u);
        if (pos->hash != ch_zobrist(pos)) return false;
    }
    return true;
}

static void test_hash_consistency_over_move_tree(void) {
    static const char* const fens[] = {
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", // kiwipete: captures, castling
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",                            // position3: en passant
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",     // position4: promotions
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",            // position5: castling rights, promo
    };
    for (size_t i = 0; i < sizeof(fens) / sizeof(fens[0]); ++i) {
        ch_pos_t pos;
        if (!ch_from_fen(&pos, fens[i])) { check(false, "walk FEN parses"); continue; }
        char what[64];
        snprintf(what, sizeof(what), "hash consistent over move tree (fen %zu, depth 3)", i);
        check(walk_hash_ok(&pos, 3), what);
    }
}

static void test_tt_store_probe(void) {
    const size_t N = 1024;
    void* buf = malloc(ch_tt_sizeof(N));
    ch_tt_t tt; ch_tt_init(&tt, buf, N);

    ch_tt_entry_t out;
    check(!ch_tt_probe(&tt, 0x1234, &out), "probe of empty table misses");

    ch_move_t mv = { .from = 12, .to = 28, .promo = 0, .flags = 0 };
    ch_tt_store(&tt, 0x1234, 5, 42, CH_TT_EXACT, mv);
    check(ch_tt_probe(&tt, 0x1234, &out), "probe finds the stored key");
    check(out.score == 42 && out.depth == 5 && out.flag == CH_TT_EXACT, "stored fields round-trip");
    check(out.move.from == 12 && out.move.to == 28, "stored move round-trips");
    check(!ch_tt_probe(&tt, 0x9999, &out), "probe of a different key misses");

    ch_tt_clear(&tt);
    check(!ch_tt_probe(&tt, 0x1234, &out), "clear empties the table");
    free(buf);
}

static uint32_t rng = 0xC0FFEEu;

static void test_tt_invariance(void) {
    const char* fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    };
    void* buf = malloc(ch_tt_sizeof(1 << 15));
    ch_tt_t tt; ch_tt_init(&tt, buf, 1 << 15);
    for (size_t i = 0; i < sizeof(fens)/sizeof(fens[0]); ++i) {
        for (int d = 1; d <= 4; ++d) {
            ch_pos_t a; ch_from_fen(&a, fens[i]);
            ch_pos_t b; ch_from_fen(&b, fens[i]);
            ch_tt_clear(&tt);
            const int with = ch_search_bestscore(&a, d, &tt);
            const int without = ch_search_bestscore(&b, d, NULL);
            char what[80]; snprintf(what, sizeof(what), "TT-invariant: fen %zu depth %d (%d==%d)", i, d, with, without);
            check(with == without, what);
        }
    }
    free(buf);
}

static void test_margin_stays_within_bound(void) {
    ch_pos_t pos; ch_init(&pos);
    const int depth = 4, margin = 40;
    const int best = ch_search_bestscore(&pos, depth, NULL);

    // With a margin, the chosen move must score within `margin` of best.
    for (int t = 0; t < 20; ++t) {
        ch_move_t mv;
        if (!ch_search_ex(&pos, depth, margin, &rng, NULL, &mv)) { check(false, "search returns a move"); return; }
        ch_undo_t u; ch_make(&pos, &mv, &u);
        const int chosen = -ch_search_bestscore(&pos, depth - 1, NULL); // value of the chosen move
        ch_unmake(&pos, &u);
        if (chosen < best - margin) { check(false, "chosen move within margin of best"); return; }
    }
    check(true, "chosen move within margin of best (20 draws)");
}

static void test_margin_zero_is_deterministic(void) {
    ch_pos_t pos; ch_init(&pos);
    ch_move_t a, b;
    ch_search_ex(&pos, 4, 0, &rng, NULL, &a);
    ch_search_ex(&pos, 4, 0, &rng, NULL, &b);
    check(a.from == b.from && a.to == b.to, "margin 0 is deterministic");
}

int main(void) {
    printf("\nzobrist\n");
    test_hash_matches_recompute_after_moves();
    test_transposition_equal();
    test_hash_consistency_over_move_tree();

    printf("\ntt\n");
    test_tt_store_probe();

    printf("\nsearch\n");
    test_tt_invariance();
    test_margin_stays_within_bound();
    test_margin_zero_is_deterministic();

    if (failures) { printf("\n%d test(s) FAILED\n", failures); return 1; }
    printf("\nall engine_tt tests passed\n");
    return 0;
}
