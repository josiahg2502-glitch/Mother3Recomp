/* ==========================================================================
 *  jsbind.c -- tiny staging buffers that make the wasm ABI pleasant to call
 *              from JavaScript without pointer juggling.
 *
 *  The web front-end writes a patch or a state blob into one of these static
 *  arenas (via the exported memory) and then calls the matching verb.  This
 *  keeps the exported surface free of malloc and of pointer arithmetic.
 * ========================================================================== */
#include "gb_string.h"
#include "gb_internal.h"

#define JS_STATE_CAP (512u * 1024u)
#define JS_PATCH_CAP (2u * 1024u * 1024u)

static uint8_t g_js_state[JS_STATE_CAP];
static uint8_t g_js_patch[JS_PATCH_CAP];
static uint32_t g_js_state_bytes = 0;

uint8_t *gb_js_state_buf(void) { return g_js_state; }
uint32_t gb_js_state_cap(void) { return JS_STATE_CAP; }
uint8_t *gb_js_patch_buf(void) { return g_js_patch; }
uint32_t gb_js_patch_cap(void) { return JS_PATCH_CAP; }

/* Save the current machine into the staging buffer.  Returns the byte count,
 * or -1 if it would not fit.                                               */
int gb_js_save_state(void)
{
    size_t need = gb_state_size();
    if (need > JS_STATE_CAP) return -1;
    if (gb_save_state(g_js_state, need) != 0) return -1;
    g_js_state_bytes = (uint32_t)need;
    return (int)need;
}

/* Restore from the staging buffer.  `size` must match a prior save. */
int gb_js_load_state(uint32_t size)
{
    if (size == 0) size = g_js_state_bytes;
    if (size == 0 || size > JS_STATE_CAP) return -1;
    return gb_load_state(g_js_state, size);
}

uint32_t gb_js_state_bytes(void) { return g_js_state_bytes; }

/* Apply the patch currently sitting in the patch arena. */
int gb_js_apply_patch(uint32_t size)
{
    if (size == 0 || size > JS_PATCH_CAP) return -1;
    return gb_apply_patch(g_js_patch, size);
}

/* Copy the live framebuffer (XRGB8888) into a caller-provided wasm address.
 * The front-end hands back a pointer it allocated with gb_js_alloc below.  */
static uint8_t g_js_px[GB_LCD_W * GB_LCD_H * 4];

uint8_t *gb_js_pixel_buf(void) { return g_js_px; }

void gb_js_grab_frame(void)
{
    memcpy(g_js_px, g_gb.fb, sizeof(g_js_px));
}

/* ---- audio staging for the web front-end ------------------------------ */
static int16_t g_js_audio[4096 * 2];

int16_t *gb_js_audio_buf(void) { return g_js_audio; }

/* Render `frames` interleaved stereo frames into the audio arena and return
 * how many were actually produced.                                        */
int gb_js_render_audio(uint32_t frames)
{
    if (frames > 4096) frames = 4096;
    return gb_render_audio(g_js_audio, (int)frames);
}
