#ifndef AMALGAMATED_BUILD
#include "chess_game.h"

#include <stdio.h>
#include <string.h>

// snprintf rather than strncpy: it always terminates, and labels are short
// enough that silent truncation would be a bug worth not hiding.
static void set_label(chg_entry_t* entry, const char* text)
{
    snprintf(entry->label, sizeof(entry->label), "%s", text);
}

static void add_entry(chg_game_t* game, chg_entry_kind_t kind, uint8_t sq, uint8_t promo, const char* label)
{
    if (game->ring_len >= CHG_RING_MAX) {
        return; // bounded by construction; drop rather than overrun
    }
    chg_entry_t* entry = &game->ring[game->ring_len++];
    entry->kind = kind;
    entry->sq = sq;
    entry->promo = promo;
    set_label(entry, label);
}

// Highlights follow the ring so the board always shows what is selected.
static void sync_highlights(chg_game_t* game)
{
    const chg_entry_t* cur = chg_current(game);

    switch (game->state) {
    case CHG_PIECE_SELECT:
        // Preview the piece under the cursor
        game->view.sel_from = (cur && cur->kind == CHG_ENTRY_PIECE) ? cur->sq : CHB_NO_SQ;
        game->view.sel_to = CHB_NO_SQ;
        break;
    case CHG_DEST_SELECT:
        game->view.sel_from = game->chosen_from;
        game->view.sel_to = (cur && cur->kind == CHG_ENTRY_DEST) ? cur->sq : CHB_NO_SQ;
        break;
    case CHG_PROMO_SELECT:
        game->view.sel_from = game->chosen_from;
        game->view.sel_to = game->chosen_to;
        break;
    default:
        game->view.sel_from = CHB_NO_SQ;
        game->view.sel_to = CHB_NO_SQ;
        break;
    }
}

static void push_history(chg_game_t* game, const char* san, uint16_t fullmove, bool black)
{
    if (game->history_len < CHG_HISTORY_MAX) {
        if (game->history_len == 0) {
            game->history_first_move = fullmove;
            game->history_first_black = black;
        }
        snprintf(game->history[game->history_len], CHG_LABEL_LEN, "%s", san);
        ++game->history_len;
        return;
    }

    // Full: drop the oldest and track what the new oldest represents
    memmove(&game->history[0], &game->history[1], sizeof(game->history[0]) * (CHG_HISTORY_MAX - 1));
    snprintf(game->history[CHG_HISTORY_MAX - 1], CHG_LABEL_LEN, "%s", san);
    if (game->history_first_black) {
        ++game->history_first_move;
        game->history_first_black = false;
    } else {
        game->history_first_black = true;
    }
}

static void build_piece_ring(chg_game_t* game)
{
    game->ring_len = 0;
    game->ring_pos = 0;

    ch_move_t moves[CH_MAX_MOVES];
    const int n = ch_gen_legal(&game->view.pos, moves);

    // Unique `from` squares. gen_pseudo scans squares in ascending order, so
    // these come out sorted already, which keeps cycling predictable.
    for (int i = 0; i < n; ++i) {
        bool seen = false;
        for (int j = 0; j < game->ring_len; ++j) {
            if (game->ring[j].kind == CHG_ENTRY_PIECE && game->ring[j].sq == moves[i].from) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }

        const uint8_t pc = game->view.pos.board[moves[i].from];
        static const char letters[7] = { '?', 'P', 'N', 'B', 'R', 'Q', 'K' };
        char label[CHG_LABEL_LEN];
        snprintf(label, sizeof(label), "%c%c%c", letters[CH_TYPE(pc)], (char)('a' + CH_FILE(moves[i].from)),
            (char)('1' + CH_RANK(moves[i].from)));
        add_entry(game, CHG_ENTRY_PIECE, moves[i].from, 0, label);
    }

    add_entry(game, CHG_ENTRY_RESIGN, CHB_NO_SQ, 0, "Resign");
    add_entry(game, CHG_ENTRY_EXIT, CHB_NO_SQ, 0, "Exit");
}

static void build_dest_ring(chg_game_t* game)
{
    game->ring_len = 0;
    game->ring_pos = 0;

    ch_move_t moves[CH_MAX_MOVES];
    const int n = ch_gen_legal(&game->view.pos, moves);

    for (int i = 0; i < n; ++i) {
        if (moves[i].from != game->chosen_from) {
            continue;
        }
        // Collapse the four promotion moves to one destination; the piece is
        // chosen afterwards in CHG_PROMO_SELECT.
        bool seen = false;
        for (int j = 0; j < game->ring_len; ++j) {
            if (game->ring[j].kind == CHG_ENTRY_DEST && game->ring[j].sq == moves[i].to) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }

        char san[CHB_SAN_LEN];
        chb_move_san(&game->view.pos, &moves[i], san, sizeof(san));
        add_entry(game, CHG_ENTRY_DEST, moves[i].to, 0, san);
    }

    add_entry(game, CHG_ENTRY_BACK, CHB_NO_SQ, 0, "< Back");
}

static void build_promo_ring(chg_game_t* game)
{
    game->ring_len = 0;
    game->ring_pos = 0;
    add_entry(game, CHG_ENTRY_PROMO, game->chosen_to, CH_QUEEN, "Queen");
    add_entry(game, CHG_ENTRY_PROMO, game->chosen_to, CH_ROOK, "Rook");
    add_entry(game, CHG_ENTRY_PROMO, game->chosen_to, CH_BISHOP, "Bishop");
    add_entry(game, CHG_ENTRY_PROMO, game->chosen_to, CH_KNIGHT, "Knight");
}

static void build_over_ring(chg_game_t* game)
{
    game->ring_len = 0;
    game->ring_pos = 0;
    add_entry(game, CHG_ENTRY_NEW, CHB_NO_SQ, 0, "New game");
    add_entry(game, CHG_ENTRY_EXIT, CHB_NO_SQ, 0, "Exit");
}

// Move to whichever state follows the position, and rebuild the ring for it.
static chg_action_t enter_turn(chg_game_t* game)
{
    game->result = ch_result(&game->view.pos);
    if (game->result != CH_ONGOING) {
        game->state = CHG_GAME_OVER;
        build_over_ring(game);
        sync_highlights(game);
        return CHG_ACT_NONE;
    }

    if (game->view.pos.side == game->human) {
        game->state = CHG_PIECE_SELECT;
        build_piece_ring(game);
        sync_highlights(game);
        return CHG_ACT_NONE;
    }

    game->state = CHG_ENGINE_THINK;
    game->ring_len = 0;
    game->ring_pos = 0;
    sync_highlights(game);
    return CHG_ACT_ENGINE;
}

// Play `move`, recording SAN and last-move highlight.
static void apply(chg_game_t* game, const ch_move_t* move)
{
    char san[CHB_SAN_LEN];
    chb_move_san(&game->view.pos, move, san, sizeof(san));
    const uint16_t fullmove = game->view.pos.fullmove;
    const bool black = game->view.pos.side == CH_BLACK;

    ch_undo_t undo;
    ch_make(&game->view.pos, move, &undo);

    game->view.last_from = move->from;
    game->view.last_to = move->to;
    push_history(game, san, fullmove, black);
}

chg_action_t chg_set_position(chg_game_t* game, const ch_pos_t* pos, uint8_t human_colour)
{
    memset(game, 0, sizeof(*game));
    chb_view_init(&game->view);
    game->view.pos = *pos;
    game->human = human_colour;
    game->result = CH_ONGOING;
    game->resigned = false;
    game->chosen_from = CHB_NO_SQ;
    game->chosen_to = CHB_NO_SQ;
    // Black at the bottom when the human plays black
    game->view.flipped = (human_colour == CH_BLACK);
    return enter_turn(game);
}

chg_action_t chg_init(chg_game_t* game, uint8_t human_colour)
{
    ch_pos_t start;
    ch_init(&start);
    return chg_set_position(game, &start, human_colour);
}

void chg_prev(chg_game_t* game)
{
    if (game->ring_len <= 0) {
        return;
    }
    game->ring_pos = (game->ring_pos + game->ring_len - 1) % game->ring_len;
    sync_highlights(game);
}

void chg_next(chg_game_t* game)
{
    if (game->ring_len <= 0) {
        return;
    }
    game->ring_pos = (game->ring_pos + 1) % game->ring_len;
    sync_highlights(game);
}

const chg_entry_t* chg_current(const chg_game_t* game)
{
    if (game->ring_len <= 0 || game->ring_pos < 0 || game->ring_pos >= game->ring_len) {
        return NULL;
    }
    return &game->ring[game->ring_pos];
}

// Find the legal move matching from/to (and promo, when promoting).
static bool find_move(const ch_pos_t* pos, uint8_t from, uint8_t to, uint8_t promo, ch_move_t* out)
{
    ch_move_t moves[CH_MAX_MOVES];
    const int n = ch_gen_legal(pos, moves);
    for (int i = 0; i < n; ++i) {
        if (moves[i].from != from || moves[i].to != to) {
            continue;
        }
        if ((moves[i].flags & CH_MF_PROMO) && promo && moves[i].promo != promo) {
            continue;
        }
        *out = moves[i];
        return true;
    }
    return false;
}

static bool dest_is_promotion(const ch_pos_t* pos, uint8_t from, uint8_t to)
{
    ch_move_t move;
    return find_move(pos, from, to, 0, &move) && (move.flags & CH_MF_PROMO);
}

chg_action_t chg_select(chg_game_t* game)
{
    const chg_entry_t* cur = chg_current(game);
    if (!cur) {
        return CHG_ACT_NONE;
    }

    switch (cur->kind) {
    case CHG_ENTRY_EXIT:
        return CHG_ACT_EXIT;

    case CHG_ENTRY_RESIGN:
        game->resigned = true;
        game->state = CHG_GAME_OVER;
        build_over_ring(game);
        sync_highlights(game);
        return CHG_ACT_NONE;

    case CHG_ENTRY_NEW:
        return chg_init(game, game->human);

    case CHG_ENTRY_BACK:
        game->chosen_from = CHB_NO_SQ;
        game->state = CHG_PIECE_SELECT;
        build_piece_ring(game);
        sync_highlights(game);
        return CHG_ACT_NONE;

    case CHG_ENTRY_PIECE:
        game->chosen_from = cur->sq;
        game->state = CHG_DEST_SELECT;
        build_dest_ring(game);
        sync_highlights(game);
        return CHG_ACT_NONE;

    case CHG_ENTRY_DEST: {
        game->chosen_to = cur->sq;
        if (dest_is_promotion(&game->view.pos, game->chosen_from, game->chosen_to)) {
            game->state = CHG_PROMO_SELECT;
            build_promo_ring(game);
            sync_highlights(game);
            return CHG_ACT_NONE;
        }
        ch_move_t move;
        if (!find_move(&game->view.pos, game->chosen_from, game->chosen_to, 0, &move)) {
            // The ring is built from legal moves, so this cannot happen unless
            // ring and position have desynced. Refuse rather than corrupt.
            game->state = CHG_PIECE_SELECT;
            build_piece_ring(game);
            sync_highlights(game);
            return CHG_ACT_NONE;
        }
        apply(game, &move);
        return enter_turn(game);
    }

    case CHG_ENTRY_PROMO: {
        ch_move_t move;
        if (!find_move(&game->view.pos, game->chosen_from, game->chosen_to, cur->promo, &move)) {
            game->state = CHG_PIECE_SELECT;
            build_piece_ring(game);
            sync_highlights(game);
            return CHG_ACT_NONE;
        }
        apply(game, &move);
        return enter_turn(game);
    }
    }

    return CHG_ACT_NONE;
}

chg_action_t chg_engine_played(chg_game_t* game, const ch_move_t* move)
{
    if (game->state != CHG_ENGINE_THINK) {
        return CHG_ACT_NONE;
    }
    apply(game, move);
    // In practice this is never CHG_ACT_ENGINE again -- the engine plays one
    // colour -- but returning it keeps the caller from having to know that.
    return enter_turn(game);
}

const char* chg_status(const chg_game_t* game)
{
    if (game->state == CHG_GAME_OVER) {
        if (game->resigned) {
            return "You resigned";
        }
        switch (game->result) {
        case CH_CHECKMATE:
            // The side to move is the one mated
            return (game->view.pos.side == game->human) ? "Checkmate - you lose" : "Checkmate - you win";
        case CH_STALEMATE:
            return "Stalemate";
        case CH_DRAW_FIFTY:
            return "Draw: fifty moves";
        case CH_DRAW_MATERIAL:
            return "Draw: material";
        default:
            return "Game over";
        }
    }

    if (game->state == CHG_ENGINE_THINK) {
        return "Thinking...";
    }
    if (ch_in_check(&game->view.pos, game->view.pos.side)) {
        return "Check!";
    }
    switch (game->state) {
    case CHG_DEST_SELECT:
        return "Move to...";
    case CHG_PROMO_SELECT:
        return "Promote to...";
    default:
        return "Your move";
    }
}

#endif // AMALGAMATED_BUILD
