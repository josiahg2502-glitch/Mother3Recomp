#!/usr/bin/env bash
# Regenerate ROM-derived code (Linux/WSL dev helper). Usage: tools/regen.sh [rom] [bios]
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
rom="${1:-$root/roms/mother3_en_v13.gba}"
bios="${2:-$root/gbarecomp/bios/gba_bios.bin}"
tool="${GBA_RECOMPILE:-$root/build/gbarecomp_build/gba_recompile}"
if [ ! -x "$tool" ]; then
  echo "Build the generator first: cmake --build build --target gba_recompile" >&2
  exit 1
fi
sha1_of() {
  if command -v sha1sum >/dev/null 2>&1; then
    sha1sum "$1" | cut -d' ' -f1
  else
    shasum -a 1 "$1" | cut -d' ' -f1
  fi | tr '[:upper:]' '[:lower:]'
}
want_en="306fb8874533b0fd0796208faa8bed2db4ae77fb"
want_hq="4ffe71ce5c5e1e8d9be5fbd09968b60787169b21"
got=$(sha1_of "$rom")
if [ "$got" != "$want_en" ] && [ "$got" != "$want_hq" ]; then
  echo "ROM SHA-1 mismatch: $got (want $want_en or $want_hq)"
  exit 1
fi
if [ -f "$bios" ]; then
  "$tool" --bios "$bios" --config "$root/gbarecomp/bios/gba_bios.toml" --out "$root/gbarecomp/src/runtime/generated_bios"
else
  echo "warning: no BIOS at $bios; skipping BIOS recompilation"
fi
overlay=()
for f in \
  "$root/symbols/mother3_jpn.toml" \
  "$root/symbols/decomp_functions.toml" \
  "$root/symbols/oracle_entries.toml" \
  "$root/symbols/pointer_functions.toml" \
  "$root/symbols/leaf_functions.toml"; do
  [ -f "$f" ] && overlay+=(--config "$f")
done
sym_args=()
[ -f "$root/symbols/imported_symbols.tsv" ] && sym_args+=(--symbols "$root/symbols/imported_symbols.tsv")
[ -f "$root/symbols/imported_data_symbols.tsv" ] && sym_args+=(--data-symbols "$root/symbols/imported_data_symbols.tsv")
"$tool" --rom "$rom" --config "$root/game.toml" "${overlay[@]}" "${sym_args[@]}" --out "$root/generated" --max-functions 65536
