/* ==========================================================================
 *  scaler.c -- CPU upscalers for the native front-end
 *
 *  The web front-end does its scaling in GLSL; this exists so the native
 *  build (and the headless test rig) get the same "upscaled, high res"
 *  look without a GPU.  Every path is integer/fixed point where it matters.
 * ========================================================================== */
#include "gb_string.h"
#include "gb_internal.h"

static inline uint32_t mix2(uint32_t a, uint32_t b, uint32_t w)
{
    /* w is 0..256 */
    uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    uint32_t r = (ar * (256 - w) + br * w) >> 8;
    uint32_t g = (ag * (256 - w) + bg * w) >> 8;
    uint32_t b2 = (ab * (256 - w) + bb * w) >> 8;
    return (r << 16) | (g << 8) | b2;
}

static inline uint32_t mix4(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                            uint32_t wx, uint32_t wy)
{
    uint32_t top = mix2(a, b, wx);
    uint32_t bot = mix2(c, d, wx);
    return mix2(top, bot, wy);
}

static inline uint32_t clamp8(int v) { return (uint32_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/* Catmull-Rom weights for a fractional position t (0..1), fixed point /64 */
static void cr_weights(int t, int w[4])
{
    /* t in 0..64 */
    int t2 = (t * t) >> 6;
    int t3 = (t2 * t) >> 6;
    w[0] = (-t3 + 2 * t2 - t * 64 / 2) >> 1;
    w[1] = (3 * t3 - 5 * t2 + 128) >> 1;
    w[2] = (-3 * t3 + 4 * t2 + t * 64 / 2) >> 1;
    w[3] = (t3 - t2) >> 1;
}

static inline uint32_t cr_sample(const uint32_t *s, int sw, int sh,
                                 int x, int y, int tx, int ty)
{
    int wx[4], wy[4];
    cr_weights(tx, wx);
    cr_weights(ty, wy);

    int32_t acc_r = 0, acc_g = 0, acc_b = 0;
    for (int j = 0; j < 4; j++) {
        int sy = y - 1 + j;
        if (sy < 0) sy = 0;
        if (sy >= sh) sy = sh - 1;
        for (int i = 0; i < 4; i++) {
            int sx = x - 1 + i;
            if (sx < 0) sx = 0;
            if (sx >= sw) sx = sw - 1;
            int wgt = wx[i] * wy[j];
            uint32_t p = s[sy * sw + sx];
            acc_r += (int32_t)((p >> 16) & 0xFF) * wgt;
            acc_g += (int32_t)((p >> 8) & 0xFF) * wgt;
            acc_b += (int32_t)(p & 0xFF) * wgt;
        }
    }
    /* weights sum to 64*64 = 4096 */
    return (clamp8((int)(acc_r / 4096)) << 16) |
           (clamp8((int)(acc_g / 4096)) << 8) |
           clamp8((int)(acc_b / 4096));
}

void gb_upscale(const uint32_t *src, uint32_t *dst,
                int src_w, int src_h, int dst_w, int dst_h,
                int filter, int grid, int scanline)
{
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return;

    /* integer scale factor, used by the pixel-art paths */
    int scale_x = dst_w / src_w;
    int scale_y = dst_h / src_h;
    int integer_scale = (scale_x >= 2 && scale_x == scale_y &&
                         dst_w == src_w * scale_x && dst_h == src_h * scale_y);

    switch (filter) {
        case GB_SCALE_NEAREST: {
            for (int y = 0; y < dst_h; y++) {
                int sy = (int)((uint64_t)y * src_h / dst_h);
                if (sy >= src_h) sy = src_h - 1;
                for (int x = 0; x < dst_w; x++) {
                    int sx = (int)((uint64_t)x * src_w / dst_w);
                    if (sx >= src_w) sx = src_w - 1;
                    dst[y * dst_w + x] = src[sy * src_w + sx];
                }
            }
            break;
        }

        case GB_SCALE_SMOOTH:
        case GB_SCALE_SHARP: {
            for (int y = 0; y < dst_h; y++) {
                float fy = ((float)y + 0.5f) * src_h / dst_h - 0.5f;
                int y0 = (int)fy;
                int ty = (int)((fy - y0) * 256.0f);
                if (y0 < 0) { y0 = 0; ty = 0; }
                int y1 = y0 + 1;
                if (y1 >= src_h) y1 = src_h - 1;
                for (int x = 0; x < dst_w; x++) {
                    float fx = ((float)x + 0.5f) * src_w / dst_w - 0.5f;
                    int x0 = (int)fx;
                    int tx = (int)((fx - x0) * 256.0f);
                    if (x0 < 0) { x0 = 0; tx = 0; }
                    int x1 = x0 + 1;
                    if (x1 >= src_w) x1 = src_w - 1;
                    uint32_t p = mix4(src[y0 * src_w + x0], src[y0 * src_w + x1],
                                      src[y1 * src_w + x0], src[y1 * src_w + x1],
                                      (uint32_t)tx, (uint32_t)ty);
                    if (filter == GB_SCALE_SHARP && integer_scale) {
                        /* snap toward the nearest source pixel so text stays
                         * crisp while edges still blend (sharp bilinear)   */
                        uint32_t n = src[y0 * src_w + x0];
                        p = mix2(n, p, 168);
                    }
                    dst[y * dst_w + x] = p;
                }
            }
            break;
        }

        case GB_SCALE_OMNI: {
            /* edge-directed bicubic: detect the local dominant direction and
             * bias the sample window along it, which keeps 1-pixel tile seams
             * from smearing while rounding sprite edges.                  */
            for (int y = 0; y < dst_h; y++) {
                float fy = ((float)y + 0.5f) * src_h / dst_h - 0.5f;
                int y0 = (int)fy;
                int ty = (int)((fy - y0) * 64.0f);
                if (y0 < 0) { y0 = 0; ty = 0; }
                if (y0 >= src_h) y0 = src_h - 1;
                for (int x = 0; x < dst_w; x++) {
                    float fx = ((float)x + 0.5f) * src_w / dst_w - 0.5f;
                    int x0 = (int)fx;
                    int tx = (int)((fx - x0) * 64.0f);
                    if (x0 < 0) { x0 = 0; tx = 0; }
                    if (x0 >= src_w) x0 = src_w - 1;

                    /* local edge direction test */
                    int xm = x0 > 0 ? x0 - 1 : 0;
                    int xp = x0 < src_w - 1 ? x0 + 1 : src_w - 1;
                    int ym = y0 > 0 ? y0 - 1 : 0;
                    int yp = y0 < src_h - 1 ? y0 + 1 : src_h - 1;
                    uint32_t c = src[y0 * src_w + x0];
                    int dh = (int)((src[y0 * src_w + xp] & 0xFFFFFF) >
                                   (src[y0 * src_w + xm] & 0xFFFFFF)) -
                             (int)((src[y0 * src_w + xp] & 0xFFFFFF) <
                                   (src[y0 * src_w + xm] & 0xFFFFFF));
                    int dv = (int)((src[yp * src_w + x0] & 0xFFFFFF) >
                                   (src[ym * src_w + x0] & 0xFFFFFF)) -
                             (int)((src[yp * src_w + x0] & 0xFFFFFF) <
                                   (src[ym * src_w + x0] & 0xFFFFFF));

                    uint32_t p = cr_sample(src, src_w, src_h, x0, y0, tx, ty);
                    if (dh > dv) {
                        /* horizontal edge: blend the vertical neighbours less */
                        uint32_t v0 = src[y0 * src_w + x0];
                        uint32_t v1 = src[y0 * src_w + (x0 < src_w - 1 ? x0 + 1 : x0)];
                        p = mix2(p, mix2(v0, v1, (uint32_t)(tx * 4)), 96);
                    } else if (dv > dh) {
                        uint32_t h0 = src[y0 * src_w + x0];
                        uint32_t h1 = src[(y0 < src_h - 1 ? y0 + 1 : y0) * src_w + x0];
                        p = mix2(p, mix2(h0, h1, (uint32_t)(ty * 4)), 96);
                    }
                    (void)c;
                    dst[y * dst_w + x] = p;
                }
            }
            break;
        }

        case GB_SCALE_SCALE3X: {
            /* AdvMAME 3x: only valid when the target is exactly 3x source,
             * otherwise fall back to a 3x render followed by nearest fit.  */
            for (int y = 0; y < src_h; y++) {
                int ym = y > 0 ? y - 1 : y;
                int yp = y < src_h - 1 ? y + 1 : y;
                for (int x = 0; x < src_w; x++) {
                    int xm = x > 0 ? x - 1 : x;
                    int xp = x < src_w - 1 ? x + 1 : x;
                    uint32_t E = src[y  * src_w + x];
                    uint32_t A = src[ym * src_w + xm];
                    uint32_t Bc = src[ym * src_w + x];
                    uint32_t Cc = src[ym * src_w + xp];
                    uint32_t D = src[y  * src_w + xm];
                    uint32_t Fv = src[y  * src_w + xp];
                    uint32_t G = src[yp * src_w + xm];
                    uint32_t H = src[yp * src_w + x];
                    uint32_t I = src[yp * src_w + xp];

                    uint32_t e0 = (D == Bc && Bc != Fv && D != H) ? D : E;
                    uint32_t e1 = (Bc == Fv && Bc != D && Fv != I) ? Fv : E;
                    uint32_t e2 = (D == H && D != Bc && H != I) ? D : E;
                    uint32_t e3 = (H == Fv && D != H && Fv != I) ? Fv : E;
                    uint32_t e4 = E;
                    uint32_t e5 = (Fv == I && Bc != Fv && H != I) ? Fv : E;
                    uint32_t e6 = (D == H && D != Bc && H != I) ? D : E;
                    uint32_t e7 = (H == I && D != H && Fv != I) ? H : E;
                    uint32_t e8 = (Fv == I && Bc != Fv && H != I) ? I : E;
                    (void)A; (void)Cc; (void)G;

                    /* blit into the destination, scaling 3x -> dst as needed */
                    int bx = x * 3, by = y * 3;
                    for (int j = 0; j < 3; j++) {
                        int dy = by + j;
                        if (dy >= dst_h) break;
                        int sy = dy * src_h * 3 / dst_h / 3;
                        (void)sy;
                        for (int i = 0; i < 3; i++) {
                            int dx = bx + i;
                            if (dx >= dst_w) break;
                            uint32_t v = (i == 0 && j == 0) ? e0 :
                                         (i == 1 && j == 0) ? e1 :
                                         (i == 2 && j == 0) ? e2 :
                                         (i == 0 && j == 1) ? e3 :
                                         (i == 1 && j == 1) ? e4 :
                                         (i == 2 && j == 1) ? e5 :
                                         (i == 0 && j == 2) ? e6 :
                                         (i == 1 && j == 2) ? e7 : e8;
                            /* map the 3x block onto the real destination */
                            int ox = (int)((uint64_t)dx * dst_w / (src_w * 3));
                            int oy = (int)((uint64_t)dy * dst_h / (src_h * 3));
                            if (ox < dst_w && oy < dst_h) dst[oy * dst_w + ox] = v;
                        }
                    }
                }
            }
            break;
        }

        default:
            filter = GB_SCALE_NEAREST;
            gb_upscale(src, dst, src_w, src_h, dst_w, dst_h,
                       GB_SCALE_NEAREST, grid, scanline);
            return;
    }

    /* optional LCD grid overlay (subtle darkening on source pixel borders) */
    if (grid && integer_scale) {
        for (int y = 0; y < dst_h; y++) {
            int edge_y = (y % scale_y) == 0;
            for (int x = 0; x < dst_w; x++) {
                int edge_x = (x % scale_x) == 0;
                if (!edge_x && !edge_y) continue;
                uint32_t p = dst[y * dst_w + x];
                uint32_t r = ((p >> 16) & 0xFF) * 224 >> 8;
                uint32_t g = ((p >> 8) & 0xFF) * 224 >> 8;
                uint32_t b = (p & 0xFF) * 224 >> 8;
                dst[y * dst_w + x] = (r << 16) | (g << 8) | b;
            }
        }
    }

    /* optional scanline effect */
    if (scanline && dst_h >= src_h * 2) {
        int step = dst_h / src_h;
        for (int y = step - 1; y < dst_h; y += step) {
            for (int x = 0; x < dst_w; x++) {
                uint32_t p = dst[y * dst_w + x];
                uint32_t r = ((p >> 16) & 0xFF) * 176 >> 8;
                uint32_t g = ((p >> 8) & 0xFF) * 176 >> 8;
                uint32_t b = (p & 0xFF) * 176 >> 8;
                dst[y * dst_w + x] = (r << 16) | (g << 8) | b;
            }
        }
    }
}
