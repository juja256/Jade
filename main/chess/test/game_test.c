// Host-side tests for the legal-move cycling state machine.
//
//     cc -O2 -o game_test main/chess/test/game_test.c main/chess/chess_game.c main/chess/chess_board.c main/chess/engine.c -Imain/chess
//     ./game_test

#include "chess_game.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(bool ok, const char* what)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        ++failures;
    }
}

// Cycle to the ring entry with the given label. Returns false if absent.
static bool cycle_to(chg_game_t* game, const char* label)
{
    for (int i = 0; i < game->ring_len; ++i) {
        const chg_entry_t* cur = chg_current(game);
        if (cur && !strcmp(cur->label, label)) {
            return true;
        }
        chg_next(game);
    }
    return false;
}

static void test_initial_ring(void)
{
    chg_game_t g;
    const chg_action_t act = chg_init(&g, CH_WHITE);

    check(act == CHG_ACT_NONE, "white to move: no engine action on init");
    check(g.state == CHG_PIECE_SELECT, "starts in piece select");

    // 20 legal opening moves from 10 distinct pieces (8 pawns + 2 knights),
    // plus Resign and Exit.
    check(g.ring_len == 12, "opening ring is 10 pieces + resign + exit");

    int pieces = 0, resign = 0, exit_ = 0;
    for (int i = 0; i < g.ring_len; ++i) {
        switch (g.ring[i].kind) {
        case CHG_ENTRY_PIECE:
            ++pieces;
            break;
        case CHG_ENTRY_RESIGN:
            ++resign;
            break;
        case CHG_ENTRY_EXIT:
            ++exit_;
            break;
        default:
            break;
        }
    }
    check(pieces == 10, "10 movable pieces at the start");
    check(resign == 1 && exit_ == 1, "exactly one Resign and one Exit");
}

static void test_engine_first_when_human_black(void)
{
    chg_game_t g;
    const chg_action_t act = chg_init(&g, CH_BLACK);
    check(act == CHG_ACT_ENGINE, "human black: engine moves first");
    check(g.state == CHG_ENGINE_THINK, "state is engine-think");
    check(g.view.flipped, "board flipped for a black human");
    check(g.ring_len == 0, "ring empty while engine thinks");
}

static void test_cycling_wraps(void)
{
    chg_game_t g;
    chg_init(&g, CH_WHITE);

    const int len = g.ring_len;
    check(g.ring_pos == 0, "ring starts at position 0");

    chg_prev(&g);
    check(g.ring_pos == len - 1, "prev from 0 wraps to the end");

    chg_next(&g);
    check(g.ring_pos == 0, "next wraps back to 0");

    for (int i = 0; i < len; ++i) {
        chg_next(&g);
    }
    check(g.ring_pos == 0, "a full lap returns to the start");
}

static void test_select_piece_then_dest(void)
{
    chg_game_t g;
    chg_init(&g, CH_WHITE);

    check(cycle_to(&g, "Pe2"), "e2 pawn is in the ring");
    chg_action_t act = chg_select(&g);
    check(act == CHG_ACT_NONE, "selecting a piece takes no action");
    check(g.state == CHG_DEST_SELECT, "moved to destination select");
    check(g.chosen_from == CH_SQ(4, 1), "chose the e2 pawn");
    check(g.view.sel_from == CH_SQ(4, 1), "selected piece is highlighted");

    // e2 has e3, e4, and a Back entry
    check(g.ring_len == 3, "e2 pawn has 2 destinations + Back");
    check(cycle_to(&g, "e4"), "e4 is offered");

    act = chg_select(&g);
    check(act == CHG_ACT_ENGINE, "committing a move hands over to the engine");
    check(g.state == CHG_ENGINE_THINK, "state is engine-think after moving");
    check(g.view.pos.board[CH_SQ(4, 3)] == (CH_PAWN | CH_WHITE), "pawn is on e4");
    check(g.view.pos.board[CH_SQ(4, 1)] == CH_EMPTY, "e2 is empty");
    check(g.view.last_from == CH_SQ(4, 1) && g.view.last_to == CH_SQ(4, 3), "last move highlighted");
    check(g.history_len == 1 && !strcmp(g.history[0], "e4"), "history records e4");
}

static void test_back_returns_to_piece_select(void)
{
    chg_game_t g;
    chg_init(&g, CH_WHITE);

    cycle_to(&g, "Pe2");
    chg_select(&g);
    check(g.state == CHG_DEST_SELECT, "in destination select");

    check(cycle_to(&g, "< Back"), "Back is in the destination ring");
    const chg_action_t act = chg_select(&g);
    check(act == CHG_ACT_NONE, "Back takes no action");
    check(g.state == CHG_PIECE_SELECT, "Back returns to piece select");
    check(g.chosen_from == CHB_NO_SQ, "selection cleared");
    check(g.view.sel_to == CHB_NO_SQ, "destination highlight cleared");
}

static void test_resign_and_exit(void)
{
    chg_game_t g;
    chg_init(&g, CH_WHITE);
    check(cycle_to(&g, "Resign"), "Resign is present");
    chg_select(&g);
    check(g.state == CHG_GAME_OVER, "resigning ends the game");
    check(g.resigned, "resigned flag set");
    check(!strcmp(chg_status(&g), "You resigned"), "status reports resignation");
    check(cycle_to(&g, "New game"), "game-over offers New game");
    check(cycle_to(&g, "Exit"), "game-over offers Exit");

    chg_game_t g2;
    chg_init(&g2, CH_WHITE);
    cycle_to(&g2, "Exit");
    check(chg_select(&g2) == CHG_ACT_EXIT, "Exit asks the caller to leave");
}

static void test_new_game_resets(void)
{
    chg_game_t g;
    chg_init(&g, CH_WHITE);
    cycle_to(&g, "Pe2");
    chg_select(&g);
    cycle_to(&g, "e4");
    chg_select(&g);

    // Force game-over so New game is reachable
    g.state = CHG_GAME_OVER;
    g.ring_len = 0;
    chg_init(&g, CH_WHITE); // what CHG_ENTRY_NEW does

    check(g.state == CHG_PIECE_SELECT, "new game back to piece select");
    check(g.history_len == 0, "history cleared");
    check(g.view.pos.board[CH_SQ(4, 1)] == (CH_PAWN | CH_WHITE), "e2 pawn restored");
    check(g.view.last_from == CHB_NO_SQ, "last-move highlight cleared");
}

static void test_promotion_flow(void)
{
    // White pawn on a7 ready to promote; kings far apart.
    ch_pos_t pos;
    // A black pawn on g2 keeps material sufficient, so promoting does not
    // immediately end the game (see test_knight_underpromotion_draws).
    ch_from_fen(&pos, "8/P7/8/8/8/8/6pk/K7 w - - 0 1");

    chg_game_t g;
    const chg_action_t init = chg_set_position(&g, &pos, CH_WHITE);
    check(init == CHG_ACT_NONE, "set_position: white to move, no engine action");
    check(g.state == CHG_PIECE_SELECT, "set_position starts in piece select");

    check(cycle_to(&g, "Pa7"), "the a7 pawn is offered");
    chg_select(&g);
    check(g.state == CHG_DEST_SELECT, "pawn selected -> destination select");

    // The four promotion moves share a destination, so the ring shows a8 once
    check(g.ring_len == 2, "four promotions collapse to one destination + Back");
    check(cycle_to(&g, "a8=Q"), "a8 destination offered");

    chg_action_t act = chg_select(&g);
    check(act == CHG_ACT_NONE, "promotion destination opens the promo ring");
    check(g.state == CHG_PROMO_SELECT, "state is promo select");
    check(g.ring_len == 4, "four promotion choices offered");

    check(cycle_to(&g, "Knight"), "underpromotion to knight is offered");
    act = chg_select(&g);
    check(g.view.pos.board[CH_SQ(0, 7)] == (CH_KNIGHT | CH_WHITE), "promoted to a knight, not a queen");
    check(act == CHG_ACT_ENGINE, "promotion hands over to the engine");
}

// Underpromoting to a knight against a bare king leaves K+N vs K, which cannot
// force mate -- so the game ends in a draw rather than continuing. Worth
// pinning: it is easy to "fix" insufficient-material detection and silently
// break this.
static void test_knight_underpromotion_draws(void)
{
    ch_pos_t pos;
    ch_from_fen(&pos, "8/P7/8/8/8/8/7k/K7 w - - 0 1");

    chg_game_t g;
    chg_set_position(&g, &pos, CH_WHITE);
    cycle_to(&g, "Pa7");
    chg_select(&g);
    cycle_to(&g, "a8=Q");
    chg_select(&g);
    check(cycle_to(&g, "Knight"), "knight underpromotion offered vs bare king");

    const chg_action_t act = chg_select(&g);
    check(act == CHG_ACT_NONE, "K+N vs K: no engine turn, the game is over");
    check(g.state == CHG_GAME_OVER, "K+N vs K ends the game");
    check(g.result == CH_DRAW_MATERIAL, "classified as insufficient material");
    check(!strcmp(chg_status(&g), "Draw: pieces"), "status reports a material draw");
}

static void test_promotion_defaults_to_queen(void)
{
    ch_pos_t pos;
    // A black pawn on g2 keeps material sufficient, so promoting does not
    // immediately end the game (see test_knight_underpromotion_draws).
    ch_from_fen(&pos, "8/P7/8/8/8/8/6pk/K7 w - - 0 1");

    chg_game_t g;
    chg_set_position(&g, &pos, CH_WHITE);
    cycle_to(&g, "Pa7");
    chg_select(&g);
    cycle_to(&g, "a8=Q");
    chg_select(&g);
    // Queen is first in the promo ring, so a player who just clicks through
    // gets the piece they almost always want.
    check(g.ring_pos == 0 && g.ring[0].promo == CH_QUEEN, "Queen is the first promo choice");
    chg_select(&g);
    check(g.view.pos.board[CH_SQ(0, 7)] == (CH_QUEEN | CH_WHITE), "clicking straight through promotes to queen");
}

// Drive a whole game through the public API only, the way chess_ui.c will.
static void test_full_game_via_api(void)
{
    chg_game_t g;
    chg_action_t act = chg_init(&g, CH_WHITE);

    int moves_played = 0;
    for (int guard = 0; guard < 400 && g.state != CHG_GAME_OVER; ++guard) {
        if (act == CHG_ACT_ENGINE) {
            ch_move_t best;
            if (!ch_search(&g.view.pos, 2, &best)) {
                break;
            }
            chg_engine_played(&g, &best);
            act = (g.state == CHG_ENGINE_THINK) ? CHG_ACT_ENGINE : CHG_ACT_NONE;
            ++moves_played;
            continue;
        }

        if (g.state == CHG_PIECE_SELECT) {
            // Always take the first piece with a legal move
            if (g.ring[0].kind != CHG_ENTRY_PIECE) {
                break;
            }
            g.ring_pos = 0;
            act = chg_select(&g);
            continue;
        }
        if (g.state == CHG_DEST_SELECT) {
            g.ring_pos = 0;
            if (g.ring[0].kind != CHG_ENTRY_DEST) {
                break;
            }
            act = chg_select(&g);
            ++moves_played;
            continue;
        }
        if (g.state == CHG_PROMO_SELECT) {
            g.ring_pos = 0;
            act = chg_select(&g);
            ++moves_played;
            continue;
        }
        break;
    }

    char what[96];
    snprintf(what, sizeof(what), "full game via public API: %d moves, ended %s", moves_played,
        g.state == CHG_GAME_OVER ? chg_status(&g) : "(guard hit)");
    check(g.state == CHG_GAME_OVER, what);
}

static void test_status_strings(void)
{
    chg_game_t g;
    chg_init(&g, CH_WHITE);
    check(!strcmp(chg_status(&g), "Your move"), "status: Your move");

    cycle_to(&g, "Pe2");
    chg_select(&g);
    check(!strcmp(chg_status(&g), "Move to..."), "status: Move to...");

    chg_game_t b;
    chg_init(&b, CH_BLACK);
    check(!strcmp(chg_status(&b), "Thinking..."), "status: Thinking...");
}

// The side panel is 160px, which fits ~13 characters in the default font, and
// the text node truncates silently: "Checkmate - you lose" once rendered on
// device as "Checkmate - yo". Pin every reachable status string's length.
#define CHG_STATUS_MAX_CHARS 13
static void test_status_strings_fit_panel(void)
{
    // Drive each terminal state and check what chg_status() would show.
    struct {
        const char* fen;
        bool resign;
        const char* what;
    } cases[] = {
        { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", true, "resigned" },
        { "R5k1/5ppp/8/8/8/8/5PPP/6K1 b - - 0 1", false, "checkmate (human mated)" },
        { "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", false, "stalemate" },
        { "8/8/4k3/8/8/4KB2/8/8 w - - 0 1", false, "insufficient material" },
        { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", false, "your move" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        ch_pos_t pos;
        if (!ch_from_fen(&pos, cases[i].fen)) {
            check(false, cases[i].what);
            continue;
        }
        chg_game_t g;
        chg_set_position(&g, &pos, CH_WHITE);
        if (cases[i].resign) {
            cycle_to(&g, "Resign");
            chg_select(&g);
        }
        const char* s = chg_status(&g);
        char what[96];
        snprintf(what, sizeof(what), "status fits panel: \"%s\" (%zu chars)", s, strlen(s));
        check(strlen(s) <= CHG_STATUS_MAX_CHARS, what);
    }

    // The in-play strings too
    static const char* const inplay[] = { "Your move", "Move to...", "Promote to...", "Thinking...", "Check!" };
    for (size_t i = 0; i < sizeof(inplay) / sizeof(inplay[0]); ++i) {
        char what[96];
        snprintf(what, sizeof(what), "status fits panel: \"%s\" (%zu chars)", inplay[i], strlen(inplay[i]));
        check(strlen(inplay[i]) <= CHG_STATUS_MAX_CHARS, what);
    }
}

static void test_ring_never_overflows(void)
{
    // A queen on an open board has 27 moves; the destination ring must hold
    // them plus Back without exceeding CHG_RING_MAX.
    ch_pos_t pos;
    ch_from_fen(&pos, "7k/8/8/3Q4/8/8/8/K7 w - - 0 1");
    ch_move_t moves[CH_MAX_MOVES];
    const int n = ch_gen_legal(&pos, moves);

    int queen_moves = 0;
    for (int i = 0; i < n; ++i) {
        if (pos.board[moves[i].from] == (CH_QUEEN | CH_WHITE)) {
            ++queen_moves;
        }
    }
    char what[80];
    snprintf(what, sizeof(what), "open queen has %d moves, ring bound is %d", queen_moves, CHG_RING_MAX);
    check(queen_moves + 1 <= CHG_RING_MAX, what);
}

static void test_level_params(void) {
    struct { uint8_t lv; int depth; int margin; } cases[] = {
        {1,3,40},{2,4,0},{3,5,0},{4,6,0},{5,7,0}
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        int d = -1, m = -1;
        chg_level_params(cases[i].lv, &d, &m);
        char what[64]; snprintf(what, sizeof(what), "Lv%u -> depth %d margin %d", cases[i].lv, cases[i].depth, cases[i].margin);
        check(d == cases[i].depth && m == cases[i].margin, what);
    }
    int d = 0, m = 0;
    chg_level_params(0, &d, &m);   check(d == 4 && m == 0, "level 0 clamps to Lv2");
    chg_level_params(99, &d, &m);  check(d == 4 && m == 0, "level 99 clamps to Lv2");
}

static void test_level_labels_fit_panel(void) {
    for (uint8_t lv = 1; lv <= CHG_NUM_LEVELS; ++lv) {
        check(strlen(chg_level_label(lv)) <= 13, "level label fits panel");
        check(strlen(chg_level_short(lv)) <= 13, "short level label fits panel");
    }
}

static void test_init_ex_records_level(void) {
    chg_game_t g;
    chg_init_ex(&g, CH_WHITE, 4);
    check(g.level == 4, "chg_init_ex records the level");
    chg_init(&g, CH_WHITE);
    check(g.level == 2, "chg_init defaults to Lv2");
}

int main(void)
{
    printf("\ninitial state\n");
    test_initial_ring();
    test_engine_first_when_human_black();

    printf("\ncycling\n");
    test_cycling_wraps();

    printf("\nselection flow\n");
    test_select_piece_then_dest();
    test_back_returns_to_piece_select();

    printf("\nresign / exit / new game\n");
    test_resign_and_exit();
    test_new_game_resets();

    printf("\npromotion\n");
    test_promotion_flow();
    test_promotion_defaults_to_queen();
    test_knight_underpromotion_draws();

    printf("\nstatus\n");
    test_status_strings();

    printf("\nbounds\n");
    test_ring_never_overflows();
    test_status_strings_fit_panel();

    printf("\nlevels\n");
    test_level_params();
    test_level_labels_fit_panel();
    test_init_ex_records_level();

    printf("\nfull game\n");
    test_full_game_via_api();

    if (failures) {
        printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall game tests passed\n");
    return 0;
}
