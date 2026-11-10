/* Drawing on the GPU with the game's own UI shader (uishader.c): quads in their corner colours (the
 * dark bands with their fades, outlines) and glyphs from the game's fonts, alpha-blended over the
 * game's picture in the frame being presented, with no read-back. Used by nativeui.c from nvn.c's
 * present hook; docs/NATIVE_UI.md has the details. */
#ifndef GPUUI_H
#define GPUUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gamefont.h"
#include "nvnapi.h"

typedef struct { float r, g, b, a; } gpu_colour_t;

/* render thread, once (after nvn.c has its device): false if the GPU path can't be used */
bool gpuui_init(getproc_t getproc, nvn_device_t *device);
bool gpuui_ready(void);

/* one frame: begin, add quads (in drawing order), end records them into cmd (between nvn.c's
 * BeginRecording and EndRecording). area: the visible part of the target, in its pixels; coordinates
 * passed to the quads are relative to it. slot: nvn.c's frame slot (its fence guards our memory). */
/* whether the next gpuui_begin starts the glyph atlas afresh (it was full): the caller first waits until
 * the GPU is done with the frames in flight, whose copies read the atlas's staging rows */
bool gpuui_atlas_resetting(void);
void gpuui_begin(nvn_texture_t *target, nvn_rect_t area, int texture_height, bool origin_lower_left, int slot);
/* a w x h rectangle at (x, y); corners: (left, top), (right, top), (left, bottom), (right, bottom) */
void gpuui_rect(float x, float y, float w, float h, const gpu_colour_t corners[4]);
/* the quads made from now on (until it's set again, or the frame's end): scaled by s about (ox, oy) */
void gpuui_scale(float s, float ox, float oy);
void gpuui_fill(float x, float y, float w, float h, gpu_colour_t colour);
/* a rectangle turned by angle (radians) about its centre */
void gpuui_rect_turned(float cx, float cy, float w, float h, float angle, gpu_colour_t colour);
/* a line from (x0, y0) to (x1, y1), `width` across, its sides soft (half its width fading out each side
 * of a solid middle as wide as `width`): smooth at any slope */
void gpuui_line(float x0, float y0, float x1, float y1, float width, gpu_colour_t colour);
/* a glyph with its pen at x and baseline at y; returns its advance in pixels (0 if it has none) */
float gpuui_glyph(gf_face_t face, int em, uint32_t codepoint, float x, float baseline, gpu_colour_t colour);
/* a shape of the caller's: a w x h coverage image (0..255 per pixel, rows `stride` apart) painted once by
 * paint(..., arg) and kept in the atlas under (id, w, h) (so arg must give the same image each time),
 * drawn at (x, y) in colour */
typedef void (*gpu_paint_t)(uint8_t *coverage, int w, int h, int stride, const void *arg);
void gpuui_shape(uint32_t id, int w, int h, gpu_paint_t paint, const void *arg, float x, float y, gpu_colour_t colour);
/* The game's own pictures (gameimage.c), as textures straight from its packaged (BNTX) data: images
 * in mem (4 KiB-aligned, `size` bytes, a multiple of 4 KiB, kept), each at its offset in its packaged
 * layout, numbered from `first`. Game thread, once a batch (0 to GPU_IMAGES - 1, each its own memory);
 * the render thread makes a batch's textures when one of them is first drawn. */
#define GPU_IMAGES 80
typedef struct {
  size_t offset, size;       /* size 0: not there */
  int width, height, format; /* NVN's format */
  uint8_t layout[8];         /* NVNpackagedTextureLayout */
  uint8_t swizzle[4];        /* NVN's, for r, g, b, a */
} gpu_image_t;
void gpuui_images_publish(int batch, int first, uint8_t *mem, size_t size, const gpu_image_t *images, int n);
/* The items' icons (gameimage.c), small and in RGBA: an atlas of GPU_ICON_COLS x GPU_ICON_ROWS slots of
 * GPU_ICON_PX square, row by row, at mem's start (4 KiB-aligned, `size` bytes, kept), the rest of mem room
 * for its texture. Game thread: published once, then each slot as it's filled; the render thread brings
 * the filled ones up before the frame's drawing. Drawn as picture GPU_ICONS_IMAGE (gpuui_icon). */
#define GPU_ICON_PX 64
#define GPU_ICON_COLS 8
#define GPU_ICON_ROWS 7
#define GPU_ICONS_IMAGE (GPU_IMAGES - 1)
void gpuui_icons_publish(uint8_t *mem, size_t size);
void gpuui_icon_filled(int slot);
/* render thread: whether icon `slot` can be drawn; drawn as a w x h quad at (x, y), its part crop (fractions
 * of the slot: x0, y0, x1, y1; NULL: all of it), times colour */
bool gpuui_icon_ready(int slot);
void gpuui_icon(int slot, float x, float y, float w, float h, const float crop[4], gpu_colour_t colour);
/* render thread: whether picture i can be drawn; drawn as a w x h quad at (x, y), its part crop (s0, t0,
 * s1, t1), times colour */
bool gpuui_image_ready(int i);
void gpuui_image(int i, float x, float y, float w, float h, const float crop[4], gpu_colour_t colour);
/* the same with its colours mapped, as the game's materials do: the texture's black to `black`, its white to
 * `white`, alpha as the colours (0 to black.a, 1 to white.a, then clamped), at `alpha` (up to 8 mappings a
 * frame) */
void gpuui_image_mapped(int i, float x, float y, float w, float h, const float crop[4], gpu_colour_t black,
                        gpu_colour_t white, float alpha);
void gpuui_end(nvn_obj_t *cmd, const void *game_texture_pool, const void *game_sampler_pool);

#endif
