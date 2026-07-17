#ifndef AMALGAMATED_BUILD
#include "chess_ui.h"
#ifdef CONFIG_CHESS_APP
#include "chess_game.h"

#include "../display.h"
#include "../gui.h"
#include "../idletimer.h"
#include "../jade_assert.h"
#include "../jade_log.h"
#include "../utils/malloc_ext.h"
#include "../utils/temporary_stack.h"

#include <esp_event.h>

#include <stdio.h>
#include <string.h>

// The board is a fixed CHB_BOARD_PX square (see the design doc: scaling it
// would mean piece art at several sizes for no real gain), so the display must
// be able to hold it plus a usable panel. Jade Plus and qemu --webdisplay-larger
// are 320x170 and libjade is 320x200; plain qemu and Jade v1.x are 240x135 and
// cannot fit it. Fail loudly here rather than silently render a clipped board.
//
// CHESS_APP has no BOARD_TYPE dependency on purpose (libjade defines no
// BOARD_TYPE_* and qemu is BOARD_TYPE_QEMU_LARGER), so this is the check.
#define CHESS_MIN_PANEL_PX 80
#if CONFIG_DISPLAY_HEIGHT < CHB_BOARD_PX
#error "CONFIG_CHESS_APP requires a display at least 160px tall. For qemu use --webdisplay-larger (320x170)."
#endif
#if CONFIG_DISPLAY_WIDTH < (CHB_BOARD_PX + CHESS_MIN_PANEL_PX)
#error "CONFIG_CHESS_APP requires a display at least 240px wide. For qemu use --webdisplay-larger (320x170)."
#endif

// Nominal search depth. Deeper plays better but thinks longer. There is no
// task watchdog on jade_v2 (CONFIG_ESP_TASK_WDT_EN=n), so an over-long search
// is a UX problem rather than a crash -- tune this on device.
#define CHESS_SEARCH_DEPTH 3

// Every negamax/quiesce frame holds a CH_MAX_MOVES array (1KB) plus locals,
// and ch_gen_legal() stacks another such array transiently on the deepest
// frame. Size from the longest possible chain rather than guessing; the search
// depth bound is why CH_QUIESCE_MAX exists.
#define CHESS_SEARCH_STACK ((CHESS_SEARCH_DEPTH + CH_QUIESCE_MAX) * 1536 + 8192)

// A player may stare at the board for minutes without pressing anything.
// Without this the device blanks mid-game.
#define CHESS_MIN_TIMEOUT_SECS 600

// Panel lines showing recent moves
#define CHESS_HIST_LINES 3

typedef struct {
    ch_pos_t pos;
    ch_move_t best;
    bool found;
} search_args_t;

// Runs on the temporary task. Must not touch the GUI: it is a different task
// from the one that owns the activity.
static bool search_impl(void* ctx)
{
    search_args_t* args = ctx;
    JADE_ASSERT(args);
    args->found = ch_search(&args->pos, CHESS_SEARCH_DEPTH, &args->best);
    return true;
}

typedef struct {
    gui_view_node_t* board;
    gui_view_node_t* status;
    gui_view_node_t* entry;
    gui_view_node_t* counter;
    gui_view_node_t* hist[CHESS_HIST_LINES];
} chess_nodes_t;

static gui_activity_t* make_chess_activity(chess_nodes_t* nodes)
{
    JADE_ASSERT(nodes);
    memset(nodes, 0, sizeof(*nodes));

    gui_activity_t* const act = gui_make_activity();

    // Absolute split: the board is a fixed 160px and must not be scaled.
    gui_view_node_t* hsplit;
    gui_make_hsplit(&hsplit, GUI_SPLIT_ABSOLUTE, 2, CHB_BOARD_PX, CONFIG_DISPLAY_WIDTH - CHB_BOARD_PX);
    gui_set_parent(hsplit, act->root_node);

    // Centred vertically because the display is 170px on Jade Plus but 200px
    // under libjade, while the board is 160px on both.
    gui_make_picture(&nodes->board, NULL);
    gui_set_align(nodes->board, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(nodes->board, hsplit);

    gui_view_node_t* vsplit;
    gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 4, 22, 22, 16, 40);
    gui_set_parent(vsplit, hsplit);

    // Every text node needs a FILL parent. gui_update_text() clears the old
    // string by repainting the node's parent, so without a background to paint
    // the previous text is never erased and successive values smear on top of
    // each other. Parenting text straight to a split looks fine until the text
    // changes. See the same pattern in ui/mnemonic.c.
    gui_view_node_t* bg;

    gui_make_fill(&bg, TFT_BLACK, FILL_PLAIN, vsplit);
    gui_make_text(&nodes->status, "", TFT_WHITE);
    gui_set_align(nodes->status, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(nodes->status, bg);

    gui_make_fill(&bg, TFT_BLACK, FILL_PLAIN, vsplit);
    gui_make_text_font(&nodes->entry, "", TFT_WHITE, GUI_TITLE_FONT);
    gui_set_align(nodes->entry, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(nodes->entry, bg);

    gui_make_fill(&bg, TFT_BLACK, FILL_PLAIN, vsplit);
    gui_make_text(&nodes->counter, "", TFT_LIGHTGREY);
    gui_set_align(nodes->counter, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(nodes->counter, bg);

    // Text nodes are single-line, so several lines need several nodes.
    gui_view_node_t* histsplit;
    gui_make_vsplit(&histsplit, GUI_SPLIT_RELATIVE, CHESS_HIST_LINES, 33, 33, 34);
    gui_set_parent(histsplit, vsplit);
    for (int i = 0; i < CHESS_HIST_LINES; ++i) {
        gui_make_fill(&bg, TFT_BLACK, FILL_PLAIN, histsplit);
        gui_make_text(&nodes->hist[i], "", TFT_LIGHTGREY);
        gui_set_align(nodes->hist[i], GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
        gui_set_parent(nodes->hist[i], bg);
    }

    return act;
}

// Pair the stored SAN strings into "N. white black" lines, newest last.
static void update_history(const chg_game_t* game, const chess_nodes_t* nodes)
{
    char pairs[CHG_HISTORY_MAX][32];
    int npairs = 0;

    uint16_t movenum = game->history_first_move;
    bool expect_black = game->history_first_black;

    for (int i = 0; i < game->history_len; ++i) {
        if (!expect_black) {
            snprintf(pairs[npairs++], sizeof(pairs[0]), "%u. %s", (unsigned)movenum, game->history[i]);
            expect_black = true;
        } else {
            if (npairs == 0) {
                // History window starts mid-pair
                snprintf(pairs[npairs++], sizeof(pairs[0]), "%u... %s", (unsigned)movenum, game->history[i]);
            } else {
                const size_t len = strlen(pairs[npairs - 1]);
                snprintf(pairs[npairs - 1] + len, sizeof(pairs[0]) - len, " %s", game->history[i]);
            }
            expect_black = false;
            ++movenum;
        }
    }

    const int first = (npairs > CHESS_HIST_LINES) ? npairs - CHESS_HIST_LINES : 0;
    for (int i = 0; i < CHESS_HIST_LINES; ++i) {
        const int idx = first + i;
        gui_update_text(nodes->hist[i], (idx < npairs) ? pairs[idx] : "");
    }
}

// `pic` must outlive every call: gui_update_picture() stores the pointer, not
// a copy (gui.c: `node->picture->picture = picture`), and the GUI task
// dereferences it later from the other core. A Picture local to this function
// would be a dangling pointer the moment it returned.
static void repaint(const chg_game_t* game, const chess_nodes_t* nodes, uint16_t* buf, const Picture* pic)
{
    chb_render(&game->view, buf);
    gui_update_picture(nodes->board, pic, false);

    gui_update_text(nodes->status, chg_status(game));

    const chg_entry_t* const cur = chg_current(game);
    gui_update_text(nodes->entry, cur ? cur->label : "");

    // Both values are bounded by CHG_RING_MAX, but the compiler cannot see
    // that through the struct and assumes the full int range, so size for the
    // worst case two ints can print ("-2147483648/-2147483648" plus a
    // terminator). Anything smaller trips -Wformat-truncation, which the IDF
    // build treats as an error.
    char counter[24] = "";
    if (game->ring_len > 0) {
        snprintf(counter, sizeof(counter), "%d/%d", game->ring_pos + 1, game->ring_len);
    }
    gui_update_text(nodes->counter, counter);

    update_history(game, nodes);
}

// Search on its own task so it gets a stack sized for the recursion without
// permanently inflating the caller's, and so the GUI task keeps painting the
// "Thinking..." indicator while it runs.
static chg_action_t engine_turn(chg_game_t* game)
{
    search_args_t args = {};
    args.pos = game->view.pos;
    args.found = false;

    if (!run_in_temporary_task(CHESS_SEARCH_STACK, search_impl, &args)) {
        JADE_LOGE("chess: failed to run search task");
        return CHG_ACT_NONE;
    }
    if (!args.found) {
        // enter_turn() only requests a search when legal moves exist, so this
        // means ring and position have desynced. Leave the game parked rather
        // than play a bogus move.
        JADE_LOGE("chess: engine found no move in a position with legal moves");
        return CHG_ACT_NONE;
    }
    return chg_engine_played(game, &args.best);
}

void chess_ui_run(void)
{
    // Capture before creating ours, so we restore whatever the caller had up.
    gui_activity_t* const prev_act = gui_current_activity();

    // 160x160 RGB565 = 50KB. PSRAM, as with every other large buffer here.
    uint16_t* const buf = JADE_MALLOC_PREFER_SPIRAM(CHB_BUF_PIXELS * sizeof(uint16_t));

    // Lives for the whole session: the GUI node holds this pointer (see
    // repaint()), so it must outlive every repaint and be cleared before it
    // goes out of scope.
    const Picture pic = {
        .data = buf,
        .width = CHB_BOARD_PX,
        .height = CHB_BOARD_PX,
        .bytes_per_pixel = 2,
    };

    chess_nodes_t nodes;
    gui_activity_t* const act = make_chess_activity(&nodes);

    chg_game_t game;
    chg_action_t action = chg_init(&game, CH_WHITE);

    idletimer_set_min_timeout_secs(CHESS_MIN_TIMEOUT_SECS);
    gui_set_current_activity(act);
    repaint(&game, &nodes, buf, &pic);

    bool running = true;
    while (running) {
        if (action == CHG_ACT_ENGINE) {
            repaint(&game, &nodes, buf, &pic); // show "Thinking..." before blocking
            const chg_state_t before = game.state;
            action = engine_turn(&game);
            if (game.state == before && before == CHG_ENGINE_THINK) {
                // The search failed and the state machine is still waiting for
                // a move it will never get. The ring is empty in this state, so
                // leaving the loop running would soft-lock the user with no way
                // out. Bail instead.
                JADE_LOGE("chess: abandoning game, engine did not move");
                break;
            }
            repaint(&game, &nodes, buf, &pic);
            continue;
        }

        // Raw prev/next/select, as main/smoketest.c does. gui_prev()/gui_next()
        // already account for display orientation before posting these, so we
        // need not. NOTE: max_wait of 0 means "wait forever" here, not "do not
        // block" -- see sync_wait_event() in utils/event.c.
        int32_t id = 0;
        if (!gui_activity_wait_event(act, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &id, NULL, 0)) {
            continue;
        }

        switch (id) {
        case GUI_WHEEL_LEFT_EVENT:
            chg_prev(&game);
            break;
        case GUI_WHEEL_RIGHT_EVENT:
            chg_next(&game);
            break;
        case GUI_FRONT_CLICK_EVENT:
        case GUI_WHEEL_CLICK_EVENT:
            action = chg_select(&game);
            if (action == CHG_ACT_EXIT) {
                running = false;
            }
            break;
        default:
            break;
        }

        if (running) {
            repaint(&game, &nodes, buf, &pic);
        }
    }

    idletimer_set_min_timeout_secs(0);
    // Clear before freeing: the node still points at `pic`, which points at
    // `buf`, and the GUI task repaints from the other core.
    gui_clear_picture(nodes.board);
    free(buf);

    if (prev_act) {
        gui_set_current_activity(prev_act);
    }
}

#endif // CONFIG_CHESS_APP
#endif // AMALGAMATED_BUILD
