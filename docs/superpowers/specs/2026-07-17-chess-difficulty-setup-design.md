# Chess App: Difficulty, Setup Menu, Stronger Engine — Design

**Date:** 2026-07-17
**Status:** Design, pending implementation
**Builds on:** [2026-07-16-chess-app-design.md](./2026-07-16-chess-app-design.md)
**Target:** Personal / DIY build. Confirmed working on a physical Jade Plus.

## Goal

Three additions to the working chess app:

1. **Stronger play** via a transposition table and iterative deepening, so the
   engine can search deep enough to see basic pins, skewers and forks.
2. **A pre-game setup menu** to choose colour (White / Black / Random) and a
   difficulty level.
3. **A level indicator** on the play screen.

## Why the engine work is the real lever

At a fixed depth the engine has no positional understanding of tactics -- the
eval is material + piece-square only, so a pin or skewer is "seen" solely when
the search reaches the ply where material actually changes and settles through
quiescence. Depth 3 catches one- and two-move shots and misses the rest.

A transposition table and iterative deepening do **not** add tactical vision at
a fixed depth; at the same target depth they return essentially the same move.
What they buy is speed: the TT best-move from the previous iteration is searched
first, sharpening alpha-beta cutoffs, so the same wall-clock budget reaches
roughly two ply deeper. Depth 5-7 in the time depth 3-4 costs today. That extra
depth is where the tactics improvement comes from.

## Difficulty levels

Five levels. Difficulty is primarily search depth; the lowest level also injects
move randomness so it is not perfectly sharp. ELO figures are rough estimates
(±150), shown to the player for orientation, not claimed as measured.

| Level | Depth | Randomness margin | Label |
|---|---|---|---|
| Lv1 | 3 | 40 cp | `Lv1 ~1250` |
| Lv2 (default) | 4 | 0 (best) | `Lv2 ~1450` |
| Lv3 | 5 | 0 | `Lv3 ~1650` |
| Lv4 | 6 | 0 | `Lv4 ~1800` |
| Lv5 | 7 | 0 | `Lv5 ~1900` |

- **Default is Lv2** (depth 4): fast, and a sensible middle strength.
- **Lv5 (depth 7) is deliberately slow.** On the ESP32-S3 it may take from
  several seconds to low tens of seconds per move even with the TT. There is no
  task watchdog on jade_v2 (`CONFIG_ESP_TASK_WDT_EN=n`), so slow-but-correct is
  acceptable; the "Thinking..." indicator keeps painting throughout because the
  GUI task outranks the search task.
- **No true-beginner floor.** The old depth-2/90cp level was dropped by request;
  Lv1 is depth 3 with mild randomness (~1250). More randomness can be added to
  Lv1 later if a gentler mode is wanted.

### Randomness model

A new engine entry point:

```c
bool ch_search_ex(ch_pos_t* pos, int depth, int margin,
                  uint32_t* rng_state, ch_tt_t* tt, ch_move_t* best);
```

Searches all root moves, then among those scoring within `margin` centipawns of
the best, picks one at random. `margin == 0` reproduces strict best-move play.

The existing `ch_search()` is **kept as a thin wrapper** --
`ch_search_ex(pos, depth, 0, NULL, NULL, best)` -- so current callers and tests
(`search_test.c`, `game_test.c`) are untouched. A NULL `tt` means "no table";
a NULL `rng` with `margin == 0` is deterministic best-move play.

`rng_state` is a caller-seeded xorshift, so `engine.c` stays dependent on
nothing outside libc and is deterministic given a seed (tests can pin it). On
device the seed comes from the hardware RNG (`random.c`); on the host, from the
clock.

## Engine internals

### Zobrist hashing

- Add `uint64_t hash` to `ch_pos_t`, and save the previous value in `ch_undo_t`
  so `ch_unmake` restores it exactly.
- Update the hash incrementally in `ch_make`/`ch_unmake` (piece on/off squares,
  side to move, castling rights, en-passant file).
- Compute the initial hash from scratch in `ch_init` and `ch_from_fen`.
- Zobrist key table generated once at startup from a fixed seed. It need not be
  stable across builds -- only within a single process, for TT consistency.

The hash becomes part of the position state, so `ch_unmake` must restore it and
the make/unmake round-trip test must cover it. **Perft is the guard:** it does
not use the TT, so if perft still matches after this change, make/unmake is
still correct.

### Transposition table

- Fixed-size array in PSRAM, ~1 MB default (2^16 entries x 16 B: key, best move,
  score, depth, bound flag).
- Caller-allocated: `ch_tt_t* ch_tt_alloc(size_t entries)` / `ch_tt_free()`, and
  passed into `ch_search_ex`. The UI owns its lifetime (allocate on entering the
  app, free on exit), mirroring the framebuffer. Tests allocate their own.
- Depth-preferred replacement; the stored best move seeds move ordering.
- **Invariant, and the key regression test:** the TT must never change the
  result, only the speed. A TT-backed search and a plain search to the same
  depth must return the same score.

### Iterative deepening

Search depth 1, 2, ..., target, each iteration seeding ordering from the TT.
Returns the same score as a direct search to the target depth (another test).
Also leaves a best move available at each depth, which a future time-limited
mode could use, though difficulty here is depth-based, not time-based.

### Stack sizing

The search stack must cover the **deepest selectable level (depth 7)**, not a
fixed depth:

```c
#define CHESS_MAX_DEPTH 7
#define CHESS_SEARCH_STACK ((CHESS_MAX_DEPTH + CH_QUIESCE_MAX) * 1536 + 8192)
```

That is ~28 KB (PSRAM), up from ~22 KB. See the memory summary below.

## Setup menu and colour

Entering **Chess** now opens a menu instead of starting a game:

```
Colour:  White        (click cycles White -> Black -> Random)
Level:   Lv2 ~1450    (click cycles Lv1 -> Lv5, wrapping)
Play
Exit
```

- Built with `make_menu_activity`; value rows update in place with
  `update_menu_item(node, label, value)` -- the pattern the BIP39 passphrase
  settings menu already uses.
- Navigation buttons move between rows; clicking a value row advances it and
  repaints its label; clicking `Play` starts a game; `Exit` leaves.
- `Random` colour is resolved when `Play` is pressed (via the game RNG).

### Persistence (session only)

Settings live in a static struct in `chess_ui.c`:

```c
typedef struct { uint8_t colour_choice; uint8_t level; } chess_settings_t;
```

Initialised to { White, Lv2 }, updated by the menu, and surviving across games
for the life of the app/session. Reset to defaults on reboot. **No `storage.c`
changes** -- deliberately no coupling to the wallet's persistence layer.

## Level indicator

A compact `Lv2` tag on the play-screen side panel. The panel is tight at 170px
on Jade Plus, so the level folds into the existing layout (alongside the status
line) rather than adding a row. It reflects the level chosen in the setup menu.

## New-game flow

After game-over, **New game** returns to the setup menu (showing the remembered
colour and level) rather than restarting blindly, so the player can adjust
before replaying. `Play` starts the next game.

## Units

- `engine.c` / `engine.h` -- Zobrist hash in the position, TT alloc/probe/store,
  iterative deepening, `ch_search_ex` with margin and RNG. Still libc-only.
- `chess_game.c` / `chess_game.h` -- carries the chosen colour and level, maps a
  level to (depth, margin), and exposes the level for the indicator.
- `chess_ui.c` -- setup menu, level indicator, TT alloc/free, RNG seeding. If the
  menu grows the file too large, split it into `chess_setup.c`.

## Memory impact

Runtime, while playing (all PSRAM, freed on exit):

| allocation | before | after |
|---|---|---|
| board framebuffer | 50 KB | 50 KB |
| search stack | 22 KB | ~28 KB (depth 7) |
| transposition table | -- | ~1 MB |
| game struct (internal DRAM stack) | ~1 KB | ~1 KB |
| **peak** | ~72 KB | ~1.08 MB |

The TT dominates, but it is PSRAM (8 MB available) and released on exit. Internal
DRAM is essentially unchanged. Flash grows modestly (Zobrist keys, TT and search
code); to be measured with the same `--chess` A/B build used before.

## Testing

Host suites (no device, no ESP-IDF), extended:

- **Perft stays green** -- proves the hash-in-state change did not break
  make/unmake. This is the load-bearing regression check.
- **Zobrist**: `make` then `unmake` restores the hash; two move orders reaching
  the same position produce equal hashes.
- **TT invariance**: for a set of positions, TT-backed search and plain search to
  the same depth return the same score. The single most important new test.
- **Iterative deepening** to depth N returns the same score as a direct depth-N
  search.
- **Randomness**: with margin M the chosen move scores within M of the best; with
  margin 0 it is always the best (deterministic given seed).
- **Levels**: each level maps to the expected (depth, margin).

Then rebuild libjade `--chess` and drive it via `send_input`/`get_display_bytes`
to confirm the setup menu, colour choice, level indicator and new-game flow, as
the base app was verified.

## Risks

- **Incremental Zobrist through make/unmake is the riskiest change** -- a wrong
  update is a subtle bug. Perft plus the round-trip and transposition-equality
  tests are the guard, all on the host.
- **Depth 7 latency on device** is unknown until measured; a `--log` build prints
  `Temporary task stack HWM: %u free` and lets the "Thinking..." duration be
  timed by eye.
- **TT and randomness must not leak into perft**, which must remain exact and
  deterministic. `ch_perft` does not call `ch_search_ex` or touch the TT.

## Out of scope (v1)

- Persisting settings across reboots (NVS) -- session-only by choice.
- Time-based difficulty -- levels are depth-based; ID leaves the door open.
- Killer/history heuristics, null-move pruning, check extensions, king-safety or
  pawn-structure eval terms. Worth more strength-per-effort than raising depth,
  but a separate piece of work.
