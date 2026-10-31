// True widescreen for the field: the PPU asks for tilemap entries outside the
// native 240px, and we answer from the game's own decompressed map in EWRAM at
// the live camera position (the BG ring itself is only 256px wide). Screens
// that don't validate as the field (menus, battles, cutscenes with other BG
// setups) are pillarboxed instead.
#pragma once

namespace m3widescreen {

void install();  // registers the PPU margin providers (harmless at 240px); tick() calls it
void tick();     // game thread; re-validates the field a few times a second
void state_loaded();  // after a state load (suspend resume, slot): validate now

}  // namespace m3widescreen
