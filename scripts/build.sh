#!/usr/bin/env bash
# Build the MPC plugin folder (native + arm32 + arm64 + skins + validation) and the test tools.
#   1. ./wav -> plugin/Content/Samples + plugin/src/generated/samples.hpp
#   2. native build -> plugin/build/params.json (parameter indices for the skin)
#   3. plugin/skins/TUI.json + Q-Links.json from params.json (the PNGs are committed;
#      regenerate them with scripts/make_skin_art.py when the look or the samples change)
#   4. full build (native, arm32, arm64), skins copied verbatim, SDK validation
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/third_party/mpc-plugin-builder-sdk"
PY="${PYTHON:-python3}"
cd "$ROOT"
[ -f "$SDK/tools/build.py" ] || git submodule update --init third_party/mpc-plugin-builder-sdk
"$PY" scripts/gen_samples.py
"$PY" "$SDK/tools/build.py" plugin --target native >/dev/null
"$PY" scripts/gen_skins.py
"$PY" "$SDK/tools/build.py" plugin "$@"
mkdir -p build
c++ -std=c++17 -O2 -Wall -Wextra -I core/include -DFIXTURES="\"$ROOT/tests/fixtures\"" tests/core_tests.cpp -o build/core_tests
c++ -std=c++17 -O2 -Wall -I "$SDK/framework" -I core/include -DFIXTURES="\"$ROOT/tests/fixtures\"" tests/scenario_host.cpp -o build/scenario_host
echo
echo "MPC folder: $ROOT/plugin/build/Slow Ops - VST - Scratchivision"
