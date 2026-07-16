// Host-side rendering and SAN tests.
//
//     cc -O2 -o render_test main/chess/test/render_test.c main/chess/chess_board.c main/chess/engine.c -Imain/chess
//     ./render_test [outdir]
//
// Writes PPM images of rendered boards to `outdir` (default: current dir) so
// the art can be looked at, which is the only way to judge 20px sprites.

#include "chess_board.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(bool ok, const char* what)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        ++failures;
    }
}

// Undo the panel's byte swap and expand RGB565 back to 8-bit channels, so the
// PPM shows exactly what the device would show. If this and chess_board.c
// disagree about the format, the image comes out visibly wrong -- which is the
// point.
static void unpack(uint16_t px, unsigned char* rgb)
{
    const uint16_t c = (uint16_t)((px >> 8) | (px << 8));
    const unsigned r5 = (c >> 11) & 0x1F;
    const unsigned g6 = (c >> 5) & 0x3F;
    const unsigned b5 = c & 0x1F;
    rgb[0] = (unsigned char)((r5 * 255 + 15) / 31);
    rgb[1] = (unsigned char)((g6 * 255 + 31) / 63);
    rgb[2] = (unsigned char)((b5 * 255 + 15) / 31);
}

static bool write_ppm(const char* path, const uint16_t* buf)
{
    FILE* f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    fprintf(f, "P6\n%d %d\n255\n", CHB_BOARD_PX, CHB_BOARD_PX);
    for (int i = 0; i < CHB_BUF_PIXELS; ++i) {
        unsigned char rgb[3];
        unpack(buf[i], rgb);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
}

static void test_san(const char* fen, const char* uci, const char* expect)
{
    ch_pos_t pos;
    if (!ch_from_fen(&pos, fen)) {
        printf("  SAN %-48s FAIL (bad FEN)\n", uci);
        ++failures;
        return;
    }

    const uint8_t from = (uint8_t)CH_SQ(uci[0] - 'a', uci[1] - '1');
    const uint8_t to = (uint8_t)CH_SQ(uci[2] - 'a', uci[3] - '1');

    ch_move_t moves[CH_MAX_MOVES];
    const int n = ch_gen_legal(&pos, moves);
    for (int i = 0; i < n; ++i) {
        if (moves[i].from != from || moves[i].to != to) {
            continue;
        }
        // Match the promotion piece when the UCI string names one
        if (uci[4] && (!(moves[i].flags & CH_MF_PROMO) || "..nbrq"[moves[i].promo] != uci[4])) {
            continue;
        }
        if (!uci[4] && (moves[i].flags & CH_MF_PROMO) && moves[i].promo != CH_QUEEN) {
            continue;
        }

        char san[CHB_SAN_LEN];
        chb_move_san(&pos, &moves[i], san, sizeof(san));
        char what[80];
        snprintf(what, sizeof(what), "SAN %s -> %s", uci, san);
        check(strcmp(san, expect) == 0, what);
        return;
    }

    printf("  SAN %-48s FAIL (move not legal)\n", uci);
    ++failures;
}

int main(int argc, char** argv)
{
    const char* outdir = (argc > 1) ? argv[1] : ".";
    static uint16_t buf[CHB_BUF_PIXELS];
    char path[512];

    printf("\npiece art\n");
    check(chb_art_valid(), "all sprites are 20x20 and well-formed");

    printf("\nrendering\n");
    chb_view_t view;
    chb_view_init(&view);
    chb_render(&view, buf);

    snprintf(path, sizeof(path), "%s/board_start.ppm", outdir);
    check(write_ppm(path, buf), "start position rendered");

    // Every pixel written? A miss would leave the zero-fill showing.
    bool all_written = true;
    for (int i = 0; i < CHB_BUF_PIXELS; ++i) {
        if (buf[i] == 0) {
            all_written = false;
            break;
        }
    }
    check(all_written, "no unwritten pixels");

    // Highlights and a mid-game position with check
    chb_view_t hl;
    chb_view_init(&hl);
    ch_from_fen(&hl.pos, "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5Q2/PPPP1PPP/RNB1K1NR w KQkq - 4 4");
    hl.sel_from = (uint8_t)CH_SQ(5, 2); // f3 queen
    hl.sel_to = (uint8_t)CH_SQ(5, 6); // f7
    hl.last_from = (uint8_t)CH_SQ(2, 5);
    hl.last_to = (uint8_t)CH_SQ(2, 5);
    chb_render(&hl, buf);
    snprintf(path, sizeof(path), "%s/board_highlight.ppm", outdir);
    check(write_ppm(path, buf), "highlighted position rendered");

    // A position where black is in check, to exercise the check tint
    chb_view_t chk;
    chb_view_init(&chk);
    ch_from_fen(&chk.pos, "rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq - 0 2");
    ch_from_fen(&chk.pos, "rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3");
    chb_render(&chk, buf);
    snprintf(path, sizeof(path), "%s/board_check.ppm", outdir);
    check(write_ppm(path, buf), "check position rendered (white in check)");

    // Flipped board
    chb_view_t fl;
    chb_view_init(&fl);
    fl.flipped = true;
    chb_render(&fl, buf);
    snprintf(path, sizeof(path), "%s/board_flipped.ppm", outdir);
    check(write_ppm(path, buf), "flipped board rendered");

    printf("\ngeometry\n");
    chb_view_t v;
    chb_view_init(&v);
    check(chb_square_at(&v, 0, 0) == CH_SQ(0, 7), "top-left pixel is a8");
    check(chb_square_at(&v, 159, 159) == CH_SQ(7, 0), "bottom-right pixel is h1");
    check(chb_square_at(&v, 0, 159) == CH_SQ(0, 0), "bottom-left pixel is a1");
    check(chb_square_at(&v, -1, 0) == CHB_NO_SQ, "off-board returns CHB_NO_SQ");
    v.flipped = true;
    check(chb_square_at(&v, 0, 0) == CH_SQ(7, 0), "flipped: top-left pixel is h1");

    printf("\nSAN\n");
    test_san("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "e2e4", "e4");
    test_san("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "g1f3", "Nf3");
    test_san("rnbqkbnr/ppp1pppp/8/3p4/4P3/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 2", "e4d5", "exd5");
    test_san("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", "e1g1", "O-O");
    test_san("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", "e1c1", "O-O-O");
    // Only the c3 knight reaches d5 -- f3->d5 is a (2,2) step, which is a
    // bishop move, not a knight's -- so no disambiguation is needed.
    test_san("r1bqkbnr/pppppppp/8/8/8/2N2N2/PPPPPPPP/R1BQKB1R w KQkq - 0 1", "c3d5", "Nd5");
    // Knights on b1 and f3 both reach d2: file disambiguates
    test_san("r1bqkbnr/pppppppp/8/8/8/5N2/PPP1PPPP/RNBQKB1R w KQkq - 0 1", "b1d2", "Nbd2");
    test_san("r1bqkbnr/pppppppp/8/8/8/5N2/PPP1PPPP/RNBQKB1R w KQkq - 0 1", "f3d2", "Nfd2");
    // Knights on c3 and c5 share a file, so only the rank disambiguates
    test_san("8/8/8/2N5/8/2N5/8/K6k w - - 0 1", "c3a4", "N3a4");
    test_san("8/8/8/2N5/8/2N5/8/K6k w - - 0 1", "c5a4", "N5a4");
    // Promotion. A black king on h1 sits on the a8-h1 diagonal so the new
    // queen checks it; on h2 it does not.
    test_san("8/P7/8/8/8/8/8/K6k w - - 0 1", "a7a8q", "a8=Q+");
    test_san("8/P7/8/8/8/8/7k/K7 w - - 0 1", "a7a8q", "a8=Q");
    // Underpromotion
    test_san("8/P7/8/8/8/8/7k/K7 w - - 0 1", "a7a8n", "a8=N");
    // Mate suffix
    test_san("6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1", "a1a8", "Ra8#");
    // Check suffix
    test_san("4k3/8/8/8/8/8/8/R3K3 w Q - 0 1", "a1a8", "Ra8+");

    if (failures) {
        printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall render tests passed\n");
    printf("images written to %s/board_*.ppm\n", outdir);
    return 0;
}
