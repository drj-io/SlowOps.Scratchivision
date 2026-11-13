#!/usr/bin/env bash
# Full verification: build, core unit tests, SDK host simulator, scenario host (real binary,
# generated control-vinyl input). Writes plugin/build/baby-scratch-{in,out}.wav for listening.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/third_party/mpc-plugin-builder-sdk"
LIB="$ROOT/plugin/build/native/scratchivision.dylib"
[ "$(uname)" = "Darwin" ] || LIB="${LIB%.dylib}.so"
cd "$ROOT"
"$ROOT/scripts/build.sh"
echo; echo "== core tests (decoder, player, engine)"; ./build/core_tests
echo; echo "== SDK host simulator (noise input: silence is expected, it is not timecode)"
"$SDK/.sdk-tools/host_sim" "$LIB" test --seconds 4 --wav plugin/build/hostsim.wav
echo; echo "== scenario host (real plugin binary, generated timecode)"
./build/scenario_host "$LIB" plugin/build
echo; echo "ALL CHECKS PASSED"
