# MOTHER 3 Native PC Recompilation — Credits & Acknowledgments

This project is made possible through the incredible work of developers, translators, sound designers, and open-source contributors across the gaming and preservation communities.

---

## 1. Original Game & Intellectual Property
* **MOTHER 3** © 2006 Nintendo / SHIGESATO ITOI / Brownie Brown / HAL Laboratory, Inc.
* **Creator & Director**: Shigesato Itoi
* **Music & Composition**: Shogo Sakai
* *Please support official releases and merchandise at [nintendo.com](https://www.nintendo.com).*

---

## 2. English Fan Translation Team
* **Official Website**: [mother3.fobby.net](http://mother3.fobby.net/)
* **Lead Translator**: Clyde "Tomato" Mandelin
* **Lead Programmer**: Jeffman
* **Project Coordinator**: Reid Young
* **Contributors**: The entire MOTHER 3 Fan Translation Team and EarthBound Central / Starmen.net community for years of dedication to translating and polishing the v1.3 English release.

---

## 3. High-Quality 16-Bit Sound Mixing Mod
* **Romhacking.net Entry**: [Romhacking.net #6951](https://www.romhacking.net/hacks/6951/)
* **GitHub Repository**: [MusicTheorist/MOTHER-3-HQ-Audio](https://github.com/MusicTheorist/MOTHER-3-HQ-Audio)
* **Mod Author & Project Lead**: **Summer Dragonfly** (@MusicTheorist) — ROM hacking, memory re-architecting, xkas integration, and patch tools.
* **HQ Sound Mixer Programming**: **ipatix** ([github.com/ipatix](https://github.com/ipatix)) — 16-bit sound mixing engine replacing the default 8-bit m4a driver.
* **Original 16-Bit Mixing Concept**: **Haruki "Bon" Kodera** for Camelot Software Planning (*Golden Sun* series).
* **Decompilation & Technical Support**: **Kurausukun** and **theo3** ([github.com/Kurausukun/mother3](https://github.com/Kurausukun/mother3)).
* **Additional Technical Assistance**: **phoenixbound**, **Lorenzooone**, and **Yosuke**.
* **Patcher Specifications & Tools**: **Near / byuu** for the `xkas` assembler format and the `UPS` patch specification.

---

## 4. Recompilation Technology & Launcher Framework
* **Core Recompiler & Hardware Runtime**: **Matthew Stanley** (@mstan)
  * **`gbarecomp`**: [github.com/mstan/gbarecomp](https://github.com/mstan/gbarecomp) — Static AOT recompilation engine for Game Boy Advance ARMv4T/Thumb code into native C++ instructions.
  * **`recomp-ui`**: [github.com/mstan/recomp-ui](https://github.com/mstan/recomp-ui) — Cross-platform launcher, on-the-fly ROM patcher (IPS, BPS, UPS), and in-game menu framework.
  * Licensed under PolyForm Noncommercial License 1.0.0.

---

## 5. Third-Party Libraries & Runtimes
* **SDL2 (Simple DirectMedia Layer)**: Sam Lantinga and the SDL development community ([libsdl.org](https://www.libsdl.org/)). Licensed under the zlib License. Powers low-latency audio mixing, XInput/DirectInput gamepad support, and display presentation.
* **Dear ImGui**: Omar Cornut (@ocornut) ([github.com/ocornut/imgui](https://github.com/ocornut/imgui)). Licensed under the MIT License. Powers the interactive launcher and in-game overlay menus.
* **TinyCC (TCC)**: Fabrice Bellard and contributors ([bellard.org/tcc](https://bellard.org/tcc/)). Licensed under GNU LGPL 2.1. Powers dynamic fallback JIT compilation for guest code.
* **MinGW-w64**: The MinGW-w64 Project ([mingw-w64.org](https://www.mingw-w64.org/)). Provides the GCC toolchain and Windows CRT libraries (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`).
* **DirectX / Direct3D 11**: Microsoft Corporation.

---

## 6. Android & Linux Ports
* **Android Port Lead**: **YlPorts** ([github.com/YlPorts/Mother3Recomp-Android](https://github.com/YlPorts/Mother3Recomp-Android)) — Android NDK port, Gradle packaging, JNI lifecycle bindings, SDLActivity integration, and the `Mother3SetupActivity` launcher.
* **Linux Port & Cross-Platform Support**: Built on the native Linux runtime architecture of `gbarecomp` by **Matthew Stanley** and the **RetroPortingToolKit** open-source community.
* **Official Forks**:
  - Android Fork: [josiahg2502-glitch/Mother3Recomp-Android](https://github.com/josiahg2502-glitch/Mother3Recomp-Android)
  - Linux Fork: [josiahg2502-glitch/Mother3Recomp-Linux](https://github.com/josiahg2502-glitch/Mother3Recomp-Linux)

---

## 7. Port Integration, Packaging & Visuals
* **In-game bezel**: Composed at first launch from the player's own ROM (the Tazmily crossroads map, a menu window in the chosen flavor, and the Lucas, Kumatora, Duster and Boney sprites). No artwork ships with the download.
* **Interposer Runtime (`mother3_bezel.dll`)**: Custom SDL Dynamic API hook for seamless 1080p bezel composition, 3:2 aspect integer scaling, F10 bezel toggle (Clean Fullscreen mode), and PC hotkeys (`F11` overlay menu, `Esc` exit, `F1`/`F2` save states).
* **Multi-Resolution Application Icon**: Custom embedded `Mother 3.ico` icon packaging official logo artwork.
* **Android runtime menu font**: [Pixelify Sans](https://github.com/eifetx/Pixelify-Sans) by the Pixelify Sans Project Authors, under the SIL Open Font License 1.1 (`android/app/src/main/res/raw/pixelify_sans_ofl.txt`). The menu's "mother3" theme follows the game's windows in colours only.
* **Automated One-Click Mod Patcher**: PowerShell/Batch script for instant zero-dependency ROM patching.

---

*This project is an unofficial, non-commercial educational and preservation effort. No copyrighted Nintendo ROMs, BIOS dumps, or commercial game audio files are bundled or distributed.*

