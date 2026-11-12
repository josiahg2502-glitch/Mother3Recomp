# Building from source

Use CMake, Ninja, a C++20 compiler and SDL2 development libraries. Android also requires Java 17 and the Android SDK/NDK; see the [Android repository](https://github.com/josiahg2502-glitch/Mother3Recomp-Android).

## Current desktop source

On Linux, the existing `tools/build_linux.sh` fetches dependencies and builds the game. For Windows, use MinGW and the repository's CMake targets. On macOS, `tools/build_macos.sh` builds the arm64 or x86_64 app; Homebrew's `sdl2`, `cmake` and `ninja` are the only extra requirements. Recompiler helpers live in `tools/`.

`tools/package_release.py <version> macos-arm64 build/Mother3Recomp release-stage` stages `Mother3Recomp.app` with its SDL, FreeType and HarfBuzz dependencies inside `Contents/Frameworks`, ad-hoc signs it and writes a zip. Use `macos-x86_64` for an Intel bundle; both run on macOS 11 or newer. Saves, settings and the mods folder are written next to the executable inside the bundle, as on the other desktop targets, so the ad-hoc signature stops verifying once the game has run; notarised distribution would need that data moved outside the bundle.

Generated game code comes from your own ROM and BIOS and stays private. Build output, saves and signing keys belong outside Git. By default a native corpus is refused unless the BIOS is recompiled from the same dump, because the exception vectors otherwise start on the interpreter bridge and cost frame pacing until runtime overlays heal them. Generate both with `tools/regen.sh <rom> <bios>` on a machine that has them; the debug loop is exercised by a muted headless run:

```sh
SDL_AUDIODRIVER=dummy ./build/Mother3Recomp --no-launcher --frames 3600 --rom <rom> --bios <bios>
```

The exit report should read `interpreted_insns=0` and `failed=0`, with `healed_native` counting the vectors served from the overlay cache.

## Runtime cleanup in review

[PR #11](https://github.com/josiahg2502-glitch/Mother3Recomp/pull/11) adds pinned dependencies and a verified source-build workflow. On that branch:

```sh
python3 tools/setup_deps.py
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Mother3Recomp
```

Supply a private generated corpus for native game-code execution. That branch uses the compatibility interpreter when the corpus is absent. See the PR for validation results and remaining device checks.
