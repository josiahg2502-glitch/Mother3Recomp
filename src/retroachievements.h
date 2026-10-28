// retroachievements.h — RetroAchievements (rcheevos rc_client) for the
// Android build.
//
// Softcore only: RetroAchievements awards hardcore unlocks to approved
// emulators, and a static recompilation is not one, so this client never
// requests hardcore and leaves save states, suspend/resume and fast-forward
// untouched.
//
// Credentials come from the setup screen (Mother3SetupActivity), which logs
// in over HTTPS and writes `retroachievements.ini` (user + API token, never
// the password) into app-private storage. Without that file nothing here
// runs and no network request is made.
#pragma once

namespace gbarecomp { struct RunOptions; }

namespace mother3 {

// Hooks the achievement runtime into the game's per-frame, overlay and
// savestate seams (partial progress is kept in "<state>.ra" beside each
// save state and the suspend state).
// `rom_path` is the imported ROM, relative to app-private storage; it is
// hashed (MD5 of the whole file, the RetroAchievements GBA method) to
// identify the game.
void ra_install(gbarecomp::RunOptions& opts, const char* rom_path);

// Stops the network worker and releases the client once run_game() returns,
// so a relaunch in the same process starts from a clean state.
void ra_shutdown();

}  // namespace mother3
