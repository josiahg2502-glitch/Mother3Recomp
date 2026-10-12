#!/usr/bin/env bash
# ===========================================================================
#  build.sh -- PokéSilver PC : build every target from the one core source
#
#    ./tools/build.sh native    # host shared library  (libpksilver_core.so)
#    ./tools/build.sh wasm      # web/wasm/pksilver_core.wasm  (via zig cc)
#    ./tools/build.sh all       # both
#    ./tools/build.sh test      # native + the 10k-vector CPU regression
#
#  Requirements: gcc or clang for `native`; python3 -m ziglang (pip install
#  ziglang) or a `zig` on PATH for `wasm`.  No other dependencies -- the core
#  is freestanding C.
# ===========================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
CORE="$ROOT/core"
OUT_NATIVE="$ROOT/build"
OUT_WASM="$ROOT/web/wasm"

CC="${CC:-cc}"
OPT="${OPT:--O3}"

zig_cmd() {
    if command -v zig >/dev/null 2>&1; then
        zig "$@"
    elif python3 -m ziglang version >/dev/null 2>&1; then
        python3 -m ziglang "$@"
    else
        echo "error: no zig toolchain.  Try:  pip install --user ziglang" >&2
        exit 1
    fi
}

# Every GB_API function in the public header becomes a wasm export.
#
# NOTE: the name must be taken as "the gb_* identifier immediately before the
# '('".  A regex anchored on the return type misses every pointer-returning
# declaration -- `uint8_t *gb_rom_buffer(void)` has no whitespace between the
# '*' and the name -- which silently dropped gb_rom_buffer, gb_framebuffer and
# the whole jsbind staging API from the wasm module.
collect_exports() {
    tr -d '\n' < "$CORE/gb.h" \
      | grep -o 'GB_API[^;]*' \
      | grep -oE 'gb_[a-z0-9_]+[[:space:]]*\(' \
      | grep -oE 'gb_[a-z0-9_]+' \
      | sort -u
}

build_native() {
    mkdir -p "$OUT_NATIVE"
    local ext="so"
    local out="$OUT_NATIVE/libpksilver_core.so"
    case "$(uname -s)" in
        Darwin) ext="dylib"; out="$OUT_NATIVE/libpksilver_core.dylib" ;;
        MINGW*|MSYS*|CYGWIN*) ext="dll"; out="$OUT_NATIVE/pksilver_core.dll" ;;
    esac
    echo "==> native core -> $out"
    $CC -shared $OPT -std=c11 -fPIC -fvisibility=hidden \
        -I"$CORE" "$CORE"/*.c -o "$out" -lm
    echo "    $(du -h "$out" | cut -f1)"
}

build_wasm() {
    mkdir -p "$OUT_WASM"
    local out="$OUT_WASM/pksilver_core.wasm"
    echo "==> wasm core   -> $out"

    local exports=()
    while read -r name; do exports+=("-Wl,--export=$name"); done < <(collect_exports)
    echo "    exporting ${#exports[@]} symbols"

    zig_cmd cc -target wasm32-freestanding $OPT -std=c11 -DGB_FREESTANDING \
        -fvisibility=hidden -fno-builtin -ffunction-sections \
        -I"$CORE" "$CORE"/*.c \
        -Wl,--no-entry -Wl,--gc-sections \
        "${exports[@]}" \
        -o "$out"
    echo "    $(du -h "$out" | cut -f1)"
}

run_tests() {
    build_native
    echo "==> cpu vectors (tests/vectors.bin)"
    python3 "$ROOT/tests/cpu_test.py" --lib "$OUT_NATIVE/libpksilver_core.so"
}

case "${1:-all}" in
    native) build_native ;;
    wasm)   build_wasm ;;
    all)    build_native; build_wasm ;;
    test)   run_tests ;;
    *)      echo "usage: $0 {native|wasm|all|test}" >&2; exit 1 ;;
esac
