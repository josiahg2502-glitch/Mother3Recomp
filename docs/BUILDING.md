# Building from source

Use CMake, Ninja, a C++20 compiler and SDL2 development libraries. Android also requires Java 17 and the Android SDK/NDK; see the [Android repository](https://github.com/josiahg2502-glitch/Mother3Recomp-Android).

## Current desktop source

On Linux, the existing `tools/build_linux.sh` fetches dependencies and builds the game. For Windows, use MinGW and the repository's CMake targets. Recompiler helpers live in `tools/`.

Generated game code comes from your own ROM and BIOS and stays private. Build output, saves and signing keys belong outside Git.

## Runtime cleanup in review

[PR #11](https://github.com/josiahg2502-glitch/Mother3Recomp/pull/11) adds pinned dependencies and a verified source-build workflow. On that branch:

```sh
python3 tools/setup_deps.py
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Mother3Recomp
```

Supply a private generated corpus for native game-code execution. That branch uses the compatibility interpreter when the corpus is absent. See the PR for validation results and remaining device checks.
