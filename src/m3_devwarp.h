// Testing aid: M3_WARP=<map id> sends the party to that map's first entrance
// the next time it takes a step in the field, through the game's own door
// transition. Unset, the hook stays disabled.
#pragma once

namespace m3devwarp {

void tick();  // game thread, each vblank

}  // namespace m3devwarp
