/* ==========================================================================
 *  state.c -- save states (F1/F2 quick slots) and battery-backed cart RAM
 *
 *  State layout:  PKSS | version | rom_crc32 | payload_size | payload | crc
 *  The payload is every byte of gb_state_t that precedes the framebuffer, so
 *  a state is portable across builds of the same core version and stays
 *  ~280 KB instead of dragging a 92 KB frame along with it.
 * ========================================================================== */
#include "gb_string.h"
#include "gb_internal.h"

#define STATE_MAGIC   0x53534B50u   /* "PKSS" */
#define STATE_HDR_SZ  20
#define STATE_CRC_SZ  4

static size_t payload_size(void)
{
    return (size_t)((const char *)&g_gb.fb - (const char *)&g_gb);
}

static uint32_t crc32_buf(const uint8_t *d, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

size_t gb_state_size(void)
{
    return STATE_HDR_SZ + payload_size() + STATE_CRC_SZ;
}

int gb_save_state(uint8_t *dst, size_t cap)
{
    size_t need = gb_state_size();
    if (!dst || cap < need || !g_gb.loaded) return -1;

    uint32_t psz = (uint32_t)payload_size();
    dst[0] = (uint8_t)(STATE_MAGIC);
    dst[1] = (uint8_t)(STATE_MAGIC >> 8);
    dst[2] = (uint8_t)(STATE_MAGIC >> 16);
    dst[3] = (uint8_t)(STATE_MAGIC >> 24);
    dst[4] = (uint8_t)(GB_STATE_VERSION);
    dst[5] = (uint8_t)(GB_STATE_VERSION >> 8);
    dst[6] = (uint8_t)(GB_STATE_VERSION >> 16);
    dst[7] = (uint8_t)(GB_STATE_VERSION >> 24);
    uint32_t crc = gb_rom_crc32();
    memcpy(&dst[8], &crc, 4);
    memcpy(&dst[12], &psz, 4);
    memcpy(&dst[16], &g_gb.cgb_mode, 1);
    dst[17] = 0; dst[18] = 0; dst[19] = 0;

    memcpy(&dst[STATE_HDR_SZ], &g_gb, psz);
    uint32_t pcrc = crc32_buf(&dst[STATE_HDR_SZ], psz);
    memcpy(&dst[STATE_HDR_SZ + psz], &pcrc, 4);
    return 0;
}

int gb_load_state(const uint8_t *src, size_t size)
{
    if (!src || size < STATE_HDR_SZ + 4) return -1;
    uint32_t magic;
    memcpy(&magic, src, 4);
    if (magic != STATE_MAGIC) return -2;

    uint32_t version;
    memcpy(&version, &src[4], 4);
    if (version != GB_STATE_VERSION) return -3;   /* incompatible layout */

    uint32_t crc;
    memcpy(&crc, &src[8], 4);
    if (g_gb.loaded && crc != gb_rom_crc32()) return -4;  /* wrong ROM */

    uint32_t psz;
    memcpy(&psz, &src[12], 4);
    if (psz != payload_size()) return -5;
    if (size < STATE_HDR_SZ + psz + 4) return -6;

    uint32_t want;
    memcpy(&want, &src[STATE_HDR_SZ + psz], 4);
    if (want != crc32_buf(&src[STATE_HDR_SZ], psz)) return -7;

    memcpy(&g_gb, &src[STATE_HDR_SZ], psz);
    /* the framebuffer is regenerated on the next frame; clear it so a
     * paused frontend never shows a stale image.                          */
    memset(g_gb.fb, 0, sizeof(g_gb.fb));
    return 0;
}

/* ----------------------------------------------------------------- SRAM  */

size_t gb_sram_size(void)
{
    if (!g_gb.cart.has_ram) return 0;
    return g_gb.cart.ram_size ? g_gb.cart.ram_size : 0;
}

int gb_has_battery(void) { return g_gb.cart.has_battery; }

size_t gb_sram_read(uint8_t *dst, size_t cap)
{
    size_t n = gb_sram_size();
    if (!dst) return n;
    if (n > cap) n = cap;
    if (n) memcpy(dst, g_gb.cram, n);
    return n;
}

void gb_sram_write(const uint8_t *src, size_t size)
{
    size_t n = gb_sram_size();
    if (!src || !n) return;
    if (size < n) n = size;
    memcpy(g_gb.cram, src, n);
}
