/* ==========================================================================
 *  patch.c -- ROM patch engine: IPS, IPS32, UPS and BPS
 *
 *  This is the hook mod support uses: the front-end loads a clean dump, then
 *  gb_apply_patch() mutates the in-memory ROM before it is committed -- the
 *  same pattern the parent Mother 3 recomp uses for its .ups mods.
 *
 *  All three formats patch in place (input == output buffer), which is valid
 *  because every copy is done with memmove and the formats never require the
 *  output to grow past the buffer we already hold.
 * ========================================================================== */
#include "gb_string.h"
#include "gb_internal.h"

/* ------------------------------------------------------------- varints   */

/* UPS / BPS unsigned LEB128.  Note the unusual carry: when the high bit is
 * set, the next group is weighted by (shift << 7) *plus* one extra shift,
 * which keeps 0x80-encoded zero distinct from a bare zero.                 */
static uint32_t vuint(const uint8_t *p, size_t size, size_t *pos, int *err)
{
    uint32_t v = 0;
    uint32_t shift = 1;
    for (;;) {
        if (*pos >= size) { *err = 1; return 0; }
        uint8_t b = p[(*pos)++];
        v += (uint32_t)(b & 0x7F) * shift;
        if (!(b & 0x80)) break;
        shift <<= 7;
        v += shift;
        if (shift > (1u << 28)) { *err = 1; return 0; }
    }
    return v;
}

/* UPS signed: zigzag, low bit = sign, magnitude = v >> 1, negative is
 * -(magnitude) - 1.                                                        */
static int32_t ups_signed(const uint8_t *p, size_t size, size_t *pos, int *err)
{
    uint32_t u = vuint(p, size, pos, err);
    if (*err) return 0;
    int32_t mag = (int32_t)(u >> 1);
    return (u & 1) ? -(mag + 1) : mag;
}

/* BPS signed: low bit = sign, magnitude = v >> 1 (no +1). */
static int32_t bps_signed(const uint8_t *p, size_t size, size_t *pos, int *err)
{
    uint32_t u = vuint(p, size, pos, err);
    if (*err) return 0;
    int32_t mag = (int32_t)(u >> 1);
    return (u & 1) ? -mag : mag;
}

/* -------------------------------------------------------------- IPS ----- */

static int patch_ips(uint8_t *rom, size_t rom_size, size_t rom_cap,
                     const uint8_t *p, size_t size, int ips32)
{
    size_t pos = ips32 ? 5 : 5;   /* "PATCH" / "IPS32" are both 5 bytes */
    size_t trunc_mark = 0;

    for (;;) {
        size_t need = ips32 ? 4 : 3;
        if (pos + need > size) break;

        /* EOF / EEOF terminator */
        if (memcmp(&p[pos], ips32 ? "EEOF" : "EOF", need) == 0) {
            pos += need;
            break;
        }

        uint32_t off;
        if (ips32) {
            off = ((uint32_t)p[pos] << 24) | ((uint32_t)p[pos+1] << 16) |
                  ((uint32_t)p[pos+2] << 8) | p[pos+3];
        } else {
            off = ((uint32_t)p[pos] << 16) | ((uint32_t)p[pos+1] << 8) | p[pos+2];
        }
        pos += need;

        if (pos + 2 > size) return -1;
        uint32_t len = (uint32_t)((p[pos] << 8) | p[pos+1]);
        pos += 2;

        if (len == 0) {
            /* RLE record */
            if (pos + 3 > size) return -1;
            uint32_t run = (uint32_t)((p[pos] << 8) | p[pos+1]);
            uint8_t val = p[pos+2];
            pos += 3;
            if ((uint64_t)off + run > rom_cap) return -2;
            memset(&rom[off], val, run);
            if (off + run > rom_size) rom_size = off + run;
        } else {
            if (pos + len > size) return -1;
            if ((uint64_t)off + len > rom_cap) return -2;
            memcpy(&rom[off], &p[pos], len);
            pos += len;
            if (off + len > rom_size) rom_size = off + len;
        }
        trunc_mark = pos;
    }

    /* optional trailing TRUNCATE record */
    if (trunc_mark && pos + 3 <= size) {
        uint32_t t = ((uint32_t)p[pos] << 16) | ((uint32_t)p[pos+1] << 8) | p[pos+2];
        if (t >= 0x8000 && t <= rom_cap) rom_size = t;
    }
    return 0;
}

/* -------------------------------------------------------------- UPS ----- */
/*  "BPS1" | inputSize | outputSize | records... | metaLen | meta |
 *  inputCRC | outputCRC | patchCRC
 *  record: (length << 2) | mode ; mode 0 = COPY, 1 = XFER                  */

static int patch_ups(uint8_t *rom, size_t rom_size, size_t rom_cap,
                     const uint8_t *p, size_t size, int dry)
{
    size_t pos = 4;
    int err = 0;

    uint32_t in_size  = vuint(p, size, &pos, &err);
    uint32_t out_size = vuint(p, size, &pos, &err);
    if (err) return -1;
    if (in_size > rom_cap || out_size > rom_cap) return -2;

    if (out_size > rom_size) {
        if (!dry) memset(&rom[rom_size], 0, out_size - rom_size);
        rom_size = out_size;
    }

    uint32_t out_pos = 0;
    int32_t  rel = 0;
    while (out_pos < out_size) {
        uint32_t hdr = vuint(p, size, &pos, &err);
        if (err) return -1;
        uint32_t mode = hdr & 3;
        uint32_t len  = hdr >> 2;
        if (len == 0) return -4;
        if ((uint64_t)out_pos + len > rom_cap) return -2;

        if (mode == 0) {                     /* COPY from the input image  */
            rel += ups_signed(p, size, &pos, &err);
            if (err) return -1;
            if (rel < 0 || (uint64_t)rel + len > rom_size) return -3;
            if (!dry) memmove(&rom[out_pos], &rom[rel], len);
            rel += (int32_t)len;
        } else if (mode == 1) {              /* XFER literal bytes         */
            if (pos + len > size) return -1;
            if (!dry) memcpy(&rom[out_pos], &p[pos], len);
            pos += len;
            rel = (int32_t)(out_pos + len);
        } else {
            return -4;                       /* UPS only defines 0 and 1   */
        }
        out_pos += len;
    }
    return (out_pos == out_size) ? 0 : -5;
}

/* -------------------------------------------------------------- BPS ----- */
/*  "BPS1" | inputSize | outputSize | metaLen | meta | actions... | CRCs    */

static int patch_bps(uint8_t *rom, size_t rom_size, size_t rom_cap,
                     const uint8_t *p, size_t size, int dry)
{
    size_t pos = 4;
    int err = 0;

    uint32_t in_size  = vuint(p, size, &pos, &err);
    uint32_t out_size = vuint(p, size, &pos, &err);
    uint32_t meta     = vuint(p, size, &pos, &err);
    if (err) return -1;
    if (in_size > rom_cap || out_size > rom_cap) return -2;
    pos += meta;
    if (pos >= size) return -1;

    if (out_size > rom_size) {
        if (!dry) memset(&rom[rom_size], 0, out_size - rom_size);
        rom_size = out_size;
    }

    uint32_t out_pos = 0;
    int32_t src_rel = 0, tgt_rel = 0;

    /* 12 trailing bytes are the three CRC32s */
    while (out_pos < out_size && pos + 12 <= size) {
        uint32_t hdr = vuint(p, size, &pos, &err);
        if (err) return -1;
        uint32_t mode = hdr & 3;
        uint32_t len  = (hdr >> 2) + 1;
        if ((uint64_t)out_pos + len > rom_cap) return -2;

        switch (mode) {
            case 0:  /* SourceRead: input[out_pos] -> output[out_pos] */
                if (out_pos + len > rom_size) return -3;
                /* in-place: nothing to do */
                break;
            case 1:  /* TargetRead: literal bytes from the patch */
                if (pos + len > size) return -1;
                if (!dry) memcpy(&rom[out_pos], &p[pos], len);
                pos += len;
                break;
            case 2: { /* SourceCopy */
                src_rel += bps_signed(p, size, &pos, &err);
                if (err) return -1;
                if (src_rel < 0 || (uint64_t)src_rel + len > rom_size) return -3;
                if (!dry) memmove(&rom[out_pos], &rom[src_rel], len);
                src_rel += (int32_t)len;
                break;
            }
            case 3: { /* TargetCopy: copy from already-written output */
                tgt_rel += bps_signed(p, size, &pos, &err);
                if (err) return -1;
                if (tgt_rel < 0 || (uint64_t)tgt_rel + len > rom_cap) return -2;
                /* byte-wise: source may overlap destination forwards */
                if (!dry)
                    for (uint32_t i = 0; i < len; i++)
                        rom[out_pos + i] = rom[tgt_rel + i];
                tgt_rel += (int32_t)len;
                break;
            }
            default:
                return -4;
        }
        out_pos += len;
    }
    return (out_pos == out_size) ? 0 : -5;
}

/* --------------------------------------------------------------- entry   */

int gb_apply_patch(const uint8_t *patch, size_t size)
{
    if (!patch || size < 8) return -1;
    uint8_t *rom = gb_rom_data();
    size_t   rsz = gb_rom_size();
    if (!rom || rsz < 0x8000) return -2;

    int r;

    if (size >= 5 && memcmp(patch, "PATCH", 5) == 0) {
        r = patch_ips(rom, rsz, GB_ROM_MAX, patch, size, 0);
        if (r == 0) gb_rom_recalc_crc();
        return r;
    }

    if (size >= 5 && memcmp(patch, "IPS32", 5) == 0) {
        r = patch_ips(rom, rsz, GB_ROM_MAX, patch, size, 1);
        if (r == 0) gb_rom_recalc_crc();
        return r;
    }

    if (size >= 4 && memcmp(patch, "BPS1", 4) == 0) {
        /* UPS and BPS share the "BPS1" magic and differ in layout: BPS puts
         * its metadata up front, UPS at the end.  Rather than snapshot the
         * whole ROM to allow a rollback, each parser can run "dry" -- full
         * structure and bounds validation with no writes -- so we can pick
         * the right one and only then mutate the image.                    */
        if (patch_ups(rom, rsz, GB_ROM_MAX, patch, size, 1) == 0) {
            r = patch_ups(rom, rsz, GB_ROM_MAX, patch, size, 0);
            if (r == 0) gb_rom_recalc_crc();
            return r;
        }

        if (patch_bps(rom, rsz, GB_ROM_MAX, patch, size, 1) == 0) {
            r = patch_bps(rom, rsz, GB_ROM_MAX, patch, size, 0);
            if (r == 0) gb_rom_recalc_crc();
            return r;
        }

        return -5;   /* neither format validated: ROM left untouched */
    }

    return -3;   /* unknown format */
}
