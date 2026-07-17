# Chess Difficulty, Setup Menu & Stronger Engine — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add difficulty levels, a pre-game colour/level setup menu, and a level indicator to the Jade chess app, and strengthen the engine with a transposition table and iterative deepening so it can afford depth 5–7.

**Architecture:** Engine changes are pure C in `main/chess/engine.c` (Zobrist hash carried in the position, a caller-buffered transposition table, iterative deepening, a `ch_search_ex` that adds a randomness margin). Difficulty mapping lives in the host-testable `chess_game.c`. The setup menu, level indicator, TT allocation and RNG seeding are firmware glue in `chess_ui.c`, verified by driving libjade.

**Tech Stack:** C (gnu17), ESP-IDF v5.5.4, libjade (native firmware build), host `cc` for the test suites.

## Global Constraints

- **Amalgamated build:** every `.c` under `main/` is wrapped in `#ifndef AMALGAMATED_BUILD … #endif`; new files must be added to `main/amalgamated.c` inside `#ifdef CONFIG_CHESS_APP`. New `main/chess/*.c` are covered by the existing `${chessdir}` in `main/CMakeLists.txt`.
- **Engine and game units stay libc-only:** `engine.c`, `chess_board.c`, `chess_game.c` must not include firmware headers, so the host suites compile them directly. `chess_ui.c` is the only unit that touches ESP-IDF.
- **IDF builds `-Werror=all`,** including `-Wformat-truncation` under `-Os`. `main/chess/test/run_tests.sh` compiles every chess source with those exact flags — it must stay green.
- **Panel is 160px wide (~13 chars).** Any new on-screen string must fit; `test_status_strings_fit_panel` in `game_test.c` is the guard for status strings.
- **Byte-swapped RGB565** on the panel — already handled by the `RGB()` macro in `chess_board.c`; no new colour work here.
- **No `storage.c` changes.** Settings are session-only, held in a static in `chess_ui.c`.
- **TT buffer must be PSRAM** on device — the engine never calls `malloc`; the caller provides the buffer (`ch_tt_init`), so `chess_ui.c` uses `JADE_MALLOC_PREFER_SPIRAM` and host tests use `malloc`.
- **Default level is Lv2 (depth 4); levels map depth 3/4/5/6/7; Lv1 has a 40 cp randomness margin.**
- **Reproducible builds are already broken by `--chess`** (documented); no new concern.
- `clang-format-19` is not available in this environment; do not attempt to run `format.sh`. Match surrounding style by hand.

---

## File Structure

- `main/chess/engine.h` / `engine.c` — add `uint64_t hash` to `ch_pos_t`/`ch_undo_t`, Zobrist, `ch_tt_t` + TT ops, iterative deepening, `ch_search_ex`.
- `main/chess/chess_game.h` / `chess_game.c` — `uint8_t level` in `chg_game_t`, `chg_init_ex`, `chg_level_params`, `chg_level_label`.
- `main/chess/chess_ui.c` — setup menu, colour resolution, level indicator, TT alloc/free, RNG seeding, `engine_turn` using `ch_search_ex`.
- `main/chess/test/engine_tt_test.c` — new host test for Zobrist + TT + search invariants.
- `main/chess/test/game_test.c` — extend with level-mapping tests.
- `main/chess/test/run_tests.sh` — add the new test binary.
- `main/button_events.h` — new button ids for the setup menu.

---

## Task 1: Zobrist hash carried in the position

**Files:**
- Modify: `main/chess/engine.h` (add `hash` to `ch_pos_t` and `ch_undo_t`; declare nothing new public)
- Modify: `main/chess/engine.c` (Zobrist table, `ch_make`/`ch_unmake`/`ch_init`/`ch_from_fen`)
- Create: `main/chess/test/engine_tt_test.c` (hash tests)

**Interfaces:**
- Consumes: existing `ch_pos_t`, `ch_make`, `ch_unmake`, `ch_gen_legal`, `ch_from_fen`, `CH_ONBOARD`, `CH_RANK`, `CH_FILE`, `CH_TYPE`, `CH_COLOUR`, `CH_SQ`.
- Produces: `pos->hash` is a correct Zobrist key, updated incrementally by `ch_make`, restored by `ch_unmake`, and equal to a from-scratch recompute after any sequence of moves. A new internal `uint64_t ch_zobrist(const ch_pos_t*)` (declared in `engine.h` for tests).

- [ ] **Step 1: Add the hash fields and the recompute declaration**

In `engine.h`, add `uint64_t hash;` as the last field of `ch_pos_t`:

```c
typedef struct {
    uint8_t board[128];
    uint8_t side;
    uint8_t castle;
    int8_t ep;
    uint16_t halfmove;
    uint16_t fullmove;
    uint8_t king_sq[2];
    uint64_t hash; // Zobrist key; maintained by ch_make/ch_unmake
} ch_pos_t;
```

Add `uint64_t hash;` as the last field of `ch_undo_t`:

```c
typedef struct {
    ch_move_t move;
    uint8_t captured;
    uint8_t castle;
    int8_t ep;
    uint16_t halfmove;
    uint16_t fullmove;
    uint64_t hash; // position hash before the move
} ch_undo_t;
```

Add near the other prototypes in `engine.h`:

```c
// Compute the Zobrist key from scratch. Normally you use pos->hash, which is
// kept up to date; this exists for initialisation and for tests.
uint64_t ch_zobrist(const ch_pos_t* pos);
```

- [ ] **Step 2: Write the failing test**

Create `main/chess/test/engine_tt_test.c`:

```c
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

int main(void) {
    printf("\nzobrist\n");
    test_hash_matches_recompute_after_moves();
    test_transposition_equal();
    if (failures) { printf("\n%d test(s) FAILED\n", failures); return 1; }
    printf("\nall engine_tt tests passed\n");
    return 0;
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cc -O2 -o /tmp/ett main/chess/test/engine_tt_test.c main/chess/engine.c -Imain/chess && /tmp/ett`
Expected: link error — `ch_zobrist` undefined (not yet implemented).

- [ ] **Step 4: Implement Zobrist in engine.c**

At the top of `engine.c` (after the includes, inside the `#ifndef AMALGAMATED_BUILD` guard), add the table and helpers:

```c
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
```

In `ch_init` and `ch_from_fen`, add `pos->hash = ch_zobrist(pos);` as the **last** statement before `return`. (In `ch_init` there is no return; add it at the end of the function.)

Now the incremental update. Replace the body of `ch_make` with the version below (identical to the current one plus the interleaved `h ^= …` lines and the `undo->hash` save):

```c
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
```

In `ch_unmake`, add `pos->hash = undo->hash;` as the first statement (before it starts restoring the board):

```c
void ch_unmake(ch_pos_t* pos, const ch_undo_t* undo)
{
    pos->hash = undo->hash;
    // ... existing body unchanged ...
```

- [ ] **Step 5: Run the engine_tt test to verify it passes**

Run: `cc -O2 -o /tmp/ett main/chess/test/engine_tt_test.c main/chess/engine.c -Imain/chess && /tmp/ett`
Expected: PASS — `all engine_tt tests passed`.

- [ ] **Step 6: Run perft to prove make/unmake is still correct**

Run: `cc -O2 -o /tmp/perft main/chess/test/perft_test.c main/chess/engine.c -Imain/chess && /tmp/perft`
Expected: `all 26 perft cases passed`. This is the load-bearing check that the hash-in-state change did not break make/unmake.

- [ ] **Step 7: Commit**

```bash
git add main/chess/engine.h main/chess/engine.c main/chess/test/engine_tt_test.c
git commit -m "chess: carry a Zobrist hash in the position (engine, foundation for TT)"
```

---

## Task 2: Transposition table data structure

**Files:**
- Modify: `main/chess/engine.h` (types + TT ops)
- Modify: `main/chess/engine.c` (TT ops)
- Modify: `main/chess/test/engine_tt_test.c` (TT unit tests)

**Interfaces:**
- Consumes: `pos->hash` from Task 1, `ch_move_t`.
- Produces:
  - `typedef struct { uint64_t key; ch_move_t move; int16_t score; uint8_t depth; uint8_t flag; } ch_tt_entry_t;`
  - `typedef struct { ch_tt_entry_t* entries; size_t count; } ch_tt_t;`
  - `#define CH_TT_NONE 0`, `CH_TT_EXACT 1`, `CH_TT_LOWER 2`, `CH_TT_UPPER 3`
  - `size_t ch_tt_sizeof(size_t count);` — bytes needed for `count` entries
  - `void ch_tt_init(ch_tt_t* tt, void* buffer, size_t count);` — buffer ≥ `ch_tt_sizeof(count)`, cleared
  - `void ch_tt_clear(ch_tt_t* tt);`
  - `bool ch_tt_probe(const ch_tt_t* tt, uint64_t key, ch_tt_entry_t* out);` — true if an entry with this key is present; fills `*out`
  - `void ch_tt_store(ch_tt_t* tt, uint64_t key, int depth, int score, uint8_t flag, ch_move_t move);`

- [ ] **Step 1: Declare the types and ops in engine.h**

Add above the search prototypes:

```c
#define CH_TT_NONE 0
#define CH_TT_EXACT 1
#define CH_TT_LOWER 2
#define CH_TT_UPPER 3

typedef struct {
    uint64_t key;
    ch_move_t move;
    int16_t score;
    uint8_t depth;
    uint8_t flag; // CH_TT_*
} ch_tt_entry_t;

typedef struct {
    ch_tt_entry_t* entries;
    size_t count;
} ch_tt_t;

// The engine never allocates; the caller provides a buffer of at least
// ch_tt_sizeof(count) bytes (PSRAM on device). ch_tt_init clears it.
size_t ch_tt_sizeof(size_t count);
void ch_tt_init(ch_tt_t* tt, void* buffer, size_t count);
void ch_tt_clear(ch_tt_t* tt);
bool ch_tt_probe(const ch_tt_t* tt, uint64_t key, ch_tt_entry_t* out);
void ch_tt_store(ch_tt_t* tt, uint64_t key, int depth, int score, uint8_t flag, ch_move_t move);
```

- [ ] **Step 2: Write the failing TT test**

Add to `engine_tt_test.c` (and call from `main` under a new `printf("\ntt\n")` section):

```c
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
```

Add `#include <stdlib.h>` if not present (it is, from Task 1).

- [ ] **Step 3: Run to verify it fails**

Run: `cc -O2 -o /tmp/ett main/chess/test/engine_tt_test.c main/chess/engine.c -Imain/chess && /tmp/ett`
Expected: link error — `ch_tt_sizeof` etc. undefined.

- [ ] **Step 4: Implement the TT in engine.c**

```c
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
```

- [ ] **Step 5: Run to verify it passes**

Run: `cc -O2 -o /tmp/ett main/chess/test/engine_tt_test.c main/chess/engine.c -Imain/chess && /tmp/ett`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add main/chess/engine.h main/chess/engine.c main/chess/test/engine_tt_test.c
git commit -m "chess: add caller-buffered transposition table (data structure)"
```

---

## Task 3: Iterative deepening, TT-backed search, and randomness margin

**Files:**
- Modify: `main/chess/engine.h` (`ch_search_ex`, `ch_search_bestscore`)
- Modify: `main/chess/engine.c` (thread TT through negamax; ID; margin/rng at root)
- Modify: `main/chess/test/engine_tt_test.c` (invariance + randomness tests)

**Interfaces:**
- Consumes: TT ops (Task 2), `pos->hash` (Task 1), existing `negamax`, `quiesce`, `sort_moves`, `evaluate`, `ch_gen_legal`.
- Produces:
  - `bool ch_search_ex(ch_pos_t* pos, int depth, int margin, uint32_t* rng_state, ch_tt_t* tt, ch_move_t* best);`
    Iterative-deepening search to `depth`. Among root moves within `margin` centipawns of the best, picks one at random using `rng_state` (xorshift32); `margin == 0` or `rng_state == NULL` ⇒ strict best move. `tt` may be NULL.
  - `int ch_search_bestscore(ch_pos_t* pos, int depth, ch_tt_t* tt);` — the best root score at `depth` (deterministic, no randomness). For tests and for the margin logic.
  - `ch_search(pos, depth, best)` becomes `ch_search_ex(pos, depth, 0, NULL, NULL, best)`.

- [ ] **Step 1: Declare the new entry points in engine.h**

Replace the existing `ch_search` prototype block with:

```c
// Best root score at the given depth (iterative deepening). Deterministic.
int ch_search_bestscore(ch_pos_t* pos, int depth, ch_tt_t* tt);

// Search for a move. Among root moves scoring within `margin` centipawns of the
// best, picks one at random via `rng_state` (xorshift32 state, may be NULL);
// margin 0 or rng NULL gives the strict best move. `tt` may be NULL.
// Returns false when there is no legal move.
bool ch_search_ex(ch_pos_t* pos, int depth, int margin, uint32_t* rng_state, ch_tt_t* tt, ch_move_t* best);

// Strict best-move search (margin 0, no randomness, no table).
bool ch_search(ch_pos_t* pos, int depth, ch_move_t* best);
```

- [ ] **Step 2: Write the failing invariance + randomness tests**

Add to `engine_tt_test.c` (new `printf("\nsearch\n")` section in `main`):

```c
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
```

- [ ] **Step 3: Run to verify it fails**

Run: `cc -O2 -o /tmp/ett main/chess/test/engine_tt_test.c main/chess/engine.c -Imain/chess && /tmp/ett`
Expected: link error — `ch_search_ex`/`ch_search_bestscore` undefined.

- [ ] **Step 4: Thread the TT through negamax**

Change `negamax`'s signature to take a `ch_tt_t* tt` and use it. Replace the current `negamax` with:

```c
static int negamax(ch_pos_t* pos, int depth, int alpha, int beta, int ply, ch_tt_t* tt)
{
    const int alpha_orig = alpha;

    ch_tt_entry_t hit;
    ch_move_t tt_move = { 0, 0, 0, 0 };
    if (tt && ch_tt_probe(tt, pos->hash, &hit)) {
        tt_move = hit.move;
        if (hit.depth >= depth) {
            if (hit.flag == CH_TT_EXACT) return hit.score;
            if (hit.flag == CH_TT_LOWER && hit.score > alpha) alpha = hit.score;
            else if (hit.flag == CH_TT_UPPER && hit.score < beta) beta = hit.score;
            if (alpha >= beta) return hit.score;
        }
    }

    if (depth <= 0) {
        return quiesce(pos, alpha, beta, CH_QUIESCE_MAX);
    }

    ch_move_t moves[CH_MAX_MOVES];
    const int n = ch_gen_legal(pos, moves);
    if (n == 0) {
        return ch_in_check(pos, pos->side) ? -CH_MATE + ply : 0;
    }
    if (pos->halfmove >= 100) {
        return 0;
    }
    sort_moves(pos, moves, n);

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
        if (alpha >= beta) break;
    }

    if (tt) {
        const uint8_t flag = (best <= alpha_orig) ? CH_TT_UPPER : (best >= beta) ? CH_TT_LOWER : CH_TT_EXACT;
        ch_tt_store(tt, pos->hash, depth, best, flag, best_move);
    }
    return best;
}
```

> Note: this converts negamax from a fail-hard (`return beta`) to a fail-soft (`return best`) formulation, which is what the TT store needs. The value returned to the root is unchanged for the best move, and the `test_tt_invariance` test confirms TT vs no-TT agree.

- [ ] **Step 5: Implement ID, `ch_search_bestscore`, `ch_search_ex`, `ch_search`**

Replace the current `ch_search` at the bottom of `engine.c` with:

```c
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
// The root uses a FULL window (-CH_INF, CH_INF) for every move, so scores[i] is
// each move's exact value -- the randomness pool in ch_search_ex depends on
// that. Alpha-beta pruning still happens in the deeper negamax calls, and the
// TT (seeded across ID iterations) supplies move ordering there. The root move
// list is not reordered, so moves[i] and scores[i] stay parallel for the caller.
static int search_root(ch_pos_t* pos, int depth, ch_tt_t* tt, ch_move_t* moves, int* scores, int* count) {
    const int n = ch_gen_legal(pos, moves);
    *count = n;
    if (n == 0) return 0;
    sort_moves(pos, moves, n);

    int best = -CH_INF;
    for (int d = 1; d <= depth; ++d) {
        best = -CH_INF;
        for (int i = 0; i < n; ++i) {
            ch_undo_t undo;
            ch_make(pos, &moves[i], &undo);
            scores[i] = -negamax(pos, d - 1, -CH_INF, CH_INF, 1, tt);
            ch_unmake(pos, &undo);
            if (scores[i] > best) best = scores[i];
        }
    }
    return best;
}

int ch_search_bestscore(ch_pos_t* pos, int depth, ch_tt_t* tt) {
    ch_move_t moves[CH_MAX_MOVES];
    int scores[CH_MAX_MOVES];
    int n = 0;
    return search_root(pos, depth, tt, moves, scores, &n);
}

bool ch_search_ex(ch_pos_t* pos, int depth, int margin, uint32_t* rng_state, ch_tt_t* tt, ch_move_t* best) {
    ch_move_t moves[CH_MAX_MOVES];
    int scores[CH_MAX_MOVES];
    int n = 0;
    const int best_score = search_root(pos, depth, tt, moves, scores, &n);
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
```

- [ ] **Step 6: Run engine_tt, then the full suite**

Run: `cc -O2 -o /tmp/ett main/chess/test/engine_tt_test.c main/chess/engine.c -Imain/chess && /tmp/ett`
Expected: PASS, including the TT-invariance and margin tests.

Run: `./main/chess/test/run_tests.sh`
Expected: perft, search, render, game suites all pass. (`search_test.c` calls `ch_search`, which now routes through `ch_search_ex`; mate-finding must still work.)

- [ ] **Step 7: Commit**

```bash
git add main/chess/engine.h main/chess/engine.c main/chess/test/engine_tt_test.c
git commit -m "chess: iterative deepening, TT-backed search, randomness margin"
```

---

## Task 4: Difficulty levels in chess_game

**Files:**
- Modify: `main/chess/chess_game.h` (`level` field, `chg_init_ex`, `chg_level_params`, `chg_level_label`, `CHG_NUM_LEVELS`)
- Modify: `main/chess/chess_game.c` (implementations)
- Modify: `main/chess/test/game_test.c` (level mapping tests)

**Interfaces:**
- Consumes: existing `chg_init`, `chg_set_position`, `chg_game_t`.
- Produces:
  - `#define CHG_NUM_LEVELS 5`
  - `uint8_t level;` field in `chg_game_t` (1..5)
  - `void chg_level_params(uint8_t level, int* depth, int* margin);` — Lv1→(3,40), Lv2→(4,0), Lv3→(5,0), Lv4→(6,0), Lv5→(7,0); out-of-range clamps to Lv2.
  - `const char* chg_level_label(uint8_t level);` — `"Lv1 ~1250"`, `"Lv2 ~1450"`, `"Lv3 ~1650"`, `"Lv4 ~1800"`, `"Lv5 ~1900"`.
  - `const char* chg_level_short(uint8_t level);` — `"Lv1"`..`"Lv5"` for the play-screen indicator.
  - `chg_action_t chg_init_ex(chg_game_t* game, uint8_t human_colour, uint8_t level);` — like `chg_init` but records `level`. `chg_init` keeps calling with level 2.

- [ ] **Step 1: Declare in chess_game.h**

Add `#define CHG_NUM_LEVELS 5` near the other constants, add `uint8_t level;` to `chg_game_t` (after `human`), and add the prototypes:

```c
chg_action_t chg_init_ex(chg_game_t* game, uint8_t human_colour, uint8_t level);
void chg_level_params(uint8_t level, int* depth, int* margin);
const char* chg_level_label(uint8_t level); // "Lv3 ~1650"
const char* chg_level_short(uint8_t level); // "Lv3"
```

- [ ] **Step 2: Write the failing level tests**

Add to `game_test.c` (new `printf("\nlevels\n")` section in `main`):

```c
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
```

- [ ] **Step 3: Run to verify it fails**

Run: `cc -O2 -o /tmp/gt main/chess/test/game_test.c main/chess/chess_game.c main/chess/chess_board.c main/chess/engine.c -Imain/chess && /tmp/gt`
Expected: link error — `chg_level_params` etc. undefined.

- [ ] **Step 4: Implement in chess_game.c**

```c
void chg_level_params(uint8_t level, int* depth, int* margin) {
    switch (level) {
    case 1: *depth = 3; *margin = 40; return;
    case 3: *depth = 5; *margin = 0;  return;
    case 4: *depth = 6; *margin = 0;  return;
    case 5: *depth = 7; *margin = 0;  return;
    case 2:
    default: *depth = 4; *margin = 0; return; // Lv2 default / clamp
    }
}

const char* chg_level_label(uint8_t level) {
    switch (level) {
    case 1: return "Lv1 ~1250";
    case 3: return "Lv3 ~1650";
    case 4: return "Lv4 ~1800";
    case 5: return "Lv5 ~1900";
    default: return "Lv2 ~1450";
    }
}

const char* chg_level_short(uint8_t level) {
    switch (level) {
    case 1: return "Lv1";
    case 3: return "Lv3";
    case 4: return "Lv4";
    case 5: return "Lv5";
    default: return "Lv2";
    }
}
```

Refactor `chg_init` to delegate, and add `chg_init_ex`. Replace the existing `chg_init`:

```c
chg_action_t chg_init_ex(chg_game_t* game, uint8_t human_colour, uint8_t level) {
    ch_pos_t start;
    ch_init(&start);
    const chg_action_t act = chg_set_position(game, &start, human_colour);
    game->level = (level >= 1 && level <= CHG_NUM_LEVELS) ? level : 2;
    return act;
}

chg_action_t chg_init(chg_game_t* game, uint8_t human_colour) {
    return chg_init_ex(game, human_colour, 2);
}
```

(`chg_set_position` zeroes the struct via `memset`, so `level` is 0 until set; set it after.)

- [ ] **Step 5: Run to verify it passes**

Run: `cc -O2 -o /tmp/gt main/chess/test/game_test.c main/chess/chess_game.c main/chess/chess_board.c main/chess/engine.c -Imain/chess && /tmp/gt`
Expected: PASS.

- [ ] **Step 6: Run the whole host suite**

Run: `./main/chess/test/run_tests.sh`
Expected: all suites pass.

- [ ] **Step 7: Commit**

```bash
git add main/chess/chess_game.h main/chess/chess_game.c main/chess/test/game_test.c
git commit -m "chess: difficulty level -> (depth, margin) mapping and labels"
```

---

## Task 5: Wire the engine into chess_ui (TT, RNG, level-driven search)

**Files:**
- Modify: `main/chess/chess_ui.c`

**Interfaces:**
- Consumes: `ch_search_ex`, `ch_tt_sizeof`, `ch_tt_init`, `ch_tt_clear`, `ch_tt_t` (Task 2/3); `chg_level_params`, `chg_game_t.level` (Task 4); `get_random` (`main/random.h`); `JADE_MALLOC_PREFER_SPIRAM`.
- Produces: the running app searches at the level's depth/margin with a live TT, on hardware.

This task is firmware glue; there is no host unit test. It is verified by building libjade and driving it (Task 7).

- [ ] **Step 1: Add includes, TT sizing, and RNG/TT state**

Add to the include block in `chess_ui.c`:

```c
#include "../random.h"
```

Replace the depth/stack defines:

```c
// Deepest selectable level (Lv5). The search stack must cover it.
#define CHESS_MAX_DEPTH 7
#define CHESS_SEARCH_STACK ((CHESS_MAX_DEPTH + CH_QUIESCE_MAX) * 1536 + 8192)
// ~1 MB transposition table (PSRAM). 2^16 entries * sizeof(ch_tt_entry_t).
#define CHESS_TT_ENTRIES (1u << 16)
```

Extend `search_args_t` to carry the level, TT and RNG:

```c
typedef struct {
    ch_pos_t pos;
    ch_move_t best;
    bool found;
    int depth;
    int margin;
    uint32_t* rng;
    ch_tt_t* tt;
} search_args_t;
```

- [ ] **Step 2: Update the search task body**

Replace `search_impl`:

```c
static bool search_impl(void* ctx) {
    search_args_t* args = ctx;
    JADE_ASSERT(args);
    args->found = ch_search_ex(&args->pos, args->depth, args->margin, args->rng, args->tt, &args->best);
    return true;
}
```

- [ ] **Step 3: Thread level/TT/RNG through engine_turn**

Change `engine_turn` to take the game plus the shared TT and RNG, and derive depth/margin from the game's level:

```c
static chg_action_t engine_turn(chg_game_t* game, ch_tt_t* tt, uint32_t* rng) {
    search_args_t args = {0};
    args.pos = game->view.pos;
    args.found = false;
    args.rng = rng;
    args.tt = tt;
    chg_level_params(game->level, &args.depth, &args.margin);

    if (!run_in_temporary_task(CHESS_SEARCH_STACK, search_impl, &args)) {
        JADE_LOGE("chess: failed to run search task");
        return CHG_ACT_NONE;
    }
    if (!args.found) {
        JADE_LOGE("chess: engine found no move in a position with legal moves");
        return CHG_ACT_NONE;
    }
    return chg_engine_played(game, &args.best);
}
```

- [ ] **Step 4: Allocate the TT and seed the RNG in chess_ui_run**

In `chess_ui_run`, after allocating `buf` and before the game loop, add:

```c
    // ~1 MB transposition table in PSRAM, cleared per game. Freed on exit.
    void* const tt_buf = JADE_MALLOC_PREFER_SPIRAM(ch_tt_sizeof(CHESS_TT_ENTRIES));
    ch_tt_t tt;
    ch_tt_init(&tt, tt_buf, CHESS_TT_ENTRIES);

    // Seed the easy-level move RNG from the hardware RNG.
    uint32_t rng = 0;
    get_random(&rng, sizeof(rng));
    if (rng == 0) rng = 0x1234567u;
```

Update the two `engine_turn(&game)` call sites to `engine_turn(&game, &tt, &rng)`, and clear the TT at the start of each new game (after `chg_init*`): `ch_tt_clear(&tt);`.

In the cleanup at the end of `chess_ui_run`, after `free(buf);` add `free(tt_buf);`.

- [ ] **Step 5: Build libjade with chess and confirm it links and plays**

```bash
sudo docker run --rm -v "$PWD":/host/jade -w /host/jade \
  blockstream/jade_builder@sha256:8b739db85b6b99664db0e3ece57cddfa4e0fee4101a20ca930c391273f21e2f9 \
  bash -c '. /opt/esp/idf/export.sh >/dev/null 2>&1; cd /host/jade; \
    ./tools/switch_to.sh jade --dev --noradio >/dev/null 2>&1; \
    ./libjade/make_libjade.sh Debug --chess --no-ci 2>&1 | grep -E "error|Built target jade$"'
```

Expected: `[100%] Built target jade`, no errors.

- [ ] **Step 6: Commit**

```bash
git add main/chess/chess_ui.c
git commit -m "chess: search at the selected level with a live transposition table"
```

---

## Task 6: Setup menu, colour choice, level indicator, new-game flow

**Files:**
- Modify: `main/button_events.h` (menu button ids)
- Modify: `main/chess/chess_ui.c` (setup menu, settings static, indicator, flow)

**Interfaces:**
- Consumes: `make_menu_activity`, `btn_data_t`, `gui_make_text`, `gui_update_text`, `gui_set_align`, `gui_set_current_activity`, `gui_activity_wait_event`, `GUI_EVENT` ids (from Task 5 wiring and existing includes); `chg_level_short`, `chg_level_label`, `chg_init_ex` (Task 4).
- Produces: entering Chess opens the setup menu; Play launches a game with the chosen colour and level; the panel shows the level; New game returns to the menu.

Firmware glue; verified by driving libjade (Task 7).

- [ ] **Step 1: Add button ids**

In `main/button_events.h`, next to `BTN_SETTINGS_CHESS`, add:

```c
    BTN_CHESS_COLOUR,
    BTN_CHESS_LEVEL,
    BTN_CHESS_PLAY,
    BTN_CHESS_QUIT,
```

- [ ] **Step 2: Add the settings static and colour enum**

Near the top of `chess_ui.c` (inside the guards), add:

```c
enum { CHESS_COL_WHITE = 0, CHESS_COL_BLACK, CHESS_COL_RANDOM };
static const char* const chess_colour_name[] = { "White", "Black", "Random" };

// Session-only settings (reset on reboot; no NVS).
static uint8_t chess_colour_choice = CHESS_COL_WHITE;
static uint8_t chess_level = 2; // Lv2 default
```

- [ ] **Step 3: Build the setup menu activity**

Add:

```c
static gui_activity_t* make_chess_setup_activity(gui_view_node_t** colour_txt, gui_view_node_t** level_txt) {
    JADE_ASSERT(colour_txt);
    JADE_ASSERT(level_txt);

    btn_data_t hdrbtns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_CHESS_QUIT },
        { .txt = NULL, .font = GUI_DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE } };

    char colour_line[24];
    snprintf(colour_line, sizeof(colour_line), "Colour: %s", chess_colour_name[chess_colour_choice]);
    gui_make_text(colour_txt, colour_line, TFT_WHITE);
    gui_set_align(*colour_txt, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);

    gui_make_text(level_txt, chg_level_label(chess_level), TFT_WHITE);
    gui_set_align(*level_txt, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);

    btn_data_t menubtns[] = {
        { .content = *colour_txt, .ev_id = BTN_CHESS_COLOUR },
        { .content = *level_txt, .ev_id = BTN_CHESS_LEVEL },
        { .txt = "Play", .font = GUI_DEFAULT_FONT, .ev_id = BTN_CHESS_PLAY },
    };
    return make_menu_activity("Chess", hdrbtns, 2, menubtns, 3);
}
```

- [ ] **Step 4: Run the setup menu; return the resolved colour + level, or -1 to exit**

Add a helper that runs the menu loop and returns the human colour to play (or 0xFF to exit):

```c
// Returns CH_WHITE or CH_BLACK to start a game, or 0 to exit the app.
static uint8_t run_chess_setup(uint32_t* rng) {
    gui_view_node_t* colour_txt = NULL;
    gui_view_node_t* level_txt = NULL;
    gui_activity_t* act = make_chess_setup_activity(&colour_txt, &level_txt);
    gui_set_current_activity(act);

    while (true) {
        int32_t ev_id = 0;
        if (!gui_activity_wait_event(act, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0)) continue;
        switch (ev_id) {
        case BTN_CHESS_COLOUR: {
            chess_colour_choice = (uint8_t)((chess_colour_choice + 1) % 3);
            char line[24];
            snprintf(line, sizeof(line), "Colour: %s", chess_colour_name[chess_colour_choice]);
            gui_update_text(colour_txt, line);
            break;
        }
        case BTN_CHESS_LEVEL:
            chess_level = (uint8_t)(chess_level % CHG_NUM_LEVELS + 1); // 1..5 wrap
            gui_update_text(level_txt, chg_level_label(chess_level));
            break;
        case BTN_CHESS_PLAY: {
            uint8_t colour = chess_colour_choice == CHESS_COL_WHITE ? CH_WHITE
                           : chess_colour_choice == CHESS_COL_BLACK ? CH_BLACK
                           : ((xrng_ui(rng) & 1u) ? CH_WHITE : CH_BLACK);
            return colour;
        }
        case BTN_CHESS_QUIT:
            return 0;
        default:
            break;
        }
    }
}
```

Add a tiny local xorshift for colour randomness (the engine's `xrng` is static to `engine.c`):

```c
static uint32_t xrng_ui(uint32_t* s) {
    uint32_t x = *s ? *s : 0x2545F491u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
```

- [ ] **Step 5: Restructure chess_ui_run around the setup menu**

Wrap the game loop so the setup menu runs first and on each New game. The outer shape:

```c
void chess_ui_run(void) {
    gui_activity_t* const prev_act = gui_current_activity();
    uint16_t* const buf = JADE_MALLOC_PREFER_SPIRAM(CHB_BUF_PIXELS * sizeof(uint16_t));
    const Picture pic = { .data = buf, .width = CHB_BOARD_PX, .height = CHB_BOARD_PX, .bytes_per_pixel = 2 };
    void* const tt_buf = JADE_MALLOC_PREFER_SPIRAM(ch_tt_sizeof(CHESS_TT_ENTRIES));
    ch_tt_t tt; ch_tt_init(&tt, tt_buf, CHESS_TT_ENTRIES);
    uint32_t rng = 0; get_random(&rng, sizeof(rng)); if (rng == 0) rng = 0x1234567u;

    idletimer_set_min_timeout_secs(CHESS_MIN_TIMEOUT_SECS);

    for (;;) {
        const uint8_t colour = run_chess_setup(&rng);
        if (colour == 0) break; // Exit chosen in the setup menu

        chess_nodes_t nodes;
        gui_activity_t* const act = make_chess_activity(&nodes);
        ch_tt_clear(&tt);
        chg_game_t game;
        chg_action_t action = chg_init_ex(&game, colour, chess_level);
        gui_set_current_activity(act);
        repaint(&game, &nodes, buf, &pic);

        bool to_setup = false;
        while (!to_setup) {
            if (action == CHG_ACT_ENGINE) {
                repaint(&game, &nodes, buf, &pic);
                const chg_state_t before = game.state;
                action = engine_turn(&game, &tt, &rng);
                if (game.state == before && before == CHG_ENGINE_THINK) { JADE_LOGE("chess: abandoning game"); to_setup = true; break; }
                repaint(&game, &nodes, buf, &pic);
                continue;
            }
            int32_t id = 0;
            if (!gui_activity_wait_event(act, GUI_EVENT, ESP_EVENT_ANY_ID, NULL, &id, NULL, 0)) continue;
            switch (id) {
            case GUI_WHEEL_LEFT_EVENT:  chg_prev(&game); break;
            case GUI_WHEEL_RIGHT_EVENT: chg_next(&game); break;
            case GUI_FRONT_CLICK_EVENT:
            case GUI_WHEEL_CLICK_EVENT:
                action = chg_select(&game);
                if (action == CHG_ACT_EXIT) to_setup = true; // New game / Exit -> back to setup
                break;
            default: break;
            }
            if (!to_setup) repaint(&game, &nodes, buf, &pic);
        }
        gui_clear_picture(nodes.board);
    }

    idletimer_set_min_timeout_secs(0);
    free(buf);
    free(tt_buf);
    if (prev_act) gui_set_current_activity(prev_act);
}
```

> Note: the game-over ring already offers `New game` and `Exit`, both of which return `CHG_ACT_EXIT` from `chg_select`. Both now route back to the setup menu; picking `Exit` there leaves the app. This is the "New game returns to setup" flow from the spec.

- [ ] **Step 6: Add the level indicator to the panel**

In `make_chess_activity`, the panel vsplit currently has 4 parts (status/entry/counter/history). Change it to place a compact level tag. Simplest: append the level to the status row via a dedicated node. Add a `level` node to `chess_nodes_t`:

```c
typedef struct {
    gui_view_node_t* board;
    gui_view_node_t* status;
    gui_view_node_t* level;
    gui_view_node_t* entry;
    gui_view_node_t* counter;
    gui_view_node_t* hist[CHESS_HIST_LINES];
} chess_nodes_t;
```

Change the panel split to 5 parts and add a FILL-backed level line under the status (mirroring the other rows):

```c
    gui_view_node_t* vsplit;
    gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 5, 18, 14, 20, 14, 34);
    gui_set_parent(vsplit, hsplit);

    gui_view_node_t* bg;
    gui_make_fill(&bg, TFT_BLACK, FILL_PLAIN, vsplit);
    gui_make_text(&nodes->status, "", TFT_WHITE);
    gui_set_align(nodes->status, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(nodes->status, bg);

    gui_make_fill(&bg, TFT_BLACK, FILL_PLAIN, vsplit);
    gui_make_text(&nodes->level, "", TFT_LIGHTGREY);
    gui_set_align(nodes->level, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
    gui_set_parent(nodes->level, bg);
```

Keep the existing entry/counter/history rows as the remaining three parts (they already each have a FILL parent). Then in `repaint`, set the level text once per paint:

```c
    gui_update_text(nodes->level, chg_level_short(game->level));
```

- [ ] **Step 7: Rebuild libjade and confirm it links**

```bash
sudo docker run --rm -v "$PWD":/host/jade -w /host/jade \
  blockstream/jade_builder@sha256:8b739db85b6b99664db0e3ece57cddfa4e0fee4101a20ca930c391273f21e2f9 \
  bash -c '. /opt/esp/idf/export.sh >/dev/null 2>&1; cd /host/jade; \
    ./tools/switch_to.sh jade --dev --noradio >/dev/null 2>&1; \
    ./libjade/make_libjade.sh Debug --chess --no-ci 2>&1 | grep -E "error|Built target jade$"'
```

Expected: `[100%] Built target jade`.

- [ ] **Step 8: Commit**

```bash
git add main/button_events.h main/chess/chess_ui.c
git commit -m "chess: pre-game setup menu (colour + level), level indicator, new-game flow"
```

---

## Task 7: Drive-test under libjade, measure size, update docs

**Files:**
- Create: a throwaway driver script under the scratchpad (not committed)
- Modify: `README.md` (mention difficulty/colour in the Chess section)
- Modify: `docs/superpowers/specs/2026-07-17-chess-difficulty-setup-design.md` (verification status)

**Interfaces:** none new.

- [ ] **Step 1: Drive the setup menu and a game via libjade**

Using the existing `scratchpad/drive.py` `Jade` helper (from the base-app verification), write a driver that: opens Options → Chess, shoots the setup menu, cycles Colour and Level, presses Play, plays a move, lets the engine reply, and shoots the play screen (confirming the `Lv` indicator). Run it with `LD_LIBRARY_PATH=$PWD/build_linux/libjade`. Inspect the screenshots: setup menu shows `Colour:`/`Lv2 ~1450`/`Play`; play screen shows the `Lv2` tag and a legal engine reply.

Expected: setup menu renders, colour and level cycle, a game starts at the chosen level, engine replies, `New game` returns to the setup menu.

- [ ] **Step 2: Measure the flash/RAM delta**

Build `jade_v2` with and without `--chess` and diff `idf.py size` (as in the earlier A/B measurement). Record the `.text`/`.rodata`/`.bss` deltas — the Zobrist table adds ~6 KB `.bss` (12×64×8), and the TT is runtime PSRAM, not static.

```bash
sudo docker run --rm -v "$PWD":/host/jade -w /host/jade \
  blockstream/jade_builder@sha256:8b739db85b6b99664db0e3ece57cddfa4e0fee4101a20ca930c391273f21e2f9 \
  bash -c '. /opt/esp/idf/export.sh >/dev/null 2>&1; cd /host/jade; \
    ./tools/switch_to.sh jade_v2 --dev --chess >/dev/null 2>&1; idf.py all >/dev/null 2>&1; idf.py size'
```

Expected: builds and fits; note the deltas.

- [ ] **Step 3: Update docs**

In `README.md`'s Chess section, note that difficulty (Lv1–Lv5) and colour are chosen in a setup menu when the app opens. In the design spec, tick the verification table: engine invariants proven on host; setup menu / levels / indicator run under libjade; device build measured.

- [ ] **Step 4: Run the full host suite one last time**

Run: `./main/chess/test/run_tests.sh`
Expected: all suites pass, including `engine_tt_test` and the new level tests.

- [ ] **Step 5: Commit**

```bash
git add README.md docs/superpowers/specs/2026-07-17-chess-difficulty-setup-design.md
git commit -m "docs: record difficulty/setup verification and size impact"
```

---

## Self-Review

**Spec coverage:**
- TT + iterative deepening → Tasks 1–3. ✓
- `ch_search_ex` with margin/rng → Task 3. ✓
- Zobrist in the position → Task 1. ✓
- Five levels 3/4/5/6/7, Lv2 default, Lv1 margin 40 → Task 4 (`chg_level_params`). ✓
- Setup menu (colour White/Black/Random, level), click-to-cycle → Task 6. ✓
- Session-only persistence, no storage.c → Task 6 statics. ✓
- Level indicator on the play screen → Task 6 Step 6. ✓
- New game returns to setup → Task 6 Step 5. ✓
- TT in PSRAM via caller buffer → Task 2 (`ch_tt_init`) + Task 5 (`JADE_MALLOC_PREFER_SPIRAM`). ✓
- Stack sized for depth 7 → Task 5 (`CHESS_MAX_DEPTH`). ✓
- Perft as the load-bearing guard → Task 1 Step 6, and full suite in Tasks 3/4/7. ✓
- TT-invariance test → Task 3 Step 2. ✓

**Type consistency:** `ch_search_ex(pos, depth, margin, rng_state, tt, best)` is defined in Task 3 and consumed identically in Task 5's `search_impl`. `ch_tt_t`/`ch_tt_init`/`ch_tt_sizeof`/`ch_tt_clear` defined in Task 2, used in Tasks 3/5. `chg_level_params(level, *depth, *margin)` defined in Task 4, used in Task 5. `chg_level_short`/`chg_level_label` defined in Task 4, used in Task 6. `chg_init_ex(game, colour, level)` defined in Task 4, used in Task 6. Button ids added in Task 6 Step 1 before use in Steps 3–5. Consistent.

**Placeholder scan:** No TBD/TODO; every code step shows complete code. Task 5/6 are glue with build-and-drive verification rather than unit tests, which is the honest test for firmware-only code (stated explicitly).

**One noted deviation from the spec:** the spec described "incremental" Zobrist; the plan implements it incrementally in `ch_make` (the XOR interleave), matching intent. `negamax` is converted from fail-hard to fail-soft to support TT storage — the `test_tt_invariance` test guards that the root result is unchanged.
