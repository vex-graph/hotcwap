#!/bin/sh
# run.sh — the full demo.
#
#   ./run.sh          fresh install tree
#   ./run.sh --keep   keep the existing tree (see the "not first run" path)
#
# What to watch, and who decides it:
#
#   boot        setup.c calls Hot_loadCore / Hot_loadApplication / Hot_loadPlugin
#               → core v1 (contract 1) + app v1 (wants 1) + plugin, all running
#
#   tick 60     core v2 is promoted. The library re-reads the contract and sees
#               the running app wants 1 while core now publishes 2 → the app
#               goes idle on its own. Nothing in setup.c gated anything.
#
#   tick 90     app v2 (wants 2) is promoted → the gate passes, the app comes
#               back, and its frame counter continues where it stopped.
#
#   tick 130    app v3 (wants contract 99) is promoted → the gate refuses it and
#               the app goes idle again, still holding its state.
#
#   throughout  the plugin keeps ticking across every core and app swap: it
#               declares no contract, so nothing gates it.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ "${1:-}" != "--keep" ]; then
    echo "wiping install tree: $here/install"
    rm -rf "$here/install"
fi

"$here/build.sh"
echo
echo "=============================================================="
echo " core v1 + app v1 + plugin running"
echo " core v2 at t60, app v2 at t90, incompatible app v3 at t130"
echo "=============================================================="
echo

cd "$here"
exec ./build/setup \
    "$here/build/core/v2" \
    "$here/build/app/v2" \
    "$here/build/app/v3" \
    "$here/build/core/v1" \
    "$here/build/app/v1"
