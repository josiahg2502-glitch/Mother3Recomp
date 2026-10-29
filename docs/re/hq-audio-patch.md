# MOTHER 3 HQ Audio patch vs the engine's MP2K shadow mixer

Status: **shadow mixer adopted** (2026-10-15), EN 1.3 only, after engine
patches 0012 (driver tick), 0013 (output level), 0014 (gain glide), 0015
(catch-up samples: the audible crackle) and 0016 (PSG low-pass: the
micro-crackle). It is switched in
the runtime menu (Audio > Enhanced audio, engine patch 0011), and on for the
author's RG556.

## Question

Can the ROM patched with MusicTheorist's
[MOTHER 3 HQ Audio](https://github.com/MusicTheorist/MOTHER-3-HQ-Audio) be
used with this port and keep RetroAchievements?

## RetroAchievements

`dorequest.php?r=gameid&m=<md5>` answers, as of 2026-10-15:

| ROM | MD5 | GameID |
|---|---|---|
| Mother 3 (Japan) | af8b0b175f7ec8914cb87b3161ba1aaa | 677 |
| EN 1.3 (`variants/mother3en`) | 78852690de6ef51af56883c3823ea0e1 | 677 |
| EN 1.3 + HQ Audio | 79827109fa1af4abbe25848657648c55 | 0 (not linked) |

Only RetroAchievements developers can link a hash, usually after a request in
the game's official forum topic with the patch link and the MD5. They have to
check that the achievements still read the right addresses. The patch moves
game buffers (below), so the request is not a formality. The author also says
it changes the RNG.

## What the patch changes (EN 1.3 vs EN 1.3 + HQ Audio)

`cmp` finds 1975 differing bytes:

| Location | Before | After | Reading |
|---|---|---|---|
| 0x0808F9EC (literal pool of the SoundMainRAM copy) | src 0x0808ED81, dst 0x03000000, CpuSet 0x04000100 (256 words) | src 0x09B0C141, dst 0x03001C00, CpuSet 0x0400021F (543 words, 2172 bytes) | new mixer, copied to a different IWRAM address |
| 0x0808ED70 (SoundMain literal) | 0x03000001 | 0x03001C01 | SoundMain calls the moved mixer |
| 0x0808F9F8 | 0x0300201B | 0x03000C50 | another IWRAM pointer moved (hypothesis: mixer work area) |
| 0x08069314, 0x08069434 | 0x02001084 | 0x02005230 | EWRAM pointer moved (purpose unknown) |
| 0x08120E20..0x08120E8F | table of (IWRAM, IWRAM buffer, count) rows | buffers moved within IWRAM and some to EWRAM 0x020010xx-0x020016xx | hypothesis: memory pools relocated to free IWRAM for the mixer |
| 0x09B0C140..0x09B0C9BB | free space | 2172 bytes | the new mixer (16-bit per-channel mix, then 8-bit) |

## Consequences for the port

- `tools/build-android.sh` checks the ROM SHA1 and `variants/mother3en/game.toml`
  pins it, so the build refuses the patched ROM.
- The IWRAM code map in `game.toml` is hand-written: `SoundMainRAM_Buffer`
  is declared at 0x03000000 as a copy of ROM 0x0808ED81 (932 bytes). With the
  patch the mixer runs at 0x03001C00, from 0x09B0C141, 2172 bytes. It does not
  overlap the next declared copy (0x03002EF0).
- Supporting it means a new variant (new SHA1, new code map, recompiled
  mixer) and a full test. That is possible but not a file swap, and it loses
  RetroAchievements until the hash is linked.

## Alternative: the engine's MP2K shadow mixer

gbarecomp already has a "verified-enhancement" shadow mixer for the stock
MP2K driver (`gbarecomp/src/gba/mp2k_shadow.*`, `audio_shadow.*`, ported from
JRickey/gba-recomp):

- The recompiled driver still runs. The shadow re-renders the same voices in
  float at 65536 Hz, without the 8-bit requantization that causes the hiss.
  This is the same problem the HQ patch addresses, and the patch still ends
  at 8 bits.
- A verifier compares it with the hardware mix every sample. The shadow only
  substitutes after a proven window, and logs `[audio] MP2K shadow DEGRADED:
  <reason>` and reverts on divergence.
- It is off by default (`[audio] shadow`, `gbarecomp/src/runtime/runtime.cpp`).
  `GBARECOMP_AUDIO_SHADOW=1` turns it on at launch, `0` forces it off.
- When armed, the log shows `[audio] MP2K shadow mixer ARMED`.
- Its loudness constant (`kShadowOutputScale`, `gba_audio.cpp`) is marked as
  still to be tuned.
- It keeps the RetroAchievements-linked ROM.

Engine patch 0011 adds a runtime-menu row, Audio > **Enhanced audio**:

- It appears only when the ROM links MP2K, and switches the shadow at runtime.
- The choice is saved in `config.ini` `[Audio] enhanced` and wins over
  `[audio] shadow`.
- `GBARECOMP_AUDIO_SHADOW` (for example via `debug-env.txt`) still overrides
  both.

## Result on the RG556 (2026-10-15)

- The player found the audio better, in overworld and in a battle.
- First session, about 10 minutes, 35843 frames, including a battle:
  - Two `DEGRADED` reverts, both within the first ~30 s after resuming a
    suspend state: `correlation 0.07, level 1.21` and `0.19, 0.92`. There
    were none after that.
  - Hypothesis: the shadow starts from the just-loaded state before it is in
    step with the driver.
  - The engine does not log when the shadow goes live, so "live for the rest
    of the session" is inferred: the verifier needed 4 passing ~1 s windows
    after the second revert, and nothing reverted again.
- That session ran before engine patch 0012. As the A/B below shows, the
  shadow was then live for little or none of the time, so these figures say
  little about the shadow itself.
- Cost: the emulation thread used about 58% of one core. Health was
  `late_frames=65 hitches=0 max_gap_ms=33.6 audio_dry_ms=0.0`; a 41939-frame
  session without the shadow had 44 late frames.

## Engine bug: the shadow polled instead of following the driver tick (2026-10-15)

`Mp2kShadow::frame_hook()` must run once per driver tick, at the mixer's entry
(SoundMainRAM). At that point SoundInfo is in flight ('Smsh'+1) and the
note-on flags have not been consumed yet. JRickey/gba-recomp calls it there,
and `init()` records the SoundMainRAM entries as hook keys for that reason.

gbarecomp's port never used the keys. `GbaAudio` polled every
`sample_rate()/60` grid samples (1092, against about 1097 per frame), so the
poll drifted across the frame. Most polls found SoundInfo idle and were
dropped as stale.

Engine patch 0012 adds a read-only dispatch observer for the SoundMainRAM
entries. In MOTHER 3, SoundMain reaches 0x03000001 with `bx r3`, which the
generated code sends through tail dispatch. The patch also adds a summary
line, logged when the session is suspended or ends.

A/B on the RG556, from the same suspend state (frame 93892, overworld
music), 95 s each:

| Build | live | driver ticks | stale | reverts | late frames |
|---|---|---|---|---|---|
| fixed-rate poll (before) | **0.0%** | 5700 | 5604 | 0 | 1 |
| driver-tick observer (0012) | **95.8%** | 5672 | 0 | 0 | 0 |

The first ~4 s are the verifier's probation. So before 0012 the player was
hearing the original mix almost all the time, and "Enhanced audio" sounded
better than it was.

## Engine bug: the shadow played about 12 dB low (2026-10-15)

With the shadow live (after 0012), the player heard crackle in the
overworld music. A temporary raw dump of the output, 90 s from the same
overworld scene, showed the cause.

While the shadow substitutes, its mix replaces only the Direct Sound part of
each sample, through a fixed `kShadowOutputScale` of 24000 that the code
marks as a knob still to tune. The canon path maps a full-scale DAC on both
FIFOs to 24576, the shadow's full scale to 6000. The PSG channels (squares
and noise) stay at their normal level, so they stood out.

Engine patch 0013 routes and scales the shadow's right/left like the FIFO
A/B bytes: SOUNDCNT_H volume, L/R enables, averaged, x48.

| | music level vs canon | music over PSG | shadow peak | >8 kHz energy |
|---|---|---|---|---|
| canon (hardware mix) | 1.00 | 15.9 dB | 17184 | 0.82% |
| shadow before 0013 | 0.24 (-12.6 dB) | 3.4 dB | 4321 | 0.03% |
| shadow with 0013 | 0.97 | 15.6 dB | 18099 | 0.03% |

The shadow stream itself had no clipping and no discontinuities. Its few
jumps over 3000 fall about every 1.8 s, on the music's beat: they are
percussion attacks, and the canon has 20 times more. The >8 kHz figure is
the 8-bit hiss the shadow removes.

## Engine bug: gain steps clicked in the clean shadow (2026-10-15)

At the right level (0013), the player still heard a raspy crackle in the
overworld music, and only with the shadow on (checked by toggling it in the
menu). Two diagnostic runs located it: an output dump with the driver ticks
marked, and a per-tick voice event log.

- The shadow's second difference at the first sample of each tick was 3.5
  times the average elsewhere.
- 212 ticks in about 44 s of live audio had an output jump over 200 (of
  32767), up to 3806.
- 108 of the 151 ticks with a gain step on a continuing voice had one.
  These are notes just released (status 0x51/0x52), with steps of up to
  15% of full scale. Most of the rest were voices cut at the tick.
- The spectrogram shows them as thin vertical (broadband) lines.

The hardware mix has the same steps, but its 8-bit noise masks them.

Engine patch 0014:
- Each voice's output gain glides toward the tick's target (one pole,
  about 0.3 ms).
- Voices the driver ends fade out over a few milliseconds instead of
  stopping at once.
- The verifier's check copy keeps the driver's stepped gains.

Same title-screen capture from a cold boot, before and after:

| | tick d2 vs average | tick jumps p90 | max | ticks with a jump over 200 |
|---|---|---|---|---|
| 0013 | 3.5x | 150 | 3806 | 212 |
| 0014 | 0.9x | 74 (random positions: 77) | 1047 | 33 |

The level stayed at 0.97 of the canon. The remaining large sample jumps sit
in one second of percussive title sound, where the canon peaks too.

## Engine bug: catch-up samples bypassed the shadow (2026-10-15) — the crackle

After 0014 the player still heard the crackle, and only with the shadow on.
WAV files made from the engine's output sounded clean on the Mac. A
capture of the stream handed to the audio bridge (`push_audio_samples`,
with the bridge output alongside) crackled on the Mac too, from 13 s on.
There, 1-3 sample spikes appeared, often as repeated values (for example
`-4016, 6768, 6768, -4051`), up to 698 a second.

The cause:
- `GbaAudio` produces samples in `run_sample_event()` and in
  `sample_until_current_time()`. The second mixes the event's slots up to
  "now" before every sound register write.
- Only the first knew about the shadow. The catch-up mixed the canon FIFO
  instead.
- The shadow runs about one frame ahead of the canon, so each register
  write inserted canon samples from elsewhere in the waveform.
- The earlier dumps sampled only `run_sample_event()`'s loop, which is why
  they looked clean.

Engine patch 0015 routes both through `mix_slot()`.

Bridge-input capture, 45 s each, overworld:

| | spikes | worst second | repeated-value jumps |
|---|---|---|---|
| before 0015 | 6192 | 698 | 381 |
| after 0015 | 189 | 52 | 21 |

The remaining ones are clean level steps in single seconds, the shape of
PSG square edges in sound effects. They are the same with the shadow off.

Lessons:
- 0013's level fix was real.
- 0014's gain glide removes real tick-boundary steps, but it was not what
  the player heard.
- An experiment that opened the Android device at 48000 Hz, so that RAB
  converts instead of the system mixer, only changed the crackle's
  character. It was dropped.
- Capture as close to the device as possible before concluding that the
  output is clean.

## Micro-crackle: hard PSG edges next to the clean shadow (2026-10-15)

After 0015 the player still heard occasional micro-crackle in the same
places. Two 45 s captures were taken from the same suspend state, with every
output slot split into its PSG and shadow parts:
- Shadow on: 171 local discontinuities, in bursts of single seconds. 170
  of them were in the PSG part; the shadow part had 3.
- Shadow off: the same PSG bursts showed 8.

The bursts are low-level noise-channel percussion (values +-600..1800,
0.2-1 s, about every 7-8 s). The engine synthesizes PSG with ideal hard
edges. The canon FIFO music (8-bit sample-and-hold at about 13 kHz) masks
them; the clean shadow does not.

The player compared three offline renders. The one with a 2nd-order
Butterworth low-pass at 8 kHz on the PSG part only removed the crackle and
kept the percussion. Engine patch 0016 applies that filter while the shadow
substitutes. It is primed at the current level, so there is no step, and
the canon path is unchanged. mGBA likewise offers an optional audio
low-pass.

Bridge-input capture from the same state, 45 s each:

| | discontinuities | worst second | >10 kHz energy |
|---|---|---|---|
| shadow on, no filter | 171 | 52 | 0.076% |
| shadow on, PSG low-pass (0016) | 1 | 1 | 0.015% |
| shadow off (canon) | 8 | 2 | 0.715% |

## Unrelated finding: mono output

The engine mixes mono and opens the host audio device with one channel
(`gba_audio.cpp` `mix_one_sample`, "mix mono for now";
`host_window.cpp` `want.channels = 1`). MOTHER 3's stereo is lost with or
without either enhancement.

## Open questions

- Battle and rhythm-combo music with the fixed shadow (0012): live
  percentage and reverts.
- `Mp2kShadow` assumes a 65536 Hz grid (`kRenderHz`), but `GbaAudio`'s grid
  follows SOUNDBIAS (32768-262144 Hz). A game that changes the resolution
  would play the shadow at the wrong pitch; the verifier should then keep it
  off.
- Does it stay in step with the rhythm-combo timing? No problem was heard in
  one battle.
- What are the pools in the table at 0x08120E20? This only matters for the
  RetroAchievements request.
