#!/usr/bin/env bash
# Native macOS Build Script for MOTHER 3 Recomp
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$DIR"

echo "=== MOTHER 3 Native Recompilation - macOS Build ==="
python3 tools/setup_deps.py

if [ ! -f "generated/dispatch_table.cpp" ]; then
    echo "Notice: generated/ has no recompiled sources; the game runs on the"
    echo "compatibility interpreter. Populate it with ./tools/regen.sh <rom> <bios>"
fi

echo "Configuring CMake build..."
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Mother3Recomp -j"$(sysctl -n hw.ncpu)"

echo "Built $DIR/build/Mother3Recomp"
case "$(uname -m)" in
    arm64) platform=macos-arm64 ;;
    x86_64) platform=macos-x86_64 ;;
    *) echo "Unsupported macOS architecture: $(uname -m)" >&2; exit 1 ;;
esac
echo "Stage the app with: python3 tools/package_release.py $(cat version.txt) $platform build/Mother3Recomp release-stage"
