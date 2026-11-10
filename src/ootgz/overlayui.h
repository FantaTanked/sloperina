/* The air walls' outlines and the enemies' health in the game's style, on the GPU (overlayui.c). */
#ifndef OVERLAYUI_H
#define OVERLAYUI_H

#include <stdbool.h>

enum { OVERLAY_LINES = 1, OVERLAY_BARS = 2, OVERLAY_PAUSE = 4 };

/* render thread, in a gpuui frame over a w x h visible picture, with gfx_pause_icon's icon (0: none): what
 * it drew (OVERLAY_* bits) */
int overlayui_draw(int w, int h, int pause_icon);

/* whether the overlays are drawn here, on the GPU: the game's style, its shader and type loaded, and the
 * GPU path not failed (nvn.c sets overlayui_failed if it does). The game thread then makes lines and bars
 * (airwall.c, enemyhp.c), else the rectangles nvn.c clears. */
extern volatile bool overlayui_failed;
bool overlayui_on_gpu(void);

#endif
