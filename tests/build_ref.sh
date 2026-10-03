#!/usr/bin/env bash
# Build the reference emulator used by tests/compare.py: libElysianVMU (MIT) plus tests/ref_trace.c.
#   bash tests/build_ref.sh [folder for the libevmu checkout, default ../libevmu]
# Needs git, cmake and a C compiler. Takes a few minutes the first time.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REF="${1:-$HERE/../../libevmu}"
COMMIT=fb041a85c742b46b25995b06b27d0c03dfc534f1        # the version the comparison was written against
if [ ! -d "$REF/.git" ]; then
  git clone https://github.com/gyrovorbis/libevmu "$REF"
  git -C "$REF" checkout -q "$COMMIT"
  git -C "$REF" submodule update --init --recursive
fi
# One deliberate fix: libevmu leaves the parity flag stale after ADD/ADDC/SUB/SUBC (it writes the
# accumulator before the status word). On the real chip parity always follows the accumulator.
git -C "$REF" apply --check "$HERE/libevmu-parity.patch" 2>/dev/null && git -C "$REF" apply "$HERE/libevmu-parity.patch"
mkdir -p "$REF/build"
( cd "$REF/build" && cmake .. -DCMAKE_BUILD_TYPE=Release -DEVMU_RESULT_ERROR_ASSERT=OFF >/dev/null && make -j"$(nproc)" 2>&1 | grep -E "error|Built target" | tail -3 )
B="$REF/build"
FLAGS="$(find "$B" -name flags.make -path '*LibElysianVMU*' | head -1)"
DEFS="$(grep '^C_DEFINES' "$FLAGS" | sed 's/^C_DEFINES = //')"
INCS="$(grep '^C_INCLUDES' "$FLAGS" | sed 's/^C_INCLUDES = //')"
LIBS="$B/lib/liblibLibElysianVMU.a $B/lib/lib/libgimbal/lib/liblibGimbal.a $B/lib/lib/libgimbal/lib/lib/lz4/build/cmake/liblz4.a $B/lib/lib/libgimbal/lib/lib/tiny-regex-c/libTinyRegexC.a $B/lib/lib/libgimbal/lib/lib/tinycthread/libtinycthread.a -lm -lpthread"
eval gcc -std=gnu17 -O2 -w $DEFS $INCS "$HERE/ref_trace.c" $LIBS -o "$HERE/ref_trace"
echo "built $HERE/ref_trace"
