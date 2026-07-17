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

int main(void) {
    printf("\nzobrist\n");
    test_hash_matches_recompute_after_moves();
    test_transposition_equal();
    test_hash_consistency_over_move_tree();
    if (failures) { printf("\n%d test(s) FAILED\n", failures); return 1; }
    printf("\nall engine_tt tests passed\n");
    return 0;
}
