#!/bin/bash
# Host-side test suite for the chess app.
#
#     ./main/chess/test/run_tests.sh [--asan]
#
# Runs everything that does not need a device, ESP-IDF or docker:
#   - perft, search, render and game-state suites
#   - a warning check of every chess source using ESP-IDF's own flags
#
# The warning check matters: engine.c, chess_board.c and chess_game.c are
# exercised by the suites, but chess_ui.c is not compiled by them at all, and
# ESP-IDF builds with -Werror=all. A -Wformat-truncation error in chess_ui.c
# once got all the way to a docker firmware build before being noticed, because
# a plain -fsyntax-only check does not run the optimiser passes that warning
# depends on. -Os here does.

set -eo pipefail
cd "$(dirname "$0")/../../.."

CHESS=main/chess
SRCS="$CHESS/chess_game.c $CHESS/chess_board.c $CHESS/engine.c"
OUT="${TMPDIR:-/tmp}/chess_tests.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

SAN=""
if [ "$1" = "--asan" ]; then
    SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g"
    export ASAN_OPTIONS=symbolize=1
    export UBSAN_OPTIONS=print_stacktrace=1
    echo "(sanitizer build)"
else
    SAN="-O2"
fi

# ESP-IDF's warning flags for main/, taken from a real build command line.
# -Os is required: it is what enables the format-truncation analysis.
IDF_WARNINGS=(-Wall -Werror=all -Wextra -Wno-error=extra -Wno-unused-parameter
              -Wno-sign-compare -Wno-enum-conversion -Os -Wwrite-strings
              -fstack-protector-strong -std=gnu17 -Wno-old-style-declaration
              -Werror=unused-result)

echo
echo "== ESP-IDF warning check =="
for f in engine chess_board chess_game; do
    cc "${IDF_WARNINGS[@]}" -c -o /dev/null -I"$CHESS" "$CHESS/$f.c"
    echo "  $f.c: clean"
done
# chess_ui.c needs the firmware headers, so it is only checked in a real build.
echo "  chess_ui.c: SKIPPED (needs ESP-IDF headers; checked by the firmware build)"

echo
echo "== test suites =="
failed=0
for t in perft_test search_test render_test game_test engine_tt_test; do
    # shellcheck disable=SC2086
    cc $SAN -Wall -Wextra -Werror -o "$OUT/$t" "$CHESS/test/$t.c" $SRCS -I"$CHESS"
    if (cd "$OUT" && "./$t" . >"$OUT/$t.log" 2>&1); then
        echo "  $t: $(grep -E 'all .* passed' "$OUT/$t.log" || echo passed)"
    else
        echo "  $t: FAILED"
        tail -20 "$OUT/$t.log" | sed 's/^/      /'
        failed=1
    fi
done

echo
if [ "$failed" -ne 0 ]; then
    echo "FAILURES"
    exit 1
fi
echo "all chess tests passed"
