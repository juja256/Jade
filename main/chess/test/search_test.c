// Host-side search and game-state tests for the chess engine.
//
// perft_test.c proves move generation; this covers the parts perft cannot see:
// that the search finds forced mates, that terminal states are classified
// correctly, and that a full engine-vs-engine game runs to completion without
// producing an illegal move or crashing.
//
//     cc -O2 -o search_test main/chess/test/search_test.c main/chess/engine.c -Imain/chess
//     ./search_test

#include "engine.h"

#include <stdio.h>
#include <string.h>

static int failures;

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

static void check(bool ok, const char* what)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        ++failures;
    }
}

// The engine must find *a* mating move -- which one is irrelevant, so assert
// on the resulting position rather than on a specific move.
static void test_finds_mate_in_1(const char* fen, const char* name)
{
    ch_pos_t pos;
    if (!ch_from_fen(&pos, fen)) {
        printf("  %-52s FAIL (bad FEN)\n", name);
        ++failures;
        return;
    }

    ch_move_t best;
    if (!ch_search(&pos, 2, &best)) {
        printf("  %-52s FAIL (no move found)\n", name);
        ++failures;
        return;
    }

    ch_undo_t undo;
    ch_make(&pos, &best, &undo);
    const ch_result_t result = ch_result(&pos);

    char buf[8];
    move_str(&best, buf);
    char what[80];
    snprintf(what, sizeof(what), "%s: plays %s -> checkmate", name, buf);
    check(result == CH_CHECKMATE, what);
}

static void test_result(const char* fen, ch_result_t expect, const char* name)
{
    ch_pos_t pos;
    if (!ch_from_fen(&pos, fen)) {
        printf("  %-52s FAIL (bad FEN)\n", name);
        ++failures;
        return;
    }
    check(ch_result(&pos) == expect, name);
}

// Engine vs engine. Catches make/unmake state corruption that perft would miss
// because perft always unwinds, whereas a real game never does.
static void test_self_play(void)
{
    ch_pos_t pos;
    ch_init(&pos);

    int plies = 0;
    ch_result_t result = CH_ONGOING;

    for (; plies < 120; ++plies) {
        result = ch_result(&pos);
        if (result != CH_ONGOING) {
            break;
        }

        ch_move_t best;
        if (!ch_search(&pos, 3, &best)) {
            break;
        }

        // The engine's own move must appear in the legal list
        ch_move_t legal[CH_MAX_MOVES];
        const int n = ch_gen_legal(&pos, legal);
        bool found = false;
        for (int i = 0; i < n; ++i) {
            if (legal[i].from == best.from && legal[i].to == best.to && legal[i].promo == best.promo) {
                found = true;
                break;
            }
        }
        if (!found) {
            char buf[8];
            move_str(&best, buf);
            printf("  self-play: ILLEGAL move %s at ply %d\n", buf, plies);
            ++failures;
            return;
        }

        ch_undo_t undo;
        ch_make(&pos, &best, &undo);
    }

    char what[80];
    snprintf(what, sizeof(what), "self-play: %d plies, all legal, no crash", plies);
    check(true, what);

    static const char* names[] = { "ongoing", "checkmate", "stalemate", "fifty-move", "insufficient material" };
    printf("      (ended: %s)\n", names[result]);
}

// make() then unmake() must restore the position exactly. Perft would catch
// most such bugs, but this pins the failure to the actual culprit.
static void test_make_unmake_roundtrip(const char* fen, const char* name)
{
    ch_pos_t pos;
    if (!ch_from_fen(&pos, fen)) {
        printf("  %-52s FAIL (bad FEN)\n", name);
        ++failures;
        return;
    }

    const ch_pos_t before = pos;
    ch_move_t moves[CH_MAX_MOVES];
    const int n = ch_gen_legal(&pos, moves);

    for (int i = 0; i < n; ++i) {
        ch_undo_t undo;
        ch_make(&pos, &moves[i], &undo);
        ch_unmake(&pos, &undo);
        if (memcmp(&before, &pos, sizeof(pos)) != 0) {
            char buf[8];
            move_str(&moves[i], buf);
            printf("  %s: FAIL (position altered by %s)\n", name, buf);
            ++failures;
            return;
        }
    }
    check(true, name);
}

int main(void)
{
    printf("\nmate detection\n");
    test_finds_mate_in_1("6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1", "back-rank rook");
    test_finds_mate_in_1("7k/6pp/8/8/8/8/8/R6K w - - 0 1", "back-rank rook (bare)");
    test_finds_mate_in_1("6k1/8/6K1/8/8/8/8/7Q w - - 0 1", "queen + king");

    printf("\nterminal states\n");
    test_result("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", CH_STALEMATE, "stalemate detected");
    test_result("6k1/5ppp/8/8/8/8/5PPP/6K1 w - - 0 1", CH_ONGOING, "ongoing position");
    test_result("8/8/4k3/8/8/4K3/8/8 w - - 0 1", CH_DRAW_MATERIAL, "K vs K insufficient");
    test_result("8/8/4k3/8/8/4KB2/8/8 w - - 0 1", CH_DRAW_MATERIAL, "K+B vs K insufficient");
    test_result("8/8/4k3/8/8/4KR2/8/8 w - - 0 1", CH_ONGOING, "K+R vs K is sufficient");
    test_result("R5k1/5ppp/8/8/8/8/5PPP/6K1 b - - 0 1", CH_CHECKMATE, "checkmate detected");

    printf("\nmake/unmake roundtrip\n");
    test_make_unmake_roundtrip("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "startpos");
    test_make_unmake_roundtrip("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", "kiwipete");
    test_make_unmake_roundtrip("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", "position3 (ep)");
    test_make_unmake_roundtrip("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", "position4 (promo)");

    printf("\nself-play\n");
    test_self_play();

    if (failures) {
        printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall search tests passed\n");
    return 0;
}
