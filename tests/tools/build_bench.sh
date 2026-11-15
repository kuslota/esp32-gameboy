#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
core=$here/../../src/gb_core
out=${1:-$here/bench_core}
shift || true
${CC:-cc} -O2 -g -Wno-unknown-attributes -Wno-unused-label -I"$core" "$@" \
  "$here/bench_core.c" "$core/gb_emulator.c" "$core/vendor/minigb_apu/minigb_apu.c" \
  -DMINIGB_APU_AUDIO_FORMAT_S16SYS -o "$out"
