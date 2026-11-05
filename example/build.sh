#!/bin/sh
# build.sh — compile the demo host and its images.
#
#   ./build.sh
#
# Produces:
#   build/setup          the frozen process (links hotcwap, NOT the images)
#   build/core/v1|v2     engine images, contracts 1 and 2
#   build/app/v1|v2      application images, wanting contracts 1 and 2
#   build/app/v3         application image wanting contract 99 (impossible)
#   build/plugin/v1      an independent add-on image
#
# hotcwap is a LIBRARY here: setup.c calls Hot_loadCore / Hot_loadApplication /
# Hot_loadPlugin and owns none of the ordering or gating.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ws="$here/../../.."          # worktree/

CC=${CC:-clang}
STD="-std=gnu23 -Wall -Wextra -g -O0"
INC="-I$ws/vex -I$ws/vex/hotcwap -I$ws/vex/hotcwap/hot -I$ws/vex/lifetime -I$ws/vex/vexspoke -I$ws/vex/re -I$ws/vex/tests -I$here"

# hotcwap's loader units. The model is split one class per file under hot/.
# throwable.c is deliberately absent: the loader never calls it, and it drags
# ChunkedList + Exception + Window into the link.
HOT="$ws/vex/hotcwap/hot/module.c \
     $ws/vex/hotcwap/hot/module_internal.c \
     $ws/vex/hotcwap/hot/save_slot.c \
     $ws/vex/hotcwap/hot/staged_section.c \
     $ws/vex/hotcwap/hot/stack.c \
     $ws/vex/hotcwap/hot/trampoline_table.c \
     $ws/vex/hotcwap/hot/retire_ring.c \
     $ws/vex/hotcwap/manifest.c \
     $ws/vex/hotcwap/ledger.c"

# hotcwap hashes cold payloads with SHA-256 — its one real R2 dependency.
CRYPTO="$ws/vex/vexspoke/security/crypto.c"

for d in core/v1 core/v2 app/v1 app/v2 app/v3 plugin/v1; do
    mkdir -p "$here/build/$d"
done

echo "== setup (the frozen host) =="
# shellcheck disable=SC2086
$CC $STD $INC "$here/setup.c" $HOT $CRYPTO -o "$here/build/setup" -lpthread

image() { # <out-dir> <source> [define]
    $CC $STD $INC ${3:+"-D$3"} -dynamiclib -fPIC -undefined dynamic_lookup \
        "$here/$2" -o "$here/build/$1/${1%%/*}.dylib"
}

echo "== core v1 (contract 1) =="
image core/v1 core.c CORE_CONTRACT_ABI=1
echo "== core v2 (contract 2) =="
image core/v2 core.c CORE_CONTRACT_ABI=2
echo "== app v1 (from app_v1.c, wants 1) =="
image app/v1 app_v1.c
echo "== app v2 (from app_v2.c, wants 2) =="
image app/v2 app_v2.c
echo "== app v3 (from app_v3.c, wants 99) =="
image app/v3 app_v3.c
echo "== plugin v1 (independent) =="
image plugin/v1 plugin.c

# A payload directory must contain ONLY real sections: a .dSYM beside the dylib
# would be recorded as a section in manifest.json on first-run reflect.
find "$here/build" -maxdepth 3 -name '*.dSYM' -exec rm -rf {} + 2>/dev/null || true

echo
echo "built:"
find "$here/build" -maxdepth 3 -name '*.dylib' -o -maxdepth 1 -name setup | sort
