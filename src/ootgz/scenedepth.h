/* The game's scene depth, so the overlays can hide behind what the game drew in front of them: the air
 * walls' lines behind Link, pixel by pixel (a depth test on the GPU), the enemies' health bars as a whole (they
 * fade out while scenery is in front of the enemy). docs/NATIVE_UI.md, "Hidden behind Link and the scenery". */
#ifndef SCENEDEPTH_H
#define SCENEDEPTH_H

#include <stdbool.h>
#include <stdint.h>

#include "enemyhp.h"
#include "nvnapi.h"

/* nvn.c's wrap_proc: our nvnCommandBufferCopyTextureToTexture for that name, else 0 */
void *scenedepth_wrap(const char *name, void *proc);

/* render thread, at each present before the overlay (nvn.c): sets itself up the first time, fences the
 * texel copies made since the last present, reads the ones the GPU has done */
void scenedepth_present(getproc_t getproc, nvn_device_t *device, nvn_queue_t *queue);

/* render thread: the bars drawn this frame, whose enemies the next depth copies look at */
void scenedepth_ask_bars(const eh_bar_t *bars, int n);
/* render thread: how much of the bar of enemy `id` to show (0 hidden behind scenery .. 1), eased over a
 * moment; 1 while there's no depth to go by (menus, a scene the game draws otherwise) */
float scenedepth_bar_alpha(uintptr_t id, uint64_t now);

/* render thread, before the lines are drawn: our depth texture (*w x *h, the visible area's shape at about the
 * scene's resolution) holding the scene's depth when the game copied it lately; else 0. The lines want it
 * from now on (the copies are only made while they do). *prime: true the first time, when it must be
 * cleared to the far plane before the GPU first copies into it (gpuui_depth) */
const nvn_texture_t *scenedepth_lines(int area_w, int area_h, int *w, int *h, bool *prime);
/* render thread: the priming clear went into this frame's commands */
void scenedepth_primed(void);
/* render thread: the lines can't be hidden after all (no memory for their layer): no more copies */
void scenedepth_lines_give_up(void);

#endif
