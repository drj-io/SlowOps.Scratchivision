#!/usr/bin/env bash
# Package a release for the open-mpc catalog (https://open-mpc.github.io/workflow.html) from the SDK plugin folder:
#   dist/Scratchivision-<version>-mpc-armv7.zip    Gen1 MPCs and Force (Plugin Manager, installers)
#   dist/Scratchivision-<version>-mpc-aarch64.zip  Gen2 MPCs
#   dist/Scratchivision-<version>-mpc-sdk.zip      one folder for every MPC, copied into "VST2 Plugins" by hand
# It runs the catalog's own tools from a checkout of open-mpc/mpc-vst-plugins (MPC_CATALOG, default ../mpc-vst-plugins),
# and stops if either per-build zip fails the catalog check. Attach all three zips to the GitHub release.
#   scripts/release.sh [SDK folder, or a zip of it]   default: the folder scripts/build.sh makes (run scripts/test.sh first)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CATALOG="${MPC_CATALOG:-$ROOT/../mpc-vst-plugins}"
PY="${PYTHON:-python3}"
ID=scratchivision
REPO=drj-io/SlowOps.Scratchivision
cd "$ROOT"
SRC="${1:-plugin/build/Slow Ops - VST - Scratchivision}"
[ -f "$CATALOG/tools/sdk_release.py" ] || { echo "release: no catalog tools in $CATALOG (clone open-mpc/mpc-vst-plugins there, or set MPC_CATALOG)" >&2; exit 1; }
[ -e "$SRC" ] || { echo "release: $SRC not found (run scripts/build.sh)" >&2; exit 1; }
VERSION="$("$PY" -c 'import json; print(json.load(open("plugin/plugin.json"))["version"])')"
STEM="dist/Scratchivision-$VERSION-mpc"
rm -f "$STEM-armv7.zip" "$STEM-aarch64.zip" "$STEM-sdk.zip"
"$PY" "$CATALOG/tools/sdk_release.py" "$SRC" --version "$VERSION" --id "$ID" --repo "$REPO" --license MIT -o dist
for arch in armv7 aarch64; do
    "$PY" "$CATALOG/tools/catalog_check.py" "$STEM-$arch.zip" --catalog --require-portable --expect-id "$ID" --expect-repo "$REPO"
done
"$PY" "$CATALOG/tools/sdk_zip.py" "$STEM-sdk.zip" "$STEM-armv7.zip" "$STEM-aarch64.zip"
ls -l "$STEM"-*.zip
