/* Panels drawn in the game's own type (Clarimo UD PE, the button glyphs) instead of gz's pixel font:
 * the same cells as gfx.h's panels (so placement, columns and scrolling stay as they are), each run of
 * text set from its first cell, numbers right-aligned to their last. Plain C on a pixel buffer, so
 * the same code renders on the PC (src/host/nativepreview.c). See docs/NATIVE_UI.md. */
#ifndef NATIVEUI_H
#define NATIVEUI_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

#define NUI_SC_ONE 16 /* scale unit: 16 = one screen pixel per font pixel (nvn.c's SC_ONE) */

/* what the native renderer can't draw yet (the folder, the wrench, the padlock): gz's pixel art.
 * Returns true with the colour of icon c's pixel (x, y) of its 8x10 cell (16x10 for two-cell icons). */
typedef bool (*nui_legacy_fn)(unsigned char c, int x, int y, int style, uint8_t rgb[3]);

/* whether native panels can be drawn (the game's fonts loaded) */
bool nativeui_ready(void);
/* a w x h panel at scale (NUI_SC_ONE units) into dst, rows of w pixels in the swapchain's format
 * (nvn.c's FMT_ values; bytes_per_pixel 4 or 8). false: not drawn (the caller draws it its own way). */
bool nativeui_rasterise(const gfx_panel_t *panel, uint8_t *dst, int scale, int w, int h, int format,
                        int bytes_per_pixel, nui_legacy_fn legacy);

/* the same panel on the GPU (gpuui.c), at (x, y) of the visible area, on a band (opacity: 0..100) fading
 * into the game's picture: between gpuui_begin and gpuui_end. false: not drawn (the caller draws it its
 * own way). */
bool nativeui_gpu_panel(const gfx_panel_t *panel, int x, int y, int w, int h, int scale, int opacity,
                        nui_legacy_fn legacy);
/* the GPU path's size of a panel at scale, in pixels, from gz's grid size of it in *w and *h: the band it
 * sits on, with the content's padding and fades, and the menu laid out the game's way (by its rows' kinds,
 * gfx_row) for its first `rows` rows; and how many of menu.c's rows fit in `space` pixels */
bool nativeui_gpu_size(const gfx_panel_t *panel, int rows, int scale, int *w, int *h);
int nativeui_menu_rows(int space, int scale);

#endif
