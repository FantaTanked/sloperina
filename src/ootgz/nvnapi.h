/* NVN's types as the module uses them: opaque objects in oversized storage (the sizes are NVN 1's;
 * NVN 2 may have grown them), and the few plain structs NVN takes by pointer. Shared by nvn.c
 * (hooks, copies) and gpuui.c (drawing with the game's UI shader). docs/NATIVE_UI.md has the NVN 2
 * details this relies on. */
#ifndef NVNAPI_H
#define NVNAPI_H

#include <stdint.h>

typedef struct nvn_device nvn_device_t;
typedef struct nvn_queue nvn_queue_t;
typedef struct nvn_window nvn_window_t;
typedef struct nvn_texture nvn_texture_t;
typedef uint64_t nvn_addr_t;
typedef uint64_t nvn_handle_t;
typedef struct { int32_t x, y, z, w, h, d; } nvn_region_t;
typedef struct { int32_t x, y, w, h; } nvn_rect_t;

#define OBJ_STORAGE 0x400 /* NVN 1: pool builder 0x40, pool 0x100, command buffer 0xA0, sync 0x40 */
typedef struct { _Alignas(16) uint8_t b[OBJ_STORAGE]; } nvn_obj_t;

typedef void *(*getproc_t)(nvn_device_t *dev, const char *name);

#endif
