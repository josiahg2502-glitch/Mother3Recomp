#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "runtime_arm.h"
#include "mother3_runtime_hooks.h"
#include "hq_audio_mixer_data.h"
#include "m3_prompts.h"
#include "m3_bezel.h"
#include "m3_mods.h"
#include "m3_settings.h"
#include <SDL.h>
#include <string>

namespace {

struct RamTemplate {
    uint32_t rom;
    uint32_t size;
};

// FLASH512_V131 helpers copied to RAM (EWRAM buffer or active IWRAM stack)
// before each call (see game.toml and asm/libc.s).
constexpr RamTemplate kFlashTemplates[] = {
    {0x080912D8u, 0x30u},  // sub_080912D8: 0x30-byte verify/compare loop
    {0x08091218u, 0x24u},  // sub_08091218: 0x24-byte byte copy loop
    {0x0809184Cu, 0x24u},  // sub_0809184C: 0x24-byte 0xFF sector erase-verify loop
    {0x080911D4u, 0x04u},  // sub_080911D4: ldrb r0,[r0]; bx lr read stub
};

bool in_ram(uint32_t a, uint32_t size) {
    const bool iw = a >= 0x03000000u && a + size <= 0x03008000u;
    const bool ew = a >= 0x02000000u && a + size <= 0x02040000u;
    return iw || ew;
}

// Fast 32-bit/16-bit template verification (4x-8x fewer bus reads than byte-by-byte).
bool ram_matches_rom(uint32_t ram, uint32_t rom, uint32_t size) {
    uint32_t off = 0;
    if (((ram | rom) & 3u) == 0u) {
        for (; off + 4u <= size; off += 4u) {
            if (bus_read_u32(ram + off) != bus_read_u32(rom + off)) return false;
        }
    }
    for (; off + 2u <= size; off += 2u) {
        if (bus_read_u16(ram + off) != bus_read_u16(rom + off)) return false;
    }
    for (; off < size; ++off) {
        if (bus_read_u8(ram + off) != bus_read_u8(rom + off)) return false;
    }
    return true;
}

// Detect ipatix's mixer at either the Japanese or fan-translation load address.
// Its prologue is:
//   7943 (ldrb r3, [r0, #5])
//   089B (lsrs r3, r3, #2)
//   D040 (beq clear_buffer)
//   A101 (adr r1, do_reverb)
constexpr uint32_t kHqMixerJapaneseStart = 0x03001B60u;
constexpr uint32_t kHqMixerFanStart      = 0x03001C00u;
constexpr uint32_t kHqMixerSize          = 0x087Cu;
constexpr uint32_t kHqMixerSig0  = 0x089B7943u;
constexpr uint32_t kHqMixerSig1  = 0xA101D040u;

bool is_hq_mixer_present(uint32_t start) {
    return bus_read_u32(start) == kHqMixerSig0 &&
           bus_read_u32(start + 4u) == kHqMixerSig1;
}

bool hq_mixer_interp_forced() {
    static int s_mode = -1;
    if (s_mode < 0) {
        const char* env = std::getenv("MOTHER3_HQ_MIXER_INTERP");
        s_mode = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    return s_mode != 0;
}

// Native C++ implementation of ipatix's 16-bit m4a_hq_mixer (revision 2.1,
// USED_GAME == GAME_A3UJ, PREVENT_CLIP == 1, ENABLE_DECOMPRESSION == 0).
// Executes the complete mixer pass in native host code, avoiding self-modifying
// ARM instruction hazards (`runtime_created_routine` / `fixed_mixing_custom_routine`)
// and Stage-2 TCC compilation stalls on the audio hot path.
int run_hq_mixer_native(uint32_t mixer_start) {
    const uint32_t sp = g_cpu.R[13];
    const uint32_t r0_in = g_cpu.R[0];  // gSoundInfo work area
    const uint32_t r4_in = g_cpu.R[4];  // pcmDmaCounter
    const uint32_t r5_in = g_cpu.R[5];  // current output buffer pos
    const uint32_t r6_in = g_cpu.R[6];  // PCM_DMA_BUF_SIZE (0x630)
    const uint32_t r8_in = g_cpu.R[8];  // pcmSamplesPerVBlank (e.g. 0x108)

    if (r8_in == 0u || r8_in > 0x400u || (r8_in & 7u) != 0u) return 0;

    const uint32_t hq_buf = bus_read_u32(mixer_start + 0x184u);
    if (hq_buf < 0x03000000u || hq_buf + r8_in * 4u > 0x03008000u) return 0;

    // Mirror adsr_setup's write to hq_buffer_length_label.
    bus_write_u32(mixer_start + 0x188u, r8_in);

    // Local 32-bit stereo accumulation buffer (mirrors hq_buf in IWRAM).
    uint32_t mix_buf[0x400];
    const uint32_t rev = bus_read_u8(r0_in + 5u) >> 2u;
    if (rev != 0u) {
        uint32_t p1 = r5_in;
        uint32_t p2 = (r4_in == 2u) ? (r0_in + 0x350u) : (r5_in + r8_in);
        const uint32_t rev_fac = rev | (rev << 16u);
        for (uint32_t i = 0; i < r8_in; i += 2u) {
            const int32_t s0 = static_cast<int8_t>(bus_read_u8(p1 + r6_in));
            const int32_t s1 = static_cast<int8_t>(bus_read_u8(p1));
            p1 += 1u;
            const int32_t s2 = static_cast<int8_t>(bus_read_u8(p2 + r6_in));
            const int32_t s3 = static_cast<int8_t>(bus_read_u8(p2));
            p2 += 1u;
            const int32_t s4 = static_cast<int8_t>(bus_read_u8(p1 + r6_in));
            const int32_t s5 = static_cast<int8_t>(bus_read_u8(p1));
            p1 += 1u;
            const int32_t s6 = static_cast<int8_t>(bus_read_u8(p2 + r6_in));
            const int32_t s7 = static_cast<int8_t>(bus_read_u8(p2));
            p2 += 1u;

            int32_t sum0 = s0 + s1 + s2 + s3;
            if (sum0 < 0) sum0 += 4;
            int32_t sum1 = s4 + s5 + s6 + s7;
            if (sum1 < 0) sum1 += 4;

            mix_buf[i]     = rev_fac * static_cast<uint32_t>(sum0);
            mix_buf[i + 1] = rev_fac * static_cast<uint32_t>(sum1);
        }
    } else {
        std::memset(mix_buf, 0, r8_in * sizeof(uint32_t));
    }

    const uint32_t var_area = bus_read_u32(sp + 0x18u);
    const uint32_t pitch_fac = bus_read_u32(var_area + 0x18u);
    int32_t remain_chn = static_cast<int32_t>(bus_read_u8(var_area + 0x06u));
    uint32_t chn_ptr = var_area + 0x50u;

    constexpr uint8_t kFlagChnInit    = 0x80u;
    constexpr uint8_t kFlagChnRelease = 0x40u;
    constexpr uint8_t kFlagChnLoop    = 0x10u;
    constexpr uint8_t kFlagChnEcho    = 0x04u;
    constexpr uint8_t kFlagChnAttack  = 0x03u;
    constexpr uint8_t kFlagChnDecay   = 0x02u;
    constexpr uint8_t kModeFixedFreq  = 0x08u;
    constexpr uint8_t kModeComp       = 0x30u;
    constexpr uint8_t kModeSynth      = 0x40u;

    while (remain_chn > 0) {
        bus_write_u32(sp + 0x04u, static_cast<uint32_t>(remain_chn));
        uint32_t wave_ofs = bus_read_u32(chn_ptr + 0x24u);
        uint8_t status = bus_read_u8(chn_ptr + 0x00u);

        if ((status & 0xC7u) != 0u) {
            uint32_t adsr_level = 0;
            bool active = true;

            if (status & kFlagChnInit) {
                if (status & kFlagChnRelease) {
                    bus_write_u8(chn_ptr + 0x00u, 0u);
                    active = false;
                } else {
                    status = kFlagChnAttack;
                    bus_write_u32(chn_ptr + 0x28u, wave_ofs + 0x10u);
                    bus_write_u32(chn_ptr + 0x18u, bus_read_u32(wave_ofs + 0x0Cu));
                    adsr_level = 0u;
                    bus_write_u8(chn_ptr + 0x09u, 0u);
                    bus_write_u32(chn_ptr + 0x1Cu, 0u);
                    if ((bus_read_u8(wave_ofs + 0x03u) >> 6u) != 0u) {
                        status |= kFlagChnLoop;
                    }
                    // Attack handler on init
                    adsr_level += bus_read_u8(chn_ptr + 0x04u);
                    if (adsr_level >= 0xFFu) {
                        adsr_level = 0xFFu;
                        status -= 1u;
                    }
                    bus_write_u8(chn_ptr + 0x00u, status);
                }
            } else {
                adsr_level = bus_read_u8(chn_ptr + 0x09u);
                if (status & kFlagChnEcho) {
                    uint8_t echo_rem = static_cast<uint8_t>(bus_read_u8(chn_ptr + 0x0Du) - 1u);
                    bus_write_u8(chn_ptr + 0x0Du, echo_rem);
                    if (echo_rem == 0u) {
                        bus_write_u8(chn_ptr + 0x00u, 0u);
                        active = false;
                    }
                } else if (status & kFlagChnRelease) {
                    adsr_level = (adsr_level * bus_read_u8(chn_ptr + 0x07u)) >> 8u;
                    const uint32_t echo_vol = bus_read_u8(chn_ptr + 0x0Cu);
                    if (adsr_level == 0u || adsr_level <= echo_vol) {
                        adsr_level = echo_vol;
                        if (adsr_level == 0u) {
                            bus_write_u8(chn_ptr + 0x00u, 0u);
                            active = false;
                        } else {
                            status |= kFlagChnEcho;
                            bus_write_u8(chn_ptr + 0x00u, status);
                        }
                    }
                } else if ((status & 3u) == kFlagChnDecay) {
                    adsr_level = (adsr_level * bus_read_u8(chn_ptr + 0x05u)) >> 8u;
                    const uint32_t sustain = bus_read_u8(chn_ptr + 0x06u);
                    if (adsr_level <= sustain) {
                        adsr_level = sustain;
                        if (adsr_level == 0u) {
                            adsr_level = bus_read_u8(chn_ptr + 0x0Cu);
                            if (adsr_level == 0u) {
                                bus_write_u8(chn_ptr + 0x00u, 0u);
                                active = false;
                            } else {
                                status |= kFlagChnEcho;
                                bus_write_u8(chn_ptr + 0x00u, status);
                            }
                        } else {
                            status -= 1u;
                            bus_write_u8(chn_ptr + 0x00u, status);
                        }
                    }
                } else if ((status & 3u) == kFlagChnAttack) {
                    adsr_level += bus_read_u8(chn_ptr + 0x04u);
                    if (adsr_level >= 0xFFu) {
                        adsr_level = 0xFFu;
                        status -= 1u;
                    }
                    bus_write_u8(chn_ptr + 0x00u, status);
                }
            }

            if (active) {
                bus_write_u8(chn_ptr + 0x09u, static_cast<uint8_t>(adsr_level));
                const uint32_t master_vol = bus_read_u8(var_area + 0x07u) + 1u;
                const uint32_t env_scaled = master_vol * adsr_level;
                const uint32_t vol_l = (bus_read_u8(chn_ptr + 0x03u) * env_scaled) >> 13u;
                const uint32_t vol_r = (bus_read_u8(chn_ptr + 0x02u) * env_scaled) >> 13u;

                uint32_t loop_start_abs = 0u;
                uint32_t loop_len = 0u;
                if (status & kFlagChnLoop) {
                    const uint32_t ls = bus_read_u32(wave_ofs + 0x08u);
                    const uint32_t le = bus_read_u32(wave_ofs + 0x0Cu);
                    loop_start_abs = wave_ofs + 0x10u + ls;
                    loop_len = le - ls;
                    bus_write_u32(sp + 0x0Cu, loop_start_abs);
                }
                bus_write_u32(sp + 0x10u, loop_len);

                int32_t rem_samples = static_cast<int32_t>(bus_read_u32(chn_ptr + 0x18u));
                uint32_t abs_pos = bus_read_u32(chn_ptr + 0x28u);
                uint8_t mode = bus_read_u8(chn_ptr + 0x01u);
                uint32_t r11 = (vol_l & 0xFFFFu) | (vol_r << 16u);

                if (r11 != 0u) {
                    if (mode & kModeFixedFreq) {
                        int32_t buf_rem = static_cast<int32_t>(r8_in);
                        uint32_t buf_idx = 0u;
                        bool chn_stopped = false;
                        while (buf_rem > 0 && !chn_stopped) {
                            int32_t blk_samples = (rem_samples <= buf_rem) ? rem_samples : buf_rem;
                            uint32_t quads = static_cast<uint32_t>(blk_samples - 1) >> 2u;
                            if (quads > 0u) {
                                const uint32_t n_samp = quads << 2u;
                                buf_rem -= static_cast<int32_t>(n_samp);
                                rem_samples -= static_cast<int32_t>(n_samp);
                                for (uint32_t q = 0; q < n_samp; ++q) {
                                    const int32_t s = static_cast<int8_t>(bus_read_u8(abs_pos++));
                                    if (s != 0) {
                                        mix_buf[buf_idx] += r11 * static_cast<uint32_t>(s);
                                    }
                                    ++buf_idx;
                                }
                            }
                            for (int u = 0; u < 4 && !chn_stopped; ++u) {
                                const int32_t s = static_cast<int8_t>(bus_read_u8(abs_pos++));
                                mix_buf[buf_idx++] += r11 * static_cast<uint32_t>(s);
                                rem_samples -= 1;
                                if (rem_samples <= 0) {
                                    if (loop_len != 0u) {
                                        rem_samples = static_cast<int32_t>(loop_len);
                                        abs_pos = loop_start_abs;
                                    } else {
                                        bus_write_u8(chn_ptr + 0x00u, 0u);
                                        chn_stopped = true;
                                    }
                                }
                            }
                            buf_rem -= 4;
                        }
                        if (!chn_stopped) {
                            bus_write_u32(chn_ptr + 0x18u, static_cast<uint32_t>(rem_samples));
                            bus_write_u32(chn_ptr + 0x28u, abs_pos);
                        }
                    } else if ((mode & kModeComp) == 0u) {
                        if (rem_samples == 0) {
                            mode |= kModeSynth;
                            bus_write_u8(chn_ptr + 0x01u, mode);
                        }
                        uint32_t fine_pos = bus_read_u32(chn_ptr + 0x1Cu);
                        const uint32_t freq = bus_read_u32(chn_ptr + 0x20u);
                        const uint32_t step = pitch_fac * freq;
                        int32_t base_s = static_cast<int8_t>(bus_read_u8(abs_pos++));
                        int32_t next_s = static_cast<int8_t>(bus_read_u8(abs_pos));

                        if (mode & kModeSynth) {
                            const uint8_t synth_type = static_cast<uint8_t>(next_s);
                            if (synth_type == 0u) {
                                const uint32_t wc1 = bus_read_u8(abs_pos + 1u);
                                const uint32_t wc2 = bus_read_u8(abs_pos + 3u);
                                rem_samples = static_cast<int32_t>(
                                    static_cast<uint32_t>(rem_samples) + (wc1 << 24u));
                                uint32_t duty = static_cast<uint32_t>(rem_samples) + (wc2 << 24u);
                                if (static_cast<int32_t>(duty) < 0) duty = ~duty;
                                const uint32_t r10 = duty >> 8u;
                                const uint32_t mod_amt = bus_read_u8(abs_pos + 2u);
                                const uint32_t base_duty =
                                    static_cast<uint32_t>(bus_read_u8(abs_pos)) << 24u;
                                const uint32_t threshold = r10 * mod_amt + base_duty;
                                const uint32_t delta_v = r11 << 6u;
                                const uint32_t step8 = step << 3u;
                                for (uint32_t i = 0; i < r8_in; ++i) {
                                    if (fine_pos < threshold) mix_buf[i] += delta_v;
                                    else                      mix_buf[i] -= delta_v;
                                    fine_pos += step8;
                                }
                                abs_pos -= 1u;
                            } else if (synth_type == 1u) {
                                uint32_t v = (r11 >> 1u) & ~0xFF00u;
                                const uint32_t step8 = step << 3u;
                                for (uint32_t i = 0; i < r8_in; ++i) {
                                    fine_pos += step8;
                                    int32_t r9 = static_cast<int32_t>(fine_pos >> 24u) - 0x70;
                                    uint32_t r6_t = fine_pos << 1u;
                                    r9 -= static_cast<int32_t>(r6_t >> 27u);
                                    rem_samples = r9 + (rem_samples >> 1);
                                    if (rem_samples != 0) {
                                        mix_buf[i] += v * static_cast<uint32_t>(rem_samples);
                                    }
                                }
                                abs_pos -= 1u;
                            } else {
                                const uint32_t step8 = step << 3u;
                                for (uint32_t i = 0; i < r8_in; ++i) {
                                    fine_pos += step8;
                                    int32_t r9;
                                    if (static_cast<int32_t>(fine_pos) >= 0) {
                                        r9 = (static_cast<int32_t>(fine_pos) >> 23) - 0x80;
                                    } else {
                                        r9 = 0x180 - static_cast<int32_t>(fine_pos >> 23u);
                                    }
                                    mix_buf[i] += r11 * static_cast<uint32_t>(r9);
                                }
                                abs_pos -= 1u;
                            }
                            bus_write_u32(chn_ptr + 0x1Cu, fine_pos);
                            bus_write_u32(chn_ptr + 0x18u, static_cast<uint32_t>(rem_samples));
                            bus_write_u32(chn_ptr + 0x28u, abs_pos);
                        } else {
                            int32_t delta = next_s - base_s;
                            const uint32_t c_in = r11 & 1u;
                            r11 = ((r11 >> 1u) + 0x8000u + c_in) & ~0xFF00u;

                            const uint64_t total_fine =
                                static_cast<uint64_t>(fine_pos) +
                                static_cast<uint64_t>(step) * static_cast<uint64_t>(r8_in);
                            const uint32_t needed = static_cast<uint32_t>(total_fine >> 23u);

                            if (rem_samples > static_cast<int32_t>(needed)) {
                                // Fast path (path 1)
                                const int32_t final_rem = rem_samples - static_cast<int32_t>(needed);
                                const uint32_t final_abs = abs_pos + needed;
                                if (step < 0x800000u) {
                                    for (uint32_t i = 0; i < r8_in; ++i) {
                                        const int32_t interp =
                                            (static_cast<int32_t>(fine_pos * static_cast<uint32_t>(delta)) >> 22) +
                                            (base_s << 1);
                                        if (interp != 0) {
                                            mix_buf[i] += r11 * static_cast<uint32_t>(interp);
                                        }
                                        fine_pos += step;
                                        if (fine_pos >= 0x800000u) {
                                            base_s += delta;
                                            ++abs_pos;
                                            delta = static_cast<int8_t>(bus_read_u8(abs_pos)) - base_s;
                                            fine_pos &= 0x007FFFFFu;
                                        }
                                    }
                                } else if (step < 0x1000000u) {
                                    const uint32_t v = r11 << 1u;
                                    for (uint32_t i = 0; i < r8_in; ++i) {
                                        const int32_t interp =
                                            base_s +
                                            (static_cast<int32_t>(fine_pos * static_cast<uint32_t>(delta)) >> 23);
                                        if (interp != 0) {
                                            mix_buf[i] += v * static_cast<uint32_t>(interp);
                                        }
                                        fine_pos += step;
                                        if (fine_pos >= 0x1000000u) {
                                            ++abs_pos;
                                            base_s = static_cast<int8_t>(bus_read_u8(abs_pos));
                                        } else {
                                            base_s += delta;
                                        }
                                        ++abs_pos;
                                        delta = static_cast<int8_t>(bus_read_u8(abs_pos)) - base_s;
                                        fine_pos &= 0x007FFFFFu;
                                    }
                                } else {
                                    const uint32_t v = r11 << 1u;
                                    for (uint32_t i = 0; i < r8_in; ++i) {
                                        const int32_t interp =
                                            base_s +
                                            (static_cast<int32_t>(fine_pos * static_cast<uint32_t>(delta)) >> 23);
                                        if (interp != 0) {
                                            mix_buf[i] += v * static_cast<uint32_t>(interp);
                                        }
                                        fine_pos += step;
                                        abs_pos += (fine_pos >> 23u) - 1u;
                                        base_s = static_cast<int8_t>(bus_read_u8(abs_pos));
                                        ++abs_pos;
                                        delta = static_cast<int8_t>(bus_read_u8(abs_pos)) - base_s;
                                        fine_pos &= 0x007FFFFFu;
                                    }
                                }
                                bus_write_u32(chn_ptr + 0x1Cu, fine_pos);
                                bus_write_u32(chn_ptr + 0x18u, static_cast<uint32_t>(final_rem));
                                bus_write_u32(chn_ptr + 0x28u, final_abs - 1u);
                            } else {
                                // Uncached path (path 2: supports loop / stop mid-buffer)
                                bool chn_stopped = false;
                                for (uint32_t i = 0; i < r8_in; ++i) {
                                    const int32_t interp =
                                        (static_cast<int32_t>(fine_pos * static_cast<uint32_t>(delta)) >> 22) +
                                        (base_s << 1);
                                    if (interp != 0) {
                                        mix_buf[i] += r11 * static_cast<uint32_t>(interp);
                                    }
                                    fine_pos += step;
                                    uint32_t overflow = fine_pos >> 23u;
                                    if (overflow != 0u) {
                                        rem_samples -= static_cast<int32_t>(overflow);
                                        if (rem_samples <= 0) {
                                            if (loop_len != 0u) {
                                                overflow = static_cast<uint32_t>(-rem_samples);
                                                rem_samples += static_cast<int32_t>(loop_len);
                                                abs_pos = loop_start_abs + overflow;
                                                base_s = static_cast<int8_t>(bus_read_u8(abs_pos));
                                                ++abs_pos;
                                                delta = static_cast<int8_t>(bus_read_u8(abs_pos)) - base_s;
                                                fine_pos &= ~0x3F800000u;
                                                continue;
                                            } else {
                                                bus_write_u8(chn_ptr + 0x00u, 0u);
                                                chn_stopped = true;
                                                break;
                                            }
                                        }
                                        overflow -= 1u;
                                        if (overflow == 0u) {
                                            base_s += delta;
                                        } else {
                                            abs_pos += overflow;
                                            base_s = static_cast<int8_t>(bus_read_u8(abs_pos));
                                        }
                                        ++abs_pos;
                                        delta = static_cast<int8_t>(bus_read_u8(abs_pos)) - base_s;
                                        fine_pos &= ~0x3F800000u;
                                    }
                                }
                                if (!chn_stopped) {
                                    bus_write_u32(chn_ptr + 0x1Cu, fine_pos);
                                    bus_write_u32(chn_ptr + 0x18u, static_cast<uint32_t>(rem_samples));
                                    bus_write_u32(chn_ptr + 0x28u, abs_pos - 1u);
                                }
                            }
                        }
                    }
                } else {
                    bus_write_u32(chn_ptr + 0x18u, static_cast<uint32_t>(rem_samples));
                    bus_write_u32(chn_ptr + 0x28u, abs_pos);
                }
            }
        }
        remain_chn -= 1;
        chn_ptr += 0x40u;
    }

    // Sync workbuffer to IWRAM and run the soft-clipping downsampler (PREVENT_CLIP == 1).
    uint32_t out_pos = bus_read_u32(sp + 0x08u);
    for (uint32_t i = 0; i < r8_in; i += 2u) {
        bus_write_u32(hq_buf + i * 4u, mix_buf[i]);
        bus_write_u32(hq_buf + (i + 1u) * 4u, mix_buf[i + 1u]);

        int32_t r2 = static_cast<int16_t>(mix_buf[i] & 0xFFFFu);
        int32_t r0 = static_cast<int16_t>(mix_buf[i] >> 16u);
        int32_t r3 = static_cast<int16_t>(mix_buf[i + 1u] & 0xFFFFu);
        int32_t r1 = static_cast<int16_t>(mix_buf[i + 1u] >> 16u);

        if (r0 >= 0x4000) r0 = 0x3F80; else if (r0 < -0x4000) r0 = -0x4000;
        if (r1 >= 0x4000) r1 = 0x3F80; else if (r1 < -0x4000) r1 = -0x4000;
        if (r2 >= 0x4000) r2 = 0x3F80; else if (r2 < -0x4000) r2 = -0x4000;
        if (r3 >= 0x4000) r3 = 0x3F80; else if (r3 < -0x4000) r3 = -0x4000;

        const uint16_t right_pair = static_cast<uint16_t>(
            ((r2 >> 7) & 0xFF) | (((r3 >> 7) & 0xFF) << 8));
        const uint16_t left_pair = static_cast<uint16_t>(
            ((r0 >> 7) & 0xFF) | (((r1 >> 7) & 0xFF) << 8));

        bus_write_u16(out_pos + 0x630u, right_pair);
        bus_write_u16(out_pos, left_pair);
        out_pos += 2u;
    }

    // Epilogue (downsampler_return): mark SoundInfo ident and restore SoundMain's frame.
    bus_write_u32(var_area, 0x68736D53u);
    const uint32_t pop_base = sp + 0x1Cu;
    g_cpu.R[8]  = bus_read_u32(pop_base + 0x00u);
    g_cpu.R[9]  = bus_read_u32(pop_base + 0x04u);
    g_cpu.R[10] = bus_read_u32(pop_base + 0x08u);
    g_cpu.R[11] = bus_read_u32(pop_base + 0x0Cu);
    g_cpu.R[4]  = bus_read_u32(pop_base + 0x10u);
    g_cpu.R[5]  = bus_read_u32(pop_base + 0x14u);
    g_cpu.R[6]  = bus_read_u32(pop_base + 0x18u);
    g_cpu.R[7]  = bus_read_u32(pop_base + 0x1Cu);
    const uint32_t ret_pc = bus_read_u32(pop_base + 0x20u);
    g_cpu.R[3]  = ret_pc;
    g_cpu.R[13] = pop_base + 0x24u;
    if (ret_pc & 1u) g_cpu.cpsr |= CPSR_T_BIT;
    else             g_cpu.cpsr &= ~CPSR_T_BIT;
    g_cpu.R[15] = ret_pc & ~1u;

    runtime_tick(r8_in * 16u);
    if (!runtime_call_should_return(ret_pc)) {
        runtime_dispatch_with_exchange(ret_pc);
    }
    return 1;
}

// Guard self-modifying RAM regions from stale AOT or Stage-2 overlay DLL freezing:
// 1. The Japanese and fan-translation mixer ranges modify their own IWRAM code.
//    The entry uses run_hq_mixer_native() unless MOTHER3_HQ_MIXER_INTERP=1;
//    any interior PC (or forced interp mode) uses the reference interpreter bridge.
// 2. 0x03000000..0x03000400: static AOT holds vanilla SoundMainRAM, which the
//    HQ Sound Mixing Mod repurposes as `hq_buffer` (16-bit audio sample buffer).
// Both can only happen when the image carries changed code: the HQ patch or a
// code mod. On the stock image neither range ever changes, so the checks are
// skipped: they ran on every dispatch inside the game's sound mixer loops, and
// their bus reads took about a tenth of the game thread on an Anbernic RG556.
bool g_mutable_code = true;  // set by mother3_rom_post_load

int force_interp_mutable_ram(uint32_t pc, int thumb) {
    // Runs at every dispatch, so only a counter here; tick() limits itself further.
    static unsigned s_dispatches = 0;
    if ((++s_dispatches & 0x3FFu) == 0) {
        m3mods::apply_deferred();  // no-op after the first call
        m3prompts::tick();  // the widescreen model is checked at vblank (main.cpp)
    }
    if (!g_mutable_code) return 0;
    // The two load ranges overlap, so pick the one whose mixer is actually installed.
    uint32_t mixer_start = 0u;
    for (uint32_t start : {kHqMixerFanStart, kHqMixerJapaneseStart}) {
        if (pc >= start && pc < start + kHqMixerSize && is_hq_mixer_present(start)) {
            mixer_start = start;
            break;
        }
    }
    if (mixer_start) {
        if (pc == mixer_start && thumb && !hq_mixer_interp_forced()) {
            return 0;  // Handled by run_hq_mixer_native() in dispatch_flash_ram_variant
        }
        return 1;
    }
    if (pc >= 0x03000000u && pc < 0x03000400u) {
        if (bus_read_u32(0x03000000u) != bus_read_u32(0x0808ED80u)) {
            return 1;
        }
    }
    return 0;
}

// The FLASH512_V131 library places its helpers at varying stack depths (and
// reuses overlapping slots), so address-only dispatch cannot identify them.
// Match the live RAM words against the ROM templates (with a 1-instruction PC
// pre-filter and a 1-slot MRU cache) and run the template's AOT code at the
// equivalent offset. Also dispatches both HQ mixer load addresses.
int dispatch_flash_ram_variant(uint32_t pc, int thumb) {
    if (!thumb) return 0;

    if ((pc == kHqMixerJapaneseStart || pc == kHqMixerFanStart) &&
        !hq_mixer_interp_forced() && is_hq_mixer_present(pc)) {
        if (run_hq_mixer_native(pc)) return 1;
    }

    const uint16_t insn_at_pc = bus_read_u16(pc);

    // Check 1-slot MRU cache first for fast O(1) validation on loop back-edges.
    static uint32_t s_last_start = 0u;
    static uint32_t s_last_rom   = 0u;
    static uint32_t s_last_size  = 0u;
    if (s_last_size != 0u && pc >= s_last_start && pc < s_last_start + s_last_size) {
        const uint32_t off = pc - s_last_start;
        if (bus_read_u16(s_last_rom + off) == insn_at_pc &&
            ram_matches_rom(s_last_start, s_last_rom, s_last_size)) {
            runtime_dispatch(s_last_rom + off);
            return 1;
        }
    }

    for (const auto& t : kFlashTemplates) {
        uint32_t start = pc >= t.size - 2u ? pc - (t.size - 2u) : 0u;
        start = (start + 1u) & ~1u;
        for (; start <= pc; start += 2u) {
            if (!in_ram(start, t.size)) continue;
            const uint32_t off = pc - start;
            if (bus_read_u16(t.rom + off) != insn_at_pc) continue;
            if (!ram_matches_rom(start, t.rom, t.size)) continue;
            s_last_start = start;
            s_last_rom   = t.rom;
            s_last_size  = t.size;
            runtime_dispatch(t.rom + off);
            return 1;
        }
    }
    return 0;
}

}  // namespace

bool mother3_apply_hq_audio_patch(std::uint8_t* rom, std::size_t rom_size,
                                  const char* source_sha1) {
    constexpr std::size_t kRomLength = 0x02000000;
    constexpr std::size_t kMixerOffset = 0x01B0C140;
    constexpr std::size_t kMixerSize = 0x087C;
    constexpr std::uint32_t kVanillaMixerRom = 0x0808ED81;
    constexpr std::uint32_t kHqMixerRom = 0x09B0C141;
    static_assert(sizeof(kHqAudioMixer) == kMixerSize);
    if (!rom || rom_size != kRomLength || std::memcmp(rom + 0xAC, "A3UJ", 4) != 0)
        return false;

    auto read32 = [rom](std::size_t offset) {
        return static_cast<std::uint32_t>(rom[offset]) |
               (static_cast<std::uint32_t>(rom[offset + 1]) << 8) |
               (static_cast<std::uint32_t>(rom[offset + 2]) << 16) |
               (static_cast<std::uint32_t>(rom[offset + 3]) << 24);
    };
    auto write32 = [rom](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            rom[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
    };
    auto write16 = [rom](std::size_t offset, std::uint16_t value) {
        rom[offset] = static_cast<std::uint8_t>(value);
        rom[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    };

    const std::uint32_t mixer_rom = read32(0x8F9EC);
    const std::uint32_t mixer_cpu = read32(0x8F9F0);
    const std::uint16_t mixer_units = static_cast<std::uint16_t>(rom[0x8F9F4]) |
        (static_cast<std::uint16_t>(rom[0x8F9F5]) << 8);
    if (mixer_rom == kHqMixerRom && mixer_units == 0x021F &&
        (mixer_cpu == 0x03001B60 || mixer_cpu == 0x03001C00))
        return false; // Already patched.

    // Refuse modified mixer layouts rather than overwrite an unknown ROM hack.
    if (mixer_rom != kVanillaMixerRom || mixer_cpu != 0x03000000 ||
        mixer_units != 0x0100 || read32(0x8F9F8) != 0x03001B20 ||
        read32(0x8ED70) != 0x03000001 || read32(0x69314) != 0x02001084 ||
        read32(0x69434) != 0x02001084)
        return false;

    const std::uint8_t free_byte = rom[kMixerOffset];
    if (free_byte != 0 && free_byte != 0xFF)
        return false;
    for (std::size_t i = 1; i < kMixerSize; ++i)
        if (rom[kMixerOffset + i] != free_byte)
            return false;

    const bool japanese = source_sha1 &&
        std::strcmp(source_sha1, "4f0f493e12c2a8c61b2d809af03f7abf87a85776") == 0;
    std::memcpy(rom + kMixerOffset, kHqAudioMixer, sizeof(kHqAudioMixer));
    write32(0x8F9EC, kHqMixerRom);
    write16(0x8F9F4, 0x021F);
    if (japanese) {
        write32(0x8F9F0, 0x03001B60);
        write32(0x8F9F8, 0x0203E000);
        write32(0x8ED70, 0x03001B61);
        constexpr std::uint32_t players[] = {
            0x030023F0, 0x03002710, 0x03002A30, 0x03000B20, 0x03000C60,
            0x03000EE0, 0x03001020, 0x03001520, 0x030017A0, 0x03001840
        };
        for (std::size_t i = 0; i < sizeof(players) / sizeof(players[0]); ++i)
            write32(0x120E20 + i * 12, players[i]);
    } else {
        write32(0x69314, 0x02005230);
        write32(0x69434, 0x02005230);
        write32(0x8F9F0, 0x03001C00);
        write32(0x8F9F8, 0x03000C50);
        write32(0x8ED70, 0x03001C01);
        constexpr std::uint32_t players[] = {
            0x03002490, 0x030027B0, 0x03000930, 0x02001090, 0x03000570,
            0x030007F0, 0x020011D0, 0x030009D0, 0x030004D0, 0x020016D0
        };
        for (std::size_t i = 0; i < sizeof(players) / sizeof(players[0]); ++i)
            write32(0x120E20 + i * 12, players[i]);
    }
    std::fprintf(stderr, "[mother3:hq-audio] built-in mixer applied (%s layout)\n",
                 japanese ? "Japanese" : "fan translation");
    return true;
}

// Mods (on the stock image, so BPS/UPS checksums match), then prompts and the
// bezel, then HQ audio. Data-only mods and the prompts edit the image later,
// after the runtime has hashed it, so it keeps its native code path.
bool mother3_rom_post_load(std::uint8_t* rom, std::size_t rom_size, const char* source_sha1) {
    std::string dir = ".";
    if (char* base = SDL_GetBasePath()) {
        dir = base;
        SDL_free(base);
        if (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    }
    const bool code_mods = m3mods::apply(rom, rom_size, dir);
    m3prompts::attach(rom, rom_size, source_sha1);
#if defined(_WIN32)
    // mother3_bezel.dll draws a bezel composed from this ROM; refresh it if the flavor changed.
    m3bezel::ensure(rom, rom_size, dir, m3settings::load(dir).flavor);
#endif
    // HQ audio is on by default. The patch also moves sound buffers in RAM
    // and changes the game's RNG (see its README), so players who need the
    // stock game (speedruns, RAM watchers) can turn it off.
    bool hq = false;
    if (m3settings::load(dir).hq_audio)
        hq = mother3_apply_hq_audio_patch(rom, rom_size, source_sha1);
    else
        std::fprintf(stderr, "[mother3:hq-audio] off in the launcher settings; stock mixer\n");
    g_mutable_code = code_mods || hq;
    return code_mods || hq;
}

void mother3_install_runtime_hooks() {
    g_runtime_ram_dispatch_hook = &dispatch_flash_ram_variant;
    g_runtime_force_interp_hook = &force_interp_mutable_ram;
}
