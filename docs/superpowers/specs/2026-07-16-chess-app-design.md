# Chess App for Jade Plus — Design

**Date:** 2026-07-16
**Status:** Design, pending implementation
**Target:** Personal / DIY build. Not intended for upstream or production firmware.

## Goal

Add a chess app to Jade firmware, launchable from the dashboard menu, that renders a
board and lets the user play a full legal game against an on-device engine.

This is a personal hack on a development device. It is explicitly *not* a proposal for
production Jade firmware: a chess engine linked into a hardware wallet is new attack
surface with no security benefit, and it perturbs reproducible builds, secure boot and
attestation. Those objections are real but out of scope here, because the target device
holds no funds (see below). The app is nonetheless gated at compile time so it can never
appear in a default build.

## Verified target

Queried from the connected device on 2026-07-16 via `jadepy` `get_version_info()`:

| Field | Value | Consequence |
|---|---|---|
| `JADE_FEATURES` | `DEV` | Development unit, not secure-boot locked — custom firmware can be flashed |
| `BOARD_TYPE` | `JADE_V2` | Jade Plus: 320×170 display, `navbtns` + `selectbtn` input |
| `JADE_NETWORKS` | `TEST` | Testnet-only unit — no real funds at risk |
| `ATTESTATION_INITIALISED` | `False` | No attestation state to invalidate |
| `JADE_VERSION` | `1.0.40` | Current firmware |
| `IDF_VERSION` | `v5.4` | Repo pins `ESP_IDF_BRANCH=v5.5.4` (`Dockerfile:9`); rebuild moves to 5.5.4 |

Hardware budget, from the repo rather than assumption:

- **Flash:** `ota_0` = `4024K` (`partitionss3.csv`). micro-Max is ~2KB of source. Non-issue.
- **RAM:** octal PSRAM enabled (`CONFIG_SPIRAM_MODE_OCT=y`, `configs/sdkconfig_dev_jade_v2.defaults:73`). Non-issue.
- **Watchdog:** `CONFIG_ESP_TASK_WDT_EN=n` (same file, line 39). A multi-second search will not trip a task watchdog.
- **Cores:** GUI task runs on `JADE_CORE_SECONDARY` at `tskIDLE_PRIORITY + 3`; main task is core 0 at `tskIDLE_PRIORITY + 1` (`main/jade_tasks.h`). A blocking search in the main task will not starve the display.

## Non-goals (v1)

Deliberately excluded to keep this to a single implementable plan:

- Save/resume a game across reboots (no NVS persistence)
- Chess clock / time controls
- Opening book
- Puzzle or tactics mode
- Two-player (human vs human) mode
- PGN export via QR
- Engine difficulty levels beyond a single fixed search depth

Undo is also excluded from v1, though it is cheap to add later since the move stack
already exists in game state.

## Architecture

### File layout

**As built** (deviates from the original three-unit sketch; see As-built notes below):

```
main/chess/
  engine.c/.h        # 0x88 board, legal move generation, make/unmake, search
  chess_board.c/.h   # game state, SAN, board -> raw RGB565 buffer
  chess_game.c/.h    # legal-move cycling state machine
  chess_ui.c/.h      # Jade activity, input pump, engine task
  test/              # host-only: perft_test, search_test, render_test, game_test
```

Four units, each with one job. The first three depend on nothing outside libc and are
fully tested on the host; only `chess_ui.c` touches the firmware. `chess_ui.c` never
reaches into engine internals — it pumps prev/next/select into `chess_game.c` and paints
what comes out.

`test/` is a subdirectory, and `SRC_DIRS` is not recursive, so the test files (each with
its own `main()`) are never compiled into the firmware.

### Build integration

Four separate things, all of which must happen or the app silently disappears:

1. **Kconfig gate.** New `config CHESS_APP` in `main/Kconfig.projbuild`, `default n`, with
   **no `BOARD_TYPE` dependency**. Nothing else in the firmware may reference chess symbols
   outside `#ifdef CONFIG_CHESS_APP`.

   A tempting `depends on BOARD_TYPE_JADE_V2_ANY` is **wrong**: libjade defines no
   `BOARD_TYPE_*` symbol at all, and qemu is `BOARD_TYPE_QEMU_LARGER`. Gating on the board
   would make the app unbuildable in exactly the two environments where most development
   happens. The board is a fixed size centred in whatever display it gets (see Rendering),
   so no board gate is needed.
2. **Amalgamation.** Each new `.c` must be added to `main/amalgamated.c` inside
   `#ifdef CONFIG_CHESS_APP`, in sorted position. Default builds set
   `CONFIG_AMALGAMATED_BUILD`; a file absent from `amalgamated.c` compiles under
   `--unamalgamated` and vanishes otherwise. This is the single easiest way to lose an
   afternoon.
3. **CMake.** `main/CMakeLists.txt` uses `SRC_DIRS`, so a new `chess` dir needs listing
   for unamalgamated builds.
4. **libjade config is hand-maintained.** `libjade/include/sdkconfig.h` is a static,
   hand-written header — it does **not** read the project `sdkconfig`. Building chess under
   libjade requires manually adding `#define CONFIG_CHESS_APP 1` there. Forgetting this
   produces a clean build with no chess in it.

### Dashboard entry

`main/ui/dashboard.c` builds menus from `btn_data_t` arrays passed to
`make_menu_activity(title, hdrbtns, n, menubtns, n)`, dispatching on `.ev_id`. The chess
entry is one more `btn_data_t` with a new `BTN_CHESS` event id, following the exact
pattern of the existing settings menus. Placed in the Options menu rather than the root
dashboard, since the root is connection-oriented.

## Rendering

**Approach: offscreen `Picture` buffer pushed into a single `PICTURE` view node.**

Precedent: `main/camera.c:416,481` builds a `Picture` (`display.h` — `uint16_t* data`,
`width`, `height`, `bytes_per_pixel`) and pushes live frames into a `PICTURE` node via
`gui_update_picture(node, &pic, false)`. Live pixel content inside the GUI framework is
already solved in this codebase; the board is a less demanding camera preview.

Rejected alternatives:

- *64 view nodes* (nested `HSPLIT`/`VSPLIT` of `FILL` + `ICON`). Idiomatic but fights a
  layout engine built for menus, allocates 64–128 nodes, and `Icon` is monochrome
  (`display.h:29-32` — bitmap plus one `color_t`), so pieces would be flat-colored against
  the square beneath them.
- *Direct `display_fill_rect()` / `display_icon()`*. The GUI task owns the display and
  would repaint over us, and we would lose activity and input integration.

### Screen layout

The board is **fixed at 160×160 (8 × 20px squares)** and centred vertically in whatever
`CONFIG_DISPLAY_HEIGHT` the build has; the panel takes the remaining width. This keeps one
set of piece sprites and one set of pixel offsets across every target:

| Target | Display | Board | Vertical margin |
|---|---|---|---|
| Jade Plus (`JADE_V2`) | 320×170 | 160×160 | 5px |
| qemu `--webdisplay-larger` (`QEMU_LARGER`) | 320×170 | 160×160 | 5px |
| libjade (`libjade/include/sdkconfig.h`) | 320×200 | 160×160 | 20px |

Scaling squares to fill the height instead would mean piece art at both 20px and 24px for
no real gain. Fixed 160×160 is simpler and predictable.

```
 x=0                        x=160                    x=319
 +---------------------------+------------------------+
 |                           |  Turn: White           |  y=0
 |     board 160x160         |  Engine: thinking...   |
 |     8 squares of 20px     |                        |
 |     centred vertically    |  1. e4    c5           |
 |                           |  2. Nf3   d6           |
 |                           |                        |
 |                           |  [ Nf3 ]  move 3/14    |
 +---------------------------+------------------------+ y=DISPLAY_HEIGHT
        160px board                 160px panel
```

The panel is a `VSPLIT` of `TEXT` nodes in the view tree — normal framework usage, no
custom rendering. Its height varies with the target, so it must lay out with the framework
rather than hardcoded offsets.

Piece art is purpose-drawn 20×20 RGB565 sprites. Stock chess art scaled to 20px is
illegible; at this size the glyphs must be designed for the pixel grid. Two piece colors
against two square colors means four combinations to check for contrast.

Board rendering is a pure function of game state:
`chess_render(const game_state_t*, Picture* out)`. Highlight state (selected piece,
candidate destination, last move, check) is passed in and drawn as square tinting.

## Interaction model

### Input vocabulary

From `main/input.c:15-16`, Jade Plus includes `navbtns.inc` + `selectbtn.inc`. From
`Kconfig.projbuild:278,286,304`:

| Input | GPIO | Event |
|---|---|---|
| `INPUT_BTN_A` | 38 | `gui_prev()` |
| `INPUT_BTN_B` | 39 | `gui_next()` |
| `INPUT_FRONT_SW` | 40 | `gui_front_click()` |

Both nav buttons pressed together also fire `gui_front_click()` (`navbtns.inc`,
`button_released()`), and holding either nav button auto-repeats prev/next but deliberately
never repeats select (`navbtns.inc`, `button_held()`).

**Constraint:** long-press-select is deliberately disabled — `selectbtn.inc`,
`button_front_long()`, logs `"front-btn long-press ignored"` with the `gui_front_click()`
call commented out. There is no gesture available for cancel or back. Every escape must be
a navigable UI element, as the rest of the firmware does with its `"="` header back button.

### Legal-move cycling

Free-roaming a cursor over 64 squares with prev/next costs ~30 clicks to reach a corner.
Instead the UI cycles **only legal moves**, which the engine already computes:

- `prev`/`next` cycle the ring; `select` commits.
- Ring contents depend on state; the board highlights the current ring item as you cycle.
- Illegal moves are unrepresentable, and the app doubles as a tutor for anyone who does
  not know how a knight moves.

Most moves become two or three clicks.

### State machine

```
PIECE_SELECT   ring = [pieces with >=1 legal move] + [Resign] + [Exit]
               select on piece -> DEST_SELECT
               select on Resign/Exit -> GAME_OVER / leave activity

DEST_SELECT    ring = [legal destinations for chosen piece] + [<- back]
               select on dest -> (promotion? PROMO_SELECT : commit) 
               select on back -> PIECE_SELECT

PROMO_SELECT   ring = [Queen, Rook, Bishop, Knight]
               select -> commit move

ENGINE_THINK   input ignored; panel shows "thinking..."
               engine returns -> PIECE_SELECT or GAME_OVER

GAME_OVER      ring = [New game] + [Exit]
```

`[← back]`, `[Resign]` and `[Exit]` are ring entries, not gestures — forced by the
long-press constraint above.

## Engine integration

**As built: a purpose-written 0x88 engine, not micro-Max.** See As-built notes below for
why. Alpha-beta with quiescence, MVV-LVA move ordering, material and piece-square
evaluation, and a real `ch_gen_legal()` API.

### Deep recursion

micro-Max recurses deeply. Jade already has the answer: `run_in_temporary_task(stack_size,
fn, ctx)` (`main/utils/temporary_stack.h:13`) runs a function in a short-lived task with a
given stack, without permanently bloating the main task. The search runs there and the UI
waits on its result.

Its sibling `run_on_temporary_stack()` (same header, line 10) swaps the *current* task's
stack instead. Prefer `run_in_temporary_task()` here: the search should be a separate task
so the UI can keep repainting a "thinking" indicator while it runs.

### Idle timer

The device auto-sleeps. Pondering a move for two minutes would blank the screen mid-game.
`idletimer_set_min_timeout_secs()` (`main/idletimer.h:8`) holds it awake for the duration
of the activity, restored on exit.

### The main risk: no move-generation API — RESOLVED

The original plan was micro-Max, whose headline problem was that it has **no
`list_legal_moves()`**: it is one recursive function `D()` that searches and generates
moves inseparably, while the entire UI depends on having that list. The mitigations on the
table were all unpleasant — hook the root move loop, duplicate the move generator, or
brute-force ~4000 (from,to) probes.

Writing the engine instead of porting one **dissolves this risk entirely**: `ch_gen_legal()`
is a first-class API, is the single source of truth for legality, and is what both the
search and the UI ring consume. There is nothing left to mitigate.

## Error handling

- **Engine returns no move** while the UI believes legal moves exist: state desync. Assert
  in debug (`JADE_ASSERT`); in release, abort the game to `GAME_OVER` with an error rather
  than corrupt the board.
- **Search exceeds a time bound:** micro-Max searches to fixed depth. Depth is chosen
  conservatively so worst-case search stays within a few seconds. If it overruns, the UI
  stays in `ENGINE_THINK` — acceptable, since there is no watchdog to trip.
- **Allocation failure** for the `Picture` buffer (160×160×2 = 50KB, PSRAM): fail the
  activity cleanly and return to the menu rather than proceed with a null buffer.
- **Fifty-move / threefold repetition / insufficient material:** detected in
  `chess_board.c`, not the engine, and routed to `GAME_OVER` as a draw.

## Testing

The dev loop does not need the physical device. The two off-device targets have different
strengths and are **not** interchangeable:

- **libjade** (`./libjade/make_libjade.sh Debug`, `./libjade/run_libjade_gui.sh`) runs the
  firmware natively on the desktop with a GUI, NVS and input. Fastest loop by a wide margin,
  native debugger and sanitizers, so it is where engine and state-machine work belongs.
  Build `--no-ci` so input is real rather than auto-accepted (CI mode is compiled in via
  `-DCI=` in `libjade/CMakeLists.txt:66` and auto-presses OK after 1ms).
  **Its display is 320×200, not the device's 320×170** — so it is the wrong place to judge
  layout.
- **qemu** (`Dockerfile.qemu`, `QEMU_CONFIG_ARGS="--dev --psram --webdisplay-larger"`) is
  `BOARD_TYPE_QEMU_LARGER` = **320×170, pixel-identical to the device**, rendered in a
  browser tab. This is the target for layout and piece-art work.
  Note `--webdisplay*` requires `--psram` (`tools/switch_to.sh:107`).
- **Device** only for final confirmation of panel colour rendering and button feel.

Rule of thumb: **logic on libjade, pixels on qemu, feel on hardware.**

Correctness testing:

- **Perft** is the decisive test for move generation: standard positions with known node
  counts at fixed depth. Run natively against the ported engine, no device involved. If
  perft matches at depth 4–5 from the start position and from the standard "kiwipete"
  position, move generation — including castling, en passant and promotion — is correct.
  This is the highest-value test in the project and should exist before any UI work.
- **Sanitizer builds** (`./libjade/make_libjade.sh Sanitize`) catch the buffer-overrun class
  of bug that a dense 0x88 engine invites.
- **Rendering** is a pure function of state, so it can be smoke-tested by dumping a
  `Picture` to PNG natively and eyeballing it.

Note `format.sh` runs `clang-format-19` over `main/*/*.{c,h,inc}`, so `main/chess/` is
formatted automatically and CI requires a clean `git diff`. micro-Max's dense formatting
**will** be reformatted; do not fight this.

## Phasing

1. **Engine + perft, natively.** No UI, no device, no firmware — compile `micromax.c` on the
   host and run perft. Prove move generation before anything else exists.
2. **Board rendering.** `chess_render()` to a `Picture`, dumped to PNG natively and
   eyeballed. Still no device and no GUI.
3. **Legal-move cycling UI.** Full state machine in **libjade** with real input (`--no-ci`),
   engine playing random legal moves. Logic only — ignore layout here, libjade is 320×200.
4. **Engine search integration.** `run_in_temporary_task()`, thinking indicator, idle timer.
   Still libjade.
5. **Layout and piece art.** Move to **qemu `--webdisplay-larger`** (320×170, pixel-identical
   to the device) and make it look right.
6. **Build integration and hardware.** Kconfig, `amalgamated.c`, `libjade/include/sdkconfig.h`,
   dashboard entry, then flash the dev Jade and check colour rendering and button feel.

Each phase is independently verifiable, and phases 1–5 need no hardware at all. Phases 1 and
2 do not even need the ESP-IDF toolchain.

## As-built notes

Where the implementation departed from this design, and why.

### Engine: purpose-written, not micro-Max

micro-Max was chosen for size, but flash was never the constraint (4024K partition), and it
carried the project's headline risk: no move-generation API. Two further problems settled
it: reproducing its famously dense ~2KB verbatim risks subtly-wrong constants in code that
is thoroughly unpleasant to debug, and its density is exactly what `format.sh` would
reformat anyway.

A written engine is ~700 lines, exposes `ch_gen_legal()` directly — dissolving the main
risk — and is provable: **all 26 perft cases pass**, matching published node counts
(startpos depth 5 = 4,865,609; kiwipete depth 4 = 4,085,603; positions 3-6). It sits behind
a clean interface, so micro-Max could still be swapped in.

### Four units, not three

The state machine was split from `chess_ui.c` into `chess_game.c`. Beside the Jade GUI calls
it would have been untestable off-device; alone it is libc-only and fully exercised on the
host, including a complete 224-move game played through the public API to checkmate.

### Rendering into a raw buffer, not a `Picture`

`chb_render()` takes a `uint16_t*`, not a Jade `Picture`. Since `color_t` is `#define`d to
`uint16_t` (`display.h:8`) this costs nothing at the boundary, and it keeps `chess_board.c`
host-testable — rendered boards are dumped to PPM and inspected by eye, which is the only
way to judge 20px sprites.

### Traps found during implementation

Worth recording; each cost real time to find and would have cost much more to debug on
device.

- **Panel pixels are byte-swapped RGB565.** `display.c` defines `TFT_RED` as `0x00F8`, not
  the textbook `0xF800`. Textbook RGB565 renders with red and blue transposed.
- **`gui_update_picture()` stores the pointer, not a copy** (`node->picture->picture =
  picture`), and the GUI task dereferences it later from the *other core*. A `Picture` local
  to a render function is a use-after-return. This is what `camera.c`'s "stack-allocated
  'pic'" comment is warning about.
- **`max_wait == 0` means "wait forever"** in `sync_wait_event()` (`utils/event.c`), the
  opposite of the usual FreeRTOS convention. `main/smoketest.c` is the precedent for raw
  `GUI_EVENT` handling.
- **Quiescence search must be bounded** (`CH_QUIESCE_MAX`). Unbounded is fine on a host but
  every frame holds a 1KB move array, so on a fixed FreeRTOS stack it is an overflow risk.
  The search stack is sized from depth + `CH_QUIESCE_MAX`.
- **Amalgamation puts 132 `.c` files in one translation unit**, so generic `static` names
  (`apply`, `repaint`, `evaluate`) can collide. Checked; none do.

### Verification status

| Area | Status |
|---|---|
| Move generation | **Proven** — 26/26 perft cases, clean under ASan+UBSan |
| Search, terminal states | **Tested** — mate-in-1, stalemate, draws, 120-ply self-play |
| Rendering, SAN | **Tested** — images inspected; SAN incl. both disambiguation forms |
| State machine | **Tested** — full 224-move game via public API |
| `chess_ui.c` | **Run** — plays real games under libjade, driven via `send_input` / `get_display_bytes` |
| Firmware build | **Builds** — libjade with `--chess`; the qemu/esp32 build previously reached 1377/1383 |
| Physical device | **Not done** — never flashed |
| `format.sh` | **Not run** — clang-format-19 absent; CI may reformat |

### What running it actually caught

Three bugs that no amount of host testing would have found, because the host
suites never compile `chess_ui.c` at all:

1. **`-Wformat-truncation`** on the ring counter. ESP-IDF builds `-Werror=all`;
   the compiler cannot see `CHG_RING_MAX` through the struct and assumes the
   full `int` range. Killed the firmware build at 1377/1383.
2. **Smeared panel text.** `gui_update_text()` clears the old string by
   repainting the node's *parent*, so a text node needs a `FILL` parent to paint
   over. Parented straight to a split, nothing was ever erased and each value
   drew on top of the last. Looks fine until the text changes.
3. **Truncated status strings.** The 160px panel fits ~13 characters and the
   text node truncates silently -- "Checkmate - you lose" rendered as
   "Checkmate - yo". Now pinned by `test_status_strings_fit_panel()`.

Note the ownership asymmetry behind (2): `gui_update_picture()` **aliases** the
pointer it is given, while `gui_update_text()` **mallocs and copies**. Same
shaped API, opposite ownership, and only one of them needs a background to
repaint correctly.

### Driving libjade

libjade exposes `send_input` (`left`/`right`/`click`, straight into
`gui_prev()`/`gui_next()`/`gui_front_click()`) and `get_display_bytes`, so the
whole app can be driven and screenshotted with no hardware:

```python
jade._jadeRpc('libjade_request', {'request': 'send_input', 'event': 'right'})
jade._jadeRpc('libjade_request', {'request': 'get_display_bytes'})
```

Build it with `./libjade/make_libjade.sh Debug --chess --no-ci`, but run
`./tools/switch_to.sh jade --dev --noradio` first: libjade's CMake globs
`managed_components/`, which only exists after `idf.py reconfigure` has fetched
it. `--no-ci` matters too, or the firmware auto-clicks its own buttons.

## Open questions

None blocking. Deferred: search depth (`CHESS_SEARCH_DEPTH`, currently 3 — tune on device),
and whether `[Resign]`/`[Exit]` stay in the `PIECE_SELECT` ring or move to a header button
(the opening ring is 12 entries, which is comfortable, so the ring is fine for now).
