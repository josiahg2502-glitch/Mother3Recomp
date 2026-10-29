#include "m3_devwarp.h"

#include <cstdint>
#include <cstdlib>

#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"

namespace m3devwarp {
namespace {

// The field asks 0x0800F5EC after each player step whether it took an exit (a
// door or a map edge); on 0 its caller (0x0800CD40) runs the map transition.
constexpr std::uint32_t kExitCheck = 0x0800F5EC;
constexpr std::uint32_t kGame = 0x02005C00;
// The game's entrance table (0x0801B4E4): 0x5DC records of 8 bytes, the
// destination map at +4 and its tile x, y at +6, +7.
constexpr std::uint32_t kWarpTable = 0x09169C28;
constexpr std::uint32_t kWarpCount = 0x5DC;
constexpr const char* kHook = "m3.dev-warp";
int g_target = -1;  // map to warp to once, from M3_WARP

// Takes the exit to the target map's first entrance, as a door does: the
// record is decoded like 0x08039D98 and stored like the warp setup 0x0801A434.
int take_exit(std::uint32_t, int, ArmCpuState* cpu) {
    if (g_target < 0) return 0;
    for (std::uint32_t i = 0; i < kWarpCount; ++i) {
        const std::uint32_t e = kWarpTable + 8 * i;
        if (bus_read_u16(e + 4) != g_target) continue;
        const int bits = bus_read_u16(e), b0 = bits & 0xFF, b1 = bits >> 8;
        const int x = 16 * bus_read_u8(e + 6) + 8 * (b0 & 3) + 8;
        const int y = 16 * bus_read_u8(e + 7) + 8 * (((b0 >> 2) & 3) + 1);
        const int flag = b1 >> 7, flag2 = flag ? bus_read_u8(e + 2) & 1 : 0;
        bus_write_u8(kGame + 0x5958, 1);  // a door warp
        bus_write_u16(kGame + 0x5978, static_cast<std::uint16_t>(g_target));
        bus_write_u8(kGame + 0x5959, static_cast<std::uint8_t>((bits >> 7) & 3));
        bus_write_u8(kGame + 0x595A, static_cast<std::uint8_t>((b0 >> 4) & 7));  // facing
        bus_write_u8(kGame + 0x598C, static_cast<std::uint8_t>((bus_read_u8(kGame + 0x598C) & ~0x18) | flag << 3 | flag2 << 4));
        bus_write_u8(kGame + 0x595B, 0);
        bus_write_u8(kGame + 0x595C, 0);
        bus_write_u16(kGame + 0x5960, 10);
        bus_write_u16(kGame + 0x597C, static_cast<std::uint16_t>(x));
        bus_write_u16(kGame + 0x597E, static_cast<std::uint16_t>(y));
        bus_write_u16(kGame + 0x5980, bus_read_u16(kGame + 0x67AC));  // the map we leave
        bus_write_u16(kGame + 0x5982, 0xFFFF);
        // shortcut: skips the warp setup's record of the old position (+0x5984/+0x5988), which only a return warp reads.
        g_target = -1;
        cpu->R[0] = 0;  // exit taken
        cpu->R[15] = cpu->R[14] & ~1u;
        return 1;
    }
    g_target = -1;  // no entrance into that map
    return 0;
}

GBA_MOD_CONSTRUCTOR(register_dev_warp) {
    gba_mod_register_function_entry_plugin(kHook, kExitCheck, 1, &take_exit);
}

}  // namespace

void tick() {
    static bool s_read = false;
    if (!s_read) {
        s_read = true;
        if (const char* m = std::getenv("M3_WARP")) g_target = std::atoi(m);
    }
    // Hooks start disabled and a launch disables them again.
    if (g_target >= 0 && !gba_mod_function_hook_enabled(kHook)) gba_mod_set_function_hook_enabled(kHook, 1);
}

}  // namespace m3devwarp
