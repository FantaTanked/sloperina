/* The GPU path (see gpuui.h). How it uses the game's MergeShader (docs/NATIVE_UI.md has the sources):
 *
 *   vertices      one strip of 4, (0,0) (1,0) (0,1) (1,1), as both attributes (_p0 position, _u0 corner),
 *                 RG32F at offset 0, stride 8
 *   VS uniform 1  `view`: a column-major 4x4 matrix; here the visible area's pixels (y down) to clip space
 *   VS uniform 2  64 zero bytes (word 15 would turn on a shadow texture-coordinate remap)
 *   VS storage 6  one 46-word element per quad: a row-major 3x4 matrix, the four corners' colours, the size,
 *                 the four corners' texture coordinates, a 2x3 texture matrix
 *   FS uniform 4  `UiMaterial`: colour scale 1, offset 0, alpha and RGB multipliers 1, flags 0x2 (discard
 *                 where alpha is 0)
 *   FS texture 8  the glyph atlas: R8 coverage seen as (1, 1, 1, coverage)
 * Output alpha is straight, so blending is SRC_ALPHA / ONE_MINUS_SRC_ALPHA. Each quad is one draw with
 * its element bound. The atlas only grows within a frame: glyphs are added as they're asked for and the
 * rows they touched are copied up before the frame's draws. Nothing here waits for the GPU: nvn.c's
 * slots (each guarded by its fence) keep a frame's elements and view until the GPU is done with them. */
#include "gpuui.h"

#include <stddef.h>

#include "sys.h"
#include "uishader.h"

void *aligned_alloc(size_t align, size_t size); /* from main */
void free(void *p);                             /* from main */
float sinf(float x);                            /* from sdk */
float cosf(float x);
float sqrtf(float x);

#define PAGE 0x1000
#define SLOTS 2               /* nvn.c's */
#define MAX_QUADS 4096        /* a frame's quads (a glyph is two: its shadow and itself) */
#define ELEMENT_SIZE 0x100    /* 46 words, at the storage-buffer alignment */
#define ELEMENT_WORDS 46
#define UNIFORM_SIZE 0x100
#define MAX_MATERIALS 8       /* a frame's colour mappings (gpuui_image_mapped), besides the plain one */
#define POOL_ENTRIES 512      /* texture / sampler descriptors: the first ones (256) are NVN's own */
#define ATLAS_W 1024
#define ATLAS_H 1024
#define CODE_ALIGN 0x100
#define CODE_PADDING 0x800    /* after the last code area (NVN 2's shader pool padding) */
#define CMD_RESERVE 0x4000    /* command memory kept free (nvn.c's own commands, the restore) */

/* NVN values (docs/NATIVE_UI.md) */
enum {
  POOL_CPU_UNCACHED = 0x02, POOL_CPU_CACHED = 0x04, POOL_GPU_CACHED = 0x20, POOL_SHADER_CODE = 0x40,
  STAGE_VERTEX = 0, STAGE_FRAGMENT = 1, STAGES_GRAPHICS = 0xFF,
  BLEND_ONE = 2, BLEND_SRC_ALPHA = 5, BLEND_ONE_MINUS_SRC_ALPHA = 6, BLEND_ADD = 1,
  FACE_NONE = 0, PRIM_TRIANGLE_STRIP = 5,
  FMT_R8 = 0x01, FMT_RG32F = 0x16, FMT_ASTC_4X4 = 0x79, TARGET_2D = 1,
  SWIZZLE_ONE = 1, SWIZZLE_R = 2, FILTER_LINEAR = 1, WRAP_CLAMP_TO_EDGE = 7,
  BARRIER_ORDER_PRIMITIVES = 1, BARRIER_ORDER_FRAGMENTS = 2, BARRIER_INVALIDATE_TEXTURE = 0x10,
  BARRIER_INVALIDATE_TEXTURE_DESCRIPTOR = 0x40, BARRIER_INVALIDATE_ZCULL = 0x80,
  DEPTH_FUNC_LESS = 2, DEPTH_FUNC_LEQUAL = 4, DEPTH_FUNC_ALWAYS = 8, DEPTH_MODE_NEAR_IS_MINUS_W = 0,
  TEXTURE_COMPRESSIBLE = 0x8, POOL_CPU_NO_ACCESS = 0x01, POOL_COMPRESSIBLE = 0x80,
  CLEAR_ALL_CHANNELS = 0xF,
  INFO_TEXTURE_DESCRIPTOR_SIZE = 0xF, INFO_SAMPLER_DESCRIPTOR_SIZE = 0x10, INFO_RESERVED_TEXTURES = 0x11,
  INFO_RESERVED_SAMPLERS = 0x12,
};

typedef struct { uint64_t data; const void *control; } shader_data_t;

static struct {
  void (*pool_builder_defaults)(nvn_obj_t *);
  void (*pool_builder_device)(nvn_obj_t *, nvn_device_t *);
  void (*pool_builder_flags)(nvn_obj_t *, int);
  void (*pool_builder_storage)(nvn_obj_t *, void *, size_t);
  bool (*pool_init)(nvn_obj_t *, const nvn_obj_t *);
  nvn_addr_t (*pool_address)(const nvn_obj_t *);
  bool (*program_init)(nvn_obj_t *, nvn_device_t *);
  bool (*program_set_shaders)(nvn_obj_t *, int, const shader_data_t *);
  void (*blend_defaults)(void *);
  void (*blend_target)(void *, int);
  void (*blend_func)(void *, int, int, int, int);
  void (*blend_equation)(void *, int, int);
  void (*colour_defaults)(void *);
  void (*colour_blend_enable)(void *, int, bool);
  void (*channel_defaults)(void *);
  void (*multisample_defaults)(void *);
  void (*polygon_defaults)(void *);
  void (*polygon_cull)(void *, int);
  void (*depth_defaults)(void *);
  void (*depth_test)(void *, bool);
  void (*depth_write)(void *, bool);
  void (*attrib_defaults)(void *);
  void (*attrib_format)(void *, int, ptrdiff_t);
  void (*attrib_stream)(void *, int);
  void (*stream_defaults)(void *);
  void (*stream_stride)(void *, ptrdiff_t);
  void (*tb_device)(void *, nvn_device_t *);
  void (*tb_defaults)(void *);
  void (*tb_target)(void *, int);
  void (*tb_format)(void *, int);
  void (*tb_size)(void *, int, int);
  void (*tb_levels)(void *, int);
  void (*tb_flags)(void *, int);
  void (*tb_swizzle)(void *, int, int, int, int);
  size_t (*tb_storage_size)(const void *);
  size_t (*tb_storage_align)(const void *);
  void (*tb_storage)(void *, const nvn_obj_t *, ptrdiff_t);
  bool (*texture_init)(void *, const void *);
  void (*sb_device)(void *, nvn_device_t *);
  void (*sb_defaults)(void *);
  void (*sb_filter)(void *, int, int);
  void (*sb_wrap)(void *, int, int, int);
  bool (*sampler_init)(void *, const void *);
  bool (*texture_pool_init)(void *, const nvn_obj_t *, ptrdiff_t, int);
  bool (*sampler_pool_init)(void *, const nvn_obj_t *, ptrdiff_t, int);
  void (*texture_pool_register)(const void *, int, const void *, const void *);
  void (*sampler_pool_register)(const void *, int, const void *);
  nvn_handle_t (*texture_handle)(const nvn_device_t *, int, int);
  void (*cmd_render_enable)(nvn_obj_t *, bool);
  void (*cmd_targets)(nvn_obj_t *, int, nvn_texture_t *const *, const void *const *, const void *, const void *);
  void (*cmd_viewport)(nvn_obj_t *, int, int, int, int);
  void (*cmd_scissor)(nvn_obj_t *, int, int, int, int);
  void (*cmd_program)(nvn_obj_t *, const nvn_obj_t *, int);
  void (*cmd_blend)(nvn_obj_t *, const void *);
  void (*cmd_colour)(nvn_obj_t *, const void *);
  void (*cmd_channels)(nvn_obj_t *, const void *);
  void (*cmd_multisample)(nvn_obj_t *, const void *);
  void (*cmd_polygon)(nvn_obj_t *, const void *);
  void (*cmd_depth)(nvn_obj_t *, const void *);
  void (*cmd_attribs)(nvn_obj_t *, int, const void *);
  void (*cmd_streams)(nvn_obj_t *, int, const void *);
  void (*cmd_vertex_buffer)(nvn_obj_t *, int, nvn_addr_t, size_t);
  void (*cmd_uniform)(nvn_obj_t *, int, int, nvn_addr_t, size_t);
  void (*cmd_storage)(nvn_obj_t *, int, int, nvn_addr_t, size_t);
  void (*cmd_texture_pool)(nvn_obj_t *, const void *);
  void (*cmd_sampler_pool)(nvn_obj_t *, const void *);
  void (*cmd_texture)(nvn_obj_t *, int, int, nvn_handle_t);
  void (*cmd_draw)(nvn_obj_t *, int, int, int);
  void (*cmd_copy_to_texture)(nvn_obj_t *, nvn_addr_t, const void *, const void *, const nvn_region_t *, int);
  void (*cmd_barrier)(nvn_obj_t *, int);
  void (*cmd_copy_stride)(nvn_obj_t *, ptrdiff_t);
  size_t (*cmd_free)(const nvn_obj_t *);
} g;

static const char *const g_names[] = {
  "nvnMemoryPoolBuilderSetDefaults", "nvnMemoryPoolBuilderSetDevice", "nvnMemoryPoolBuilderSetFlags",
  "nvnMemoryPoolBuilderSetStorage", "nvnMemoryPoolInitialize", "nvnMemoryPoolGetBufferAddress",
  "nvnProgramInitialize", "nvnProgramSetShaders",
  "nvnBlendStateSetDefaults", "nvnBlendStateSetBlendTarget", "nvnBlendStateSetBlendFunc", "nvnBlendStateSetBlendEquation",
  "nvnColorStateSetDefaults", "nvnColorStateSetBlendEnable", "nvnChannelMaskStateSetDefaults",
  "nvnMultisampleStateSetDefaults", "nvnPolygonStateSetDefaults", "nvnPolygonStateSetCullFace",
  "nvnDepthStencilStateSetDefaults", "nvnDepthStencilStateSetDepthTestEnable", "nvnDepthStencilStateSetDepthWriteEnable",
  "nvnVertexAttribStateSetDefaults", "nvnVertexAttribStateSetFormat", "nvnVertexAttribStateSetStreamIndex",
  "nvnVertexStreamStateSetDefaults", "nvnVertexStreamStateSetStride",
  "nvnTextureBuilderSetDevice", "nvnTextureBuilderSetDefaults", "nvnTextureBuilderSetTarget", "nvnTextureBuilderSetFormat",
  "nvnTextureBuilderSetSize2D", "nvnTextureBuilderSetLevels", "nvnTextureBuilderSetFlags", "nvnTextureBuilderSetSwizzle",
  "nvnTextureBuilderGetStorageSize", "nvnTextureBuilderGetStorageAlignment", "nvnTextureBuilderSetStorage",
  "nvnTextureInitialize",
  "nvnSamplerBuilderSetDevice", "nvnSamplerBuilderSetDefaults", "nvnSamplerBuilderSetMinMagFilter",
  "nvnSamplerBuilderSetWrapMode", "nvnSamplerInitialize",
  "nvnTexturePoolInitialize", "nvnSamplerPoolInitialize", "nvnTexturePoolRegisterTexture", "nvnSamplerPoolRegisterSampler",
  "nvnDeviceGetTextureHandle",
  "nvnCommandBufferSetRenderEnable", "nvnCommandBufferSetRenderTargets", "nvnCommandBufferSetViewport",
  "nvnCommandBufferSetScissor", "nvnCommandBufferBindProgram", "nvnCommandBufferBindBlendState",
  "nvnCommandBufferBindColorState", "nvnCommandBufferBindChannelMaskState", "nvnCommandBufferBindMultisampleState",
  "nvnCommandBufferBindPolygonState", "nvnCommandBufferBindDepthStencilState", "nvnCommandBufferBindVertexAttribState",
  "nvnCommandBufferBindVertexStreamState", "nvnCommandBufferBindVertexBuffer", "nvnCommandBufferBindUniformBuffer",
  "nvnCommandBufferBindStorageBuffer", "nvnCommandBufferSetTexturePool", "nvnCommandBufferSetSamplerPool",
  "nvnCommandBufferBindTexture", "nvnCommandBufferDrawArrays", "nvnCommandBufferCopyBufferToTexture",
  "nvnCommandBufferBarrier", "nvnCommandBufferSetCopyRowStride", "nvnCommandBufferGetCommandMemoryFree",
};
_Static_assert(sizeof(g_names) / sizeof(*g_names) == sizeof(g) / sizeof(void *), "gpuui function table");

/* only for the game's pictures (gpuui_image): without them, there are none */
static struct {
  void (*tb_packaged_layout)(void *, const void *);
  void (*pool_flush)(const nvn_obj_t *, ptrdiff_t, size_t);
  void (*device_integer)(const nvn_device_t *, int, int *);
} gi;
static const char *const gi_names[] = { "nvnTextureBuilderSetPackagedTextureLayout", "nvnMemoryPoolFlushMappedRange",
                                        "nvnDeviceGetInteger" };

/* only for hiding the lines behind Link (gpuui_depth): without them, the lines show through him */
static struct {
  void (*depth_func)(void *, int);
  void (*cmd_depth_range)(nvn_obj_t *, float, float);
  void (*cmd_clear_depth)(nvn_obj_t *, float, bool, int, int);
  int (*depth_mode)(const nvn_device_t *);
  void (*cmd_clear_colour)(nvn_obj_t *, int, const float *, int);
  void (*view_defaults)(void *);
  void (*view_swizzle)(void *, int, int, int, int);
} gd;
static const char *const gd_names[] = { "nvnDepthStencilStateSetDepthFunc", "nvnCommandBufferSetDepthRange",
                                        "nvnCommandBufferClearDepthStencil", "nvnDeviceGetDepthMode",
                                        "nvnCommandBufferClearColor", "nvnTextureViewSetDefaults",
                                        "nvnTextureViewSetSwizzle" };
_Static_assert(sizeof(gd_names) / sizeof(*gd_names) == sizeof(gd) / sizeof(void *), "gpuui depth functions");
static bool depth_usable;
_Static_assert(sizeof(gi_names) / sizeof(*gi_names) == sizeof(gi) / sizeof(void *), "gpuui image functions");

/* --- objects and memory ------------------------------------------------------------------------------ */

static nvn_device_t *device;
static bool ready;

static nvn_obj_t code_pool, data_pool;
static uint8_t *code_mem, *data_mem;
static size_t code_size;
static nvn_addr_t code_gpu, data_gpu;
static nvn_obj_t programs[UI_PROGRAMS];
static shader_data_t shader_data[UI_PROGRAMS][2];
static nvn_obj_t blend, colour, channels, multisample, polygon, depth, depth_tested, depth_beyond, depth_far;
static _Alignas(16) uint8_t attribs[2 * 4]; /* NVN 2's vertex attribute state is 4 bytes (the game's stride) */
static _Alignas(16) uint8_t streams[8];     /* ... its vertex stream state 8 */
static nvn_obj_t atlas_builder, atlas, sampler_builder, sampler, texture_pool, sampler_pool;
static nvn_handle_t atlas_handle;
/* the descriptor pools: each descriptor's size, and the first IDs (NVN's own); ours come after them */
static int texture_descriptor = 32, sampler_descriptor = 32, our_texture_id = 256, our_sampler_id = 256;

/* NVN's answer for an item, if it's sane, else NVN 2's (its sdk's table) */
static int device_info(int item, int fallback, int most)
{
  int v = 0;
  if (gi.device_integer)
    gi.device_integer(device, item, &v);
  return v > 0 && v <= most ? v : fallback;
}

/* the data pool's layout */
static size_t off_vertices, off_view, off_shadow, off_material, off_mask, off_materials, off_textures, off_samplers, off_elements,
  off_staging, off_atlas, data_size;

static size_t round_up(size_t n, size_t to) { return (n + to - 1) / to * to; }

static bool make_pool(nvn_obj_t *pool, void *mem, size_t size, int flags)
{
  nvn_obj_t builder;
  __builtin_memset(&builder, 0, sizeof(builder));
  g.pool_builder_defaults(&builder);
  g.pool_builder_device(&builder, device);
  g.pool_builder_flags(&builder, flags);
  g.pool_builder_storage(&builder, mem, size);
  return g.pool_init(pool, &builder);
}

/* the shader code areas in a pool of their own, each 256-aligned, with the padding NVN 2 wants after them */
static bool make_programs(void)
{
  code_size = 0;
  for (int p = 0; p < UI_PROGRAMS; p++) {
    const uishader_program_t *program = uishader_program(p);
    if (!program)
      return false;
    code_size += round_up(program->vertex.code_size, CODE_ALIGN) + round_up(program->fragment.code_size, CODE_ALIGN);
  }
  code_size = round_up(code_size + CODE_PADDING, PAGE);
  code_mem = aligned_alloc(PAGE, code_size);
  if (!code_mem)
    return false;
  /* written once it's a pool (uncached): nothing of it waits in the CPU's cache */
  if (!make_pool(&code_pool, code_mem, code_size, POOL_CPU_UNCACHED | POOL_GPU_CACHED | POOL_SHADER_CODE))
    return false;
  __builtin_memset(code_mem, 0, code_size);
  size_t at = 0;
  for (int p = 0; p < UI_PROGRAMS; p++) {
    const uishader_program_t *program = uishader_program(p);
    const uishader_stage_t *stages[2] = { &program->vertex, &program->fragment };
    for (int s = 0; s < 2; s++) {
      __builtin_memcpy(code_mem + at, stages[s]->code, stages[s]->code_size);
      shader_data[p][s].data = at; /* made an address below, once the pool has one */
      shader_data[p][s].control = stages[s]->control;
      at += round_up(stages[s]->code_size, CODE_ALIGN);
    }
  }
  code_gpu = g.pool_address(&code_pool);
  for (int p = 0; p < UI_PROGRAMS; p++) {
    shader_data[p][0].data += code_gpu;
    shader_data[p][1].data += code_gpu;
    if (!g.program_init(&programs[p], device) || !g.program_set_shaders(&programs[p], 2, shader_data[p]))
      return false;
  }
  return true;
}

static void make_states(void)
{
  g.blend_defaults(&blend);
  g.blend_target(&blend, 0);
  g.blend_func(&blend, BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA, BLEND_ONE, BLEND_ONE_MINUS_SRC_ALPHA);
  g.blend_equation(&blend, BLEND_ADD, BLEND_ADD);
  g.colour_defaults(&colour);
  g.colour_blend_enable(&colour, 0, true);
  g.channel_defaults(&channels);
  g.multisample_defaults(&multisample);
  g.polygon_defaults(&polygon);
  g.polygon_cull(&polygon, FACE_NONE);
  g.depth_defaults(&depth);
  g.depth_test(&depth, false);
  g.depth_write(&depth, false);
  if (depth_usable) { /* the lines behind Link (gpuui_depth): */
    nvn_obj_t *states[3] = { &depth_tested, &depth_beyond, &depth_far };
    static const int funcs[3] = { DEPTH_FUNC_LEQUAL, DEPTH_FUNC_LESS, DEPTH_FUNC_ALWAYS };
    for (int i = 0; i < 3; i++) {
      g.depth_defaults(states[i]);
      g.depth_test(states[i], true);
      g.depth_write(states[i], i == 2); /* only the far depth is written (where the scene isn't Link) */
      gd.depth_func(states[i], funcs[i]);
    }
  }
  for (int i = 0; i < 2; i++) {
    g.attrib_defaults(attribs + 4 * i);
    g.attrib_format(attribs + 4 * i, FMT_RG32F, 0); /* both read the corner: _p0 scaled by the size, _u0 as is */
    g.attrib_stream(attribs + 4 * i, 0);
  }
  g.stream_defaults(streams);
  g.stream_stride(streams, 8);
}

/* the data pool: the vertices, the uniforms, the descriptor pools, the elements, the atlas */
static bool make_data(void)
{
  g.tb_device(&atlas_builder, device);
  g.tb_defaults(&atlas_builder);
  g.tb_target(&atlas_builder, TARGET_2D);
  g.tb_format(&atlas_builder, FMT_R8);
  g.tb_size(&atlas_builder, ATLAS_W, ATLAS_H);
  g.tb_levels(&atlas_builder, 1);
  g.tb_flags(&atlas_builder, 0); /* block-linear: the copy lays the texels out */
  g.tb_swizzle(&atlas_builder, SWIZZLE_ONE, SWIZZLE_ONE, SWIZZLE_ONE, SWIZZLE_R);
  size_t atlas_size = g.tb_storage_size(&atlas_builder);
  size_t atlas_align = g.tb_storage_align(&atlas_builder);
  if (atlas_align < PAGE)
    atlas_align = PAGE;

  size_t at = 0;
  off_vertices = at, at += UNIFORM_SIZE;
  off_view = at, at += UNIFORM_SIZE * SLOTS;
  off_shadow = at, at += UNIFORM_SIZE;
  off_material = at, at += UNIFORM_SIZE;
  off_mask = at, at += UNIFORM_SIZE;
  off_materials = at, at += (size_t)SLOTS * MAX_MATERIALS * UNIFORM_SIZE;
  at = round_up(at, PAGE);
  texture_descriptor = device_info(INFO_TEXTURE_DESCRIPTOR_SIZE, 32, 256);
  sampler_descriptor = device_info(INFO_SAMPLER_DESCRIPTOR_SIZE, 32, 256);
  our_texture_id = device_info(INFO_RESERVED_TEXTURES, 256, POOL_ENTRIES - 2 - GPU_IMAGES); /* the atlas, the
                                                                                    pictures, the lines' layer */
  our_sampler_id = device_info(INFO_RESERVED_SAMPLERS, 256, POOL_ENTRIES - 1);
  off_textures = at, at += (size_t)POOL_ENTRIES * texture_descriptor;
  at = round_up(at, PAGE);
  off_samplers = at, at += (size_t)POOL_ENTRIES * sampler_descriptor;
  at = round_up(at, PAGE);
  off_elements = at, at += (size_t)SLOTS * MAX_QUADS * ELEMENT_SIZE;
  off_staging = at, at += (size_t)ATLAS_W * ATLAS_H;
  at = round_up(at, atlas_align);
  off_atlas = at, at += atlas_size;
  data_size = round_up(at, PAGE);
  data_mem = aligned_alloc(PAGE, data_size);
  if (!data_mem)
    return false;
  /* written once it's a pool (uncached): nothing of it waits in the CPU's cache */
  if (!make_pool(&data_pool, data_mem, data_size, POOL_CPU_UNCACHED | POOL_GPU_CACHED))
    return false;
  __builtin_memset(data_mem, 0, off_elements);
  data_gpu = g.pool_address(&data_pool);

  static const float corners[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
  __builtin_memcpy(data_mem + off_vertices, corners, sizeof(corners));
  float material[16] = { 1, 1, 1, 1, 0, 0, 0, 0, 1, 1 };
  uint32_t discard_clear = 0x2; /* flags, an int: discard where alpha is 0 */
  __builtin_memcpy(&material[10], &discard_clear, 4);
  __builtin_memcpy(data_mem + off_material, material, sizeof(material));
  /* Link's mask (the lines' layer, sampled as 1, 1, 1, R) to its opposite's alpha: 0 (discarded) on him */
  float mask[16] = { 0, 0, 0, -1, 1, 1, 1, 1, 1, 1 };
  __builtin_memcpy(&mask[10], &discard_clear, 4);
  __builtin_memcpy(data_mem + off_mask, mask, sizeof(mask));

  g.tb_storage(&atlas_builder, &data_pool, (ptrdiff_t)off_atlas);
  if (!g.texture_init(&atlas, &atlas_builder))
    return false;
  g.sb_device(&sampler_builder, device);
  g.sb_defaults(&sampler_builder);
  g.sb_filter(&sampler_builder, FILTER_LINEAR, FILTER_LINEAR);
  g.sb_wrap(&sampler_builder, WRAP_CLAMP_TO_EDGE, WRAP_CLAMP_TO_EDGE, WRAP_CLAMP_TO_EDGE);
  if (!g.sampler_init(&sampler, &sampler_builder))
    return false;
  if (!g.texture_pool_init(&texture_pool, &data_pool, (ptrdiff_t)off_textures, POOL_ENTRIES) ||
      !g.sampler_pool_init(&sampler_pool, &data_pool, (ptrdiff_t)off_samplers, POOL_ENTRIES))
    return false;
  g.texture_pool_register(&texture_pool, our_texture_id, &atlas, 0);
  g.sampler_pool_register(&sampler_pool, our_sampler_id, &sampler);
  atlas_handle = g.texture_handle(device, our_texture_id, our_sampler_id);
  return true;
}

bool gpuui_init(getproc_t getproc, nvn_device_t *dev)
{
  if (ready)
    return true;
  device = dev;
  void **slots = (void **)&g;
  for (unsigned i = 0; i < sizeof(g_names) / sizeof(*g_names); i++) {
    slots[i] = getproc(dev, g_names[i]);
    if (!slots[i]) {
      log_printf("gpu drawing: NVN has no %s", g_names[i]);
      return false;
    }
  }
  void **optional = (void **)&gi;
  for (unsigned i = 0; i < sizeof(gi_names) / sizeof(*gi_names); i++)
    optional[i] = getproc(dev, gi_names[i]);
  void **for_depth = (void **)&gd;
  depth_usable = true;
  for (unsigned i = 0; i < sizeof(gd_names) / sizeof(*gd_names); i++)
    depth_usable &= (for_depth[i] = getproc(dev, gd_names[i])) != 0;
  if (!make_programs()) {
    log_printf("gpu drawing: the ui shader's programs didn't build");
    return false;
  }
  make_states();
  if (!make_data()) {
    log_printf("gpu drawing: no memory for its buffers and atlas");
    return false;
  }
  ready = true;
  log_printf("gpu drawing ready (%u KiB, shader code %u KiB)", (unsigned)(data_size / 1024),
             (unsigned)(code_size / 1024));
  return true;
}

bool gpuui_ready(void)
{
  return ready;
}

/* --- the glyph atlas ------------------------------------------------------------------------------- */

#define ATLAS_SLOTS 2048 /* a power of two */
#define GLYPH_PAD 1      /* empty texels around each glyph, so linear filtering doesn't bleed */

typedef struct {
  uint32_t codepoint;
  uint16_t em;
  uint8_t face, state; /* 0 empty, 1 placed, 2 nothing to draw */
  uint16_t x, y, w, h;
  int16_t x0, y0;
  float advance;
} atlas_slot_t;

static atlas_slot_t atlas_map[ATLAS_SLOTS];
static int shelf_x, shelf_y, shelf_h, atlas_count;
static int dirty_top = ATLAS_H, dirty_bottom = 0;

static void atlas_reset(void)
{
  for (int i = 0; i < ATLAS_SLOTS; i++)
    atlas_map[i].state = 0;
  shelf_x = shelf_y = shelf_h = atlas_count = 0;
}

#define SHAPE_FACE 0xFF /* atlas_map's face for gpuui_shape's images (not a gf_face_t) */

/* the slot for (face, em, cp): found, or the empty one it goes in (state 0); NULL: the map is full */
static atlas_slot_t *atlas_find(uint8_t face, int em, uint32_t cp)
{
  uint32_t h = cp * 2654435761u ^ (uint32_t)em * 40503u ^ (uint32_t)face * 0x9E3779B9u;
  unsigned i = (h ^ h >> 15) & (ATLAS_SLOTS - 1);
  for (int probe = 0; probe < ATLAS_SLOTS; probe++, i = (i + 1) & (ATLAS_SLOTS - 1)) {
    atlas_slot_t *s = &atlas_map[i];
    if (!s->state || (s->codepoint == cp && s->em == em && s->face == face))
      return s;
  }
  return 0;
}

/* room for a w x h image in the slot s (empty), with an empty border, cleared in the staging copy;
 * false: the atlas is full (gpuui_begin starts it afresh next frame) */
static bool atlas_place(atlas_slot_t *s, uint8_t face, int em, uint32_t cp, int w, int h)
{
  int pw = w + 2 * GLYPH_PAD, ph = h + 2 * GLYPH_PAD;
  if (atlas_count >= ATLAS_SLOTS * 3 / 4 || pw > ATLAS_W || ph > ATLAS_H)
    return false;
  if (shelf_x + pw > ATLAS_W) { /* the next shelf */
    shelf_y += shelf_h;
    shelf_x = shelf_h = 0;
  }
  if (shelf_y + ph > ATLAS_H)
    return false;
  *s = (atlas_slot_t){ cp, (uint16_t)em, face, 1, (uint16_t)(shelf_x + GLYPH_PAD), (uint16_t)(shelf_y + GLYPH_PAD),
                       (uint16_t)w, (uint16_t)h, 0, 0, 0 };
  if (!w || !h)
    s->state = 2;
  uint8_t *staging = data_mem + off_staging;
  for (int y = 0; y < ph; y++)
    __builtin_memset(staging + (size_t)(shelf_y + y) * ATLAS_W + shelf_x, 0, (size_t)pw);
  if (shelf_y < dirty_top)
    dirty_top = shelf_y;
  if (shelf_y + ph > dirty_bottom)
    dirty_bottom = shelf_y + ph;
  shelf_x += pw;
  if (ph > shelf_h)
    shelf_h = ph;
  atlas_count++;
  return true;
}

static const atlas_slot_t *atlas_glyph(gf_face_t face, int em, uint32_t cp)
{
  atlas_slot_t *s = atlas_find((uint8_t)face, em, cp);
  if (s && s->state)
    return s;
  const gf_glyph_t *glyph = gamefont_glyph(face, em, cp);
  if (!glyph || !s || !atlas_place(s, (uint8_t)face, em, cp, glyph->w, glyph->h))
    return 0;
  s->x0 = glyph->x0;
  s->y0 = glyph->y0;
  s->advance = glyph->advance / 64.0f;
  uint8_t *staging = data_mem + off_staging;
  for (int y = 0; y < glyph->h; y++)
    __builtin_memcpy(staging + (size_t)(s->y + y) * ATLAS_W + s->x, glyph->coverage + y * glyph->w, glyph->w);
  return s;
}

/* --- a frame ----------------------------------------------------------------------------------------- */

typedef struct {
  float m[12];         /* row-major 3x4 */
  gpu_colour_t c[4];
  float w, h;
  float st[8];         /* the corners' texture coordinates */
  bool textured;
  int8_t image;        /* textured: the atlas (-1) or a picture (gpuui_image) */
  uint8_t material;    /* 0 the plain material, else 1 + a frame's mapping */
  bool depth_tested;   /* hidden where the frame's depth is nearer (gpuui_line_depth) */
  uint8_t layer;       /* depth_tested: its colour's layer */
  float scale, sx, sy; /* gpuui_scale's when it was made */
  float alpha;         /* gpuui_alpha's */
} quad_t;

static quad_t quads[MAX_QUADS];
static int n_quads;
static float quad_scale = 1, quad_sx, quad_sy, quad_alpha = 1;
/* the frame's colour mappings: UiMaterial's colour scale (white - black) and offset (black), its words 0-7,
 * alpha too (the shader clamps the alpha it maps before the corner colours' alpha multiplies it) */
static struct { gpu_colour_t black, white; } mappings[MAX_MATERIALS];
static int n_mappings;

static bool atlas_full;
static nvn_texture_t *frame_target;
static const nvn_texture_t *frame_depth; /* gpuui_depth's */
static bool frame_depth_clear;
/* The lines' layer: an R8 coverage at the depth's size. The depth tested quads go into it a colour at a time,
 * in white (with their alpha), against the depth; it's then laid over the picture in that colour (sampled as
 * 1, 1, 1, coverage). A colour's layer is the same as drawing its lines straight onto the picture: over the
 * background B, a glow g then a line l of colour c give c (l + g - g l) + B (1 - g) (1 - l). */
#define IMAGE_LAYER (-2)
#define LAYER_COLOURS 4 /* the colours a frame's depth tested quads may have (the air walls have 3) */
#define MASK_QUADS 4    /* Link's mask: three, and the far depth round him */
#define MATERIAL_MASK 0xFF
static gpu_occluder_t frame_only; /* gpuui_depth's: Link's box */
static nvn_obj_t layer_pool, layer, layer_view;
static gpu_colour_t layer_colours[LAYER_COLOURS];
static int n_layer_colours;
static uint8_t *layer_mem;
static int layer_w, layer_h;
static nvn_handle_t layer_handle;
static bool layer_failed;
static bool descriptors_new; /* textures registered this frame: the GPU's descriptor cache is stale */
static nvn_rect_t frame_area;
static int frame_texture_height, frame_slot;
static bool frame_lower_left;

bool gpuui_atlas_resetting(void)
{
  return atlas_full;
}

void gpuui_begin(nvn_texture_t *target, nvn_rect_t area, int texture_height, bool origin_lower_left, int slot)
{
  n_quads = 0;
  n_mappings = 0;
  quad_scale = 1;
  quad_alpha = 1;
  frame_target = target;
  frame_depth = 0;
  frame_depth_clear = false;
  n_layer_colours = 0;
  frame_area = area;
  frame_texture_height = texture_height;
  frame_lower_left = origin_lower_left;
  frame_slot = slot % SLOTS;
  gamefont_begin_draw();
  if (atlas_full) {
    atlas_reset();
    atlas_full = false;
  }
}

static quad_t *new_quad(void)
{
  if (ready && n_quads < MAX_QUADS - LAYER_COLOURS - MASK_QUADS) { /* the last elements are the lines' layer's */
    quads[n_quads].material = 0;
    quads[n_quads].depth_tested = false;
    quads[n_quads].scale = quad_scale, quads[n_quads].sx = quad_sx, quads[n_quads].sy = quad_sy;
    quads[n_quads].alpha = quad_alpha;
    return &quads[n_quads++];
  }
  static bool warned;
  if (ready && !warned)
    log_printf("gpu drawing: more than %d quads in a frame; the rest left out", MAX_QUADS);
  warned = true;
  return 0;
}

void gpuui_scale(float s, float ox, float oy)
{
  quad_scale = s, quad_sx = ox, quad_sy = oy;
}

void gpuui_alpha(float a)
{
  quad_alpha = a < 0 ? 0 : a > 1 ? 1 : a;
}

/* the lines' layer, R8 w x h, a render target that's also sampled (as 1, 1, 1, R) */
static bool make_layer(int w, int h)
{
  nvn_obj_t builder;
  __builtin_memset(&builder, 0, sizeof(builder));
  g.tb_device(&builder, device);
  g.tb_defaults(&builder);
  g.tb_target(&builder, TARGET_2D);
  g.tb_format(&builder, FMT_R8);
  g.tb_size(&builder, w, h);
  g.tb_levels(&builder, 1);
  g.tb_flags(&builder, TEXTURE_COMPRESSIBLE);
  size_t size = g.tb_storage_size(&builder), align = g.tb_storage_align(&builder);
  if (align < PAGE)
    align = PAGE;
  size = round_up(size, align);
  layer_mem = aligned_alloc(align, size);
  if (!layer_mem || !make_pool(&layer_pool, layer_mem, size, POOL_CPU_NO_ACCESS | POOL_GPU_CACHED | POOL_COMPRESSIBLE)) {
    log_printf("gpu drawing: no memory for the lines' layer (%u KiB); they show through Link",
               (unsigned)(size / 1024));
    free(layer_mem);
    layer_mem = 0;
    return false;
  }
  g.tb_storage(&builder, &layer_pool, 0);
  if (!g.texture_init(&layer, &builder)) {
    log_printf("gpu drawing: the lines' layer didn't initialise");
    return false;
  }
  int id = our_texture_id + 1 + GPU_IMAGES; /* after the atlas and the pictures */
  gd.view_defaults(&layer_view);
  gd.view_swizzle(&layer_view, SWIZZLE_ONE, SWIZZLE_ONE, SWIZZLE_ONE, SWIZZLE_R);
  g.texture_pool_register(&texture_pool, id, &layer, &layer_view);
  layer_handle = g.texture_handle(device, id, our_sampler_id);
  descriptors_new = true;
  layer_w = w, layer_h = h;
  log_printf("gpu drawing: the lines' layer %dx%d (%u KiB)", w, h, (unsigned)(size / 1024));
  return true;
}

bool gpuui_depth(const nvn_texture_t *depth_target, int w, int h, bool clear, const gpu_occluder_t *only)
{
  frame_depth = 0;
  frame_depth_clear = false;
  if (!depth_target || !only || !depth_usable || w <= 0 || h <= 0)
    return true;
  frame_only = *only;
  if (layer_failed)
    return false;
  if (!layer_mem && !make_layer(w, h)) {
    layer_failed = true;
    return false;
  }
  if (w != layer_w || h != layer_h)
    return true; /* not the size it was made for: shown through, this frame */
  frame_depth = depth_target;
  frame_depth_clear = clear;
  return true;
}

static void set_translate(quad_t *q, float x, float y)
{
  static const float identity[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
  __builtin_memcpy(q->m, identity, sizeof(identity));
  q->m[3] = x;
  q->m[7] = y;
}

void gpuui_rect(float x, float y, float w, float h, const gpu_colour_t corners[4])
{
  if (w <= 0 || h <= 0)
    return;
  quad_t *q = new_quad();
  if (!q)
    return;
  set_translate(q, x, y);
  __builtin_memcpy(q->c, corners, sizeof(q->c));
  q->w = w;
  q->h = h;
  q->textured = false;
}

void gpuui_fill(float x, float y, float w, float h, gpu_colour_t colour)
{
  gpu_colour_t corners[4] = { colour, colour, colour, colour };
  gpuui_rect(x, y, w, h, corners);
}

/* the line's profile across it: nothing at its sides, full over its middle half */
#define LINE_PROFILE_ID 0xFFF0u
#define LINE_PROFILE_H 32
static void paint_line_profile(uint8_t *cov, int w, int h, int stride, const void *arg)
{
  (void)arg;
  for (int y = 0; y < h; y++) {
    float v = (y + 0.5f) / h, edge = v < 0.5f ? v : 1 - v; /* 0 at a side, 0.5 in the middle */
    float c = edge >= 0.25f ? 1 : edge / 0.25f;
    for (int x = 0; x < w; x++)
      cov[y * stride + x] = (uint8_t)(c * c * (3 - 2 * c) * 255 + 0.5f);
  }
}

void gpuui_line(float x0, float y0, float x1, float y1, float width, gpu_colour_t colour)
{
  float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
  if (!ready || len < 0.01f || width <= 0)
    return;
  atlas_slot_t *s = atlas_find(SHAPE_FACE, 4, LINE_PROFILE_ID << 16 | LINE_PROFILE_H);
  if (s && !s->state) {
    if (!atlas_place(s, SHAPE_FACE, 4, LINE_PROFILE_ID << 16 | LINE_PROFILE_H, 4, LINE_PROFILE_H)) {
      atlas_full = true;
      return;
    }
    paint_line_profile(data_mem + off_staging + (size_t)s->y * ATLAS_W + s->x, 4, LINE_PROFILE_H, ATLAS_W, 0);
  }
  if (!s || s->state != 1)
    return;
  quad_t *q = new_quad();
  if (!q)
    return;
  /* a len x h quad along the line, turned about the line's start: (0, h / 2) on (x0, y0) */
  float h = width * 2, c = dx / len, sn = dy / len;
  float m[12] = { c, -sn, 0, x0 + sn * h / 2, sn, c, 0, y0 - c * h / 2, 0, 0, 0, 0 };
  __builtin_memcpy(q->m, m, sizeof(m));
  for (int i = 0; i < 4; i++)
    q->c[i] = colour;
  q->w = len;
  q->h = h;
  float sm = (s->x + 2.0f) / ATLAS_W, t0 = (float)s->y / ATLAS_H, t1 = (float)(s->y + s->h) / ATLAS_H;
  float st[8] = { sm, t0, sm, t0, sm, t1, sm, t1 };
  __builtin_memcpy(q->st, st, sizeof(st));
  q->textured = true;
  q->image = -1;
}

/* a depth (0..1, as the game's depth buffer has it) as the clip z that gives it: NVN's GL depth mode (the
 * game's) maps clip z -1..1 to depth 0..1, its other one takes z as the depth */
static float clip_z(float d)
{
  bool gl = !gd.depth_mode || gd.depth_mode(device) == DEPTH_MODE_NEAR_IS_MINUS_W;
  return gl ? 2 * d - 1 : d;
}

/* quad q (translated, not turned) at a plane's depth: p[0] x + p[1] y + p[2], x and y 0..1 of the frame's area.
 * Linear on the picture, so the matrix's third row gives it (clip z from the quad's own x and y); where it's
 * past the far plane (the sky side of the ground's horizon) the quad is clipped */
static void plane_z(quad_t *q, const float p[3])
{
  float sw = (float)frame_area.w, sh = (float)frame_area.h, k = clip_z(1) - clip_z(0);
  q->m[8] = k * p[0] / sw;
  q->m[9] = k * p[1] / sh;
  q->m[11] = k * (p[0] * q->m[3] / sw + p[1] * q->m[7] / sh + p[2]) + clip_z(0);
}

void gpuui_line_depth(float x0, float y0, float d0, float x1, float y1, float d1, float width, gpu_colour_t colour)
{
  int before = n_quads;
  gpuui_line(x0, y0, x1, y1, width, colour);
  if (n_quads == before || !frame_depth)
    return;
  /* the depth along it: the matrix's third row gives clip z from the quad's own x (0 to its length), and the
   * view passes it on with w 1. The depth along a line on the picture is linear in the picture, as the
   * scene's is (1 / distance is). */
  quad_t *q = &quads[n_quads - 1];
  int k = 0; /* its colour's layer */
  while (k < n_layer_colours && (layer_colours[k].r != colour.r || layer_colours[k].g != colour.g ||
                                 layer_colours[k].b != colour.b))
    k++;
  if (k == LAYER_COLOURS)
    return; /* one colour too many: drawn as gpuui_line */
  if (k == n_layer_colours)
    layer_colours[n_layer_colours++] = colour;
  q->layer = (uint8_t)k;
  for (int i = 0; i < 4; i++) /* white: its colour comes with the layer */
    q->c[i] = (gpu_colour_t){ 1, 1, 1, colour.a };
  float z0 = clip_z(d0), z1 = clip_z(d1);
  q->m[8] = (z1 - z0) / q->w;
  q->m[11] = z0;
  q->depth_tested = true;
}

void gpuui_rect_turned(float cx, float cy, float w, float h, float angle, gpu_colour_t colour)
{
  quad_t *q = new_quad();
  if (!q)
    return;
  float c = cosf(angle), s = sinf(angle);
  /* about the centre: the corner (0,0) of the w x h rectangle goes to (-w/2, -h/2) turned */
  float m[12] = { c, -s, 0, cx - c * w / 2 + s * h / 2, s, c, 0, cy - s * w / 2 - c * h / 2, 0, 0, 0, 0 };
  __builtin_memcpy(q->m, m, sizeof(m));
  for (int i = 0; i < 4; i++)
    q->c[i] = colour;
  q->w = w;
  q->h = h;
  q->textured = false;
}

/* the atlas image of slot s as a w x h quad at (x, y) */
static void textured_quad(const atlas_slot_t *s, float x, float y, gpu_colour_t colour)
{
  quad_t *q = new_quad();
  if (!q)
    return;
  set_translate(q, x, y);
  for (int i = 0; i < 4; i++)
    q->c[i] = colour;
  q->w = s->w;
  q->h = s->h;
  float s0 = (float)s->x / ATLAS_W, t0 = (float)s->y / ATLAS_H;
  float s1 = (float)(s->x + s->w) / ATLAS_W, t1 = (float)(s->y + s->h) / ATLAS_H;
  float st[8] = { s0, t0, s1, t0, s0, t1, s1, t1 };
  __builtin_memcpy(q->st, st, sizeof(st));
  q->textured = true;
  q->image = -1;
}

float gpuui_glyph(gf_face_t face, int em, uint32_t cp, float x, float baseline, gpu_colour_t colour)
{
  if (!ready)
    return 0;
  const atlas_slot_t *s = atlas_glyph(face, em, cp);
  if (!s) {
    const gf_glyph_t *glyph = gamefont_glyph(face, em, cp);
    if (glyph)
      atlas_full = true; /* there was a glyph, but no room: next frame starts the atlas afresh */
    return glyph ? glyph->advance / 64.0f : 0;
  }
  if (s->state == 1) /* pixel-aligned, as the glyph was rasterised */
    textured_quad(s, (float)(int)(x + 0.5f) + s->x0, baseline + s->y0, colour);
  return s->advance;
}

void gpuui_shape(uint32_t id, int w, int h, gpu_paint_t paint, const void *arg, float x, float y, gpu_colour_t colour)
{
  if (!ready || w <= 0 || h <= 0 || w > 0xFFFF || h > 0xFFFF)
    return;
  uint32_t key = id << 16 | (uint32_t)h;
  atlas_slot_t *s = atlas_find(SHAPE_FACE, w, key);
  if (s && !s->state) {
    if (!atlas_place(s, SHAPE_FACE, w, key, w, h)) {
      atlas_full = true;
      return;
    }
    paint(data_mem + off_staging + (size_t)s->y * ATLAS_W + s->x, w, h, ATLAS_W, arg);
  }
  if (s && s->state == 1)
    textured_quad(s, (float)(int)(x + 0.5f), (float)(int)(y + 0.5f), colour);
}

/* --- the game's pictures ---------------------------------------------------------------------------- */


/* The pictures come in batches (gameimage.c: the shared archive's, then each item's icon), each its own
 * memory and pool, its pictures numbered from `first`. */
#define GPU_BATCHES GPU_IMAGES
static struct {
  uint8_t *mem;
  size_t size;
  int first, n;
  bool published; /* game thread to render thread */
  enum { IMAGES_NONE, IMAGES_MADE, IMAGES_FAILED } state;
  nvn_obj_t pool;
} batches[GPU_BATCHES];
static gpu_image_t image_info[GPU_IMAGES];
static nvn_obj_t image_builder, image_tex[GPU_IMAGES];
static nvn_handle_t image_handle[GPU_IMAGES];
static bool image_ok[GPU_IMAGES];

void gpuui_images_publish(int batch, int first, uint8_t *mem, size_t size, const gpu_image_t *images, int n)
{
  if (batch < 0 || batch >= GPU_BATCHES || __atomic_load_n(&batches[batch].published, __ATOMIC_ACQUIRE) || n <= 0 ||
      first < 0 || first >= GPU_IMAGES)
    return;
  n = first + n <= GPU_IMAGES ? n : GPU_IMAGES - first;
  batches[batch].mem = mem;
  batches[batch].size = size;
  batches[batch].first = first;
  batches[batch].n = n;
  __builtin_memcpy(&image_info[first], images, sizeof(*images) * (size_t)n);
  __atomic_store_n(&batches[batch].published, true, __ATOMIC_RELEASE);
}

/* render thread: a published batch's pictures as textures, in a pool on their memory */
static void make_images(int batch)
{
  __typeof__(batches[0]) *b = &batches[batch];
  b->state = IMAGES_FAILED;
  if (!gi.tb_packaged_layout || !gi.pool_flush) {
    log_printf("gpu drawing: NVN can't take the game's packaged textures; no pictures");
    return;
  }
  if (!make_pool(&b->pool, b->mem, b->size, POOL_CPU_CACHED | POOL_GPU_CACHED)) {
    log_printf("gpu drawing: no memory pool for the pictures");
    return;
  }
  gi.pool_flush(&b->pool, 0, b->size); /* written through the CPU's cache before the pool was made */
  int made = 0;
  for (int i = b->first; i < b->first + b->n; i++) {
    const gpu_image_t *im = &image_info[i];
    if (!im->size)
      continue;
    g.tb_device(&image_builder, device);
    g.tb_defaults(&image_builder);
    g.tb_target(&image_builder, TARGET_2D);
    g.tb_format(&image_builder, im->format);
    g.tb_size(&image_builder, im->width, im->height);
    g.tb_levels(&image_builder, 1);
    g.tb_flags(&image_builder, 0);
    g.tb_swizzle(&image_builder, im->swizzle[0], im->swizzle[1], im->swizzle[2], im->swizzle[3]);
    gi.tb_packaged_layout(&image_builder, im->layout);
    g.tb_storage(&image_builder, &b->pool, (ptrdiff_t)im->offset);
    if (g.tb_storage_size(&image_builder) > im->size || !g.texture_init(&image_tex[i], &image_builder)) {
      log_printf("gpu drawing: picture %d (%dx%d) didn't make a texture", i, im->width, im->height);
      continue;
    }
    g.texture_pool_register(&texture_pool, our_texture_id + 1 + i, &image_tex[i], 0); /* after the atlas */
    image_handle[i] = g.texture_handle(device, our_texture_id + 1 + i, our_sampler_id);
    descriptors_new = true;
    image_ok[i] = true;
    made++;
  }
  b->state = made ? IMAGES_MADE : IMAGES_FAILED;
  log_printf("gpu drawing: %d of the game's pictures ready (batch %d)", made, batch);
}

/* --- the items' icons: an RGBA atlas filled on the CPU, brought up as its slots fill ------------------ */

#define FMT_RGBA8 0x25
#define ICONS_W (GPU_ICON_PX * GPU_ICON_COLS)
#define ICONS_H (GPU_ICON_PX * GPU_ICON_ROWS)
#define ICONS_STAGING ((size_t)ICONS_W * ICONS_H * 4)
static struct {
  uint8_t *mem;
  size_t size;
  bool published;    /* game thread to render thread */
  uint64_t filled;   /* game thread to render thread: the slots filled */
  uint64_t up;       /* render thread: the slots brought up */
  enum { ICONS_NONE, ICONS_MADE, ICONS_FAILED } state;
  nvn_obj_t pool, tex;
  nvn_addr_t gpu;
} icons;

void gpuui_icons_publish(uint8_t *mem, size_t size)
{
  if (__atomic_load_n(&icons.published, __ATOMIC_ACQUIRE))
    return;
  icons.mem = mem;
  icons.size = size;
  __atomic_store_n(&icons.published, true, __ATOMIC_RELEASE);
}

void gpuui_icon_filled(int slot)
{
  if (slot >= 0 && slot < GPU_ICON_COLS * GPU_ICON_ROWS)
    __atomic_fetch_or(&icons.filled, 1ull << slot, __ATOMIC_RELEASE);
}

/* render thread: the atlas's texture, after its staging rows in the same pool */
static void make_icons(void)
{
  icons.state = ICONS_FAILED;
  if (!gi.pool_flush) {
    log_printf("gpu drawing: NVN can't flush a pool; no icons");
    return;
  }
  nvn_obj_t builder;
  g.tb_device(&builder, device);
  g.tb_defaults(&builder);
  g.tb_target(&builder, TARGET_2D);
  g.tb_format(&builder, FMT_RGBA8);
  g.tb_size(&builder, ICONS_W, ICONS_H);
  g.tb_levels(&builder, 1);
  g.tb_flags(&builder, 0); /* block-linear: the copy lays the texels out */
  g.tb_swizzle(&builder, SWIZZLE_R, SWIZZLE_R + 1, SWIZZLE_R + 2, SWIZZLE_R + 3);
  size_t align = g.tb_storage_align(&builder), at = ICONS_STAGING;
  align = align < PAGE ? PAGE : align;
  at = (at + align - 1) / align * align;
  if (at + g.tb_storage_size(&builder) > icons.size) {
    log_printf("gpu drawing: the icons' memory is too small for their texture");
    return;
  }
  if (!make_pool(&icons.pool, icons.mem, icons.size, POOL_CPU_CACHED | POOL_GPU_CACHED)) {
    log_printf("gpu drawing: no memory pool for the icons");
    return;
  }
  g.tb_storage(&builder, &icons.pool, (ptrdiff_t)at);
  if (!g.texture_init(&icons.tex, &builder)) {
    log_printf("gpu drawing: the icons' texture wasn't made");
    return;
  }
  icons.gpu = g.pool_address(&icons.pool);
  g.texture_pool_register(&texture_pool, our_texture_id + 1 + GPU_ICONS_IMAGE, &icons.tex, 0);
  image_handle[GPU_ICONS_IMAGE] = g.texture_handle(device, our_texture_id + 1 + GPU_ICONS_IMAGE, our_sampler_id);
  descriptors_new = true;
  image_ok[GPU_ICONS_IMAGE] = true;
  icons.state = ICONS_MADE;
}

bool gpuui_icon_ready(int slot)
{
  if (!ready || slot < 0 || slot >= GPU_ICON_COLS * GPU_ICON_ROWS || !__atomic_load_n(&icons.published, __ATOMIC_ACQUIRE))
    return false;
  if (icons.state == ICONS_NONE)
    make_icons();
  /* drawn once it's up (gpuui_end brings up what's filled before the frame's quads are drawn) */
  return icons.state == ICONS_MADE && (__atomic_load_n(&icons.filled, __ATOMIC_ACQUIRE) >> slot & 1);
}

void gpuui_icon(int slot, float x, float y, float w, float h, const float part[4], gpu_colour_t colour)
{
  if (!gpuui_icon_ready(slot) || w <= 0 || h <= 0)
    return;
  static const float all[4] = { 0, 0, 1, 1 };
  part = part ? part : all;
  float s0 = (float)(slot % GPU_ICON_COLS * GPU_ICON_PX) / ICONS_W, t0 = (float)(slot / GPU_ICON_COLS * GPU_ICON_PX) / ICONS_H;
  float sw = (float)GPU_ICON_PX / ICONS_W, th = (float)GPU_ICON_PX / ICONS_H;
  float crop[4] = { s0 + part[0] * sw, t0 + part[1] * th, s0 + part[2] * sw, t0 + part[3] * th };
  quad_t *q = new_quad();
  if (!q)
    return;
  set_translate(q, x, y);
  for (int k = 0; k < 4; k++)
    q->c[k] = colour;
  q->w = w;
  q->h = h;
  float st[8] = { crop[0], crop[1], crop[2], crop[1], crop[0], crop[3], crop[2], crop[3] };
  __builtin_memcpy(q->st, st, sizeof(st));
  q->textured = true;
  q->image = (int8_t)GPU_ICONS_IMAGE;
}

/* render thread, before the frame's drawing: the slots filled since, up from the staging rows */
static void icons_up(nvn_obj_t *cmd)
{
  if (icons.state != ICONS_MADE)
    return;
  uint64_t filled = __atomic_load_n(&icons.filled, __ATOMIC_ACQUIRE);
  if (filled == icons.up)
    return;
  gi.pool_flush(&icons.pool, 0, ICONS_STAGING); /* written through the CPU's cache */
  g.cmd_copy_stride(cmd, 0);
  nvn_region_t all = { 0, 0, 0, ICONS_W, ICONS_H, 1 };
  g.cmd_copy_to_texture(cmd, icons.gpu, &icons.tex, 0, &all, 0);
  g.cmd_barrier(cmd, BARRIER_ORDER_PRIMITIVES | BARRIER_ORDER_FRAGMENTS | BARRIER_INVALIDATE_TEXTURE);
  icons.up = filled;
}

bool gpuui_image_ready(int i)
{
  if (!ready || i < 0 || i >= GPU_IMAGES)
    return false;
  for (int k = 0; k < GPU_BATCHES; k++) {
    if (!__atomic_load_n(&batches[k].published, __ATOMIC_ACQUIRE) || i < batches[k].first ||
        i >= batches[k].first + batches[k].n)
      continue;
    if (batches[k].state == IMAGES_NONE)
      make_images(k);
    return batches[k].state == IMAGES_MADE && image_ok[i];
  }
  return false;
}

void gpuui_image(int i, float x, float y, float w, float h, const float crop[4], gpu_colour_t colour)
{
  if (!gpuui_image_ready(i) || w <= 0 || h <= 0)
    return;
  quad_t *q = new_quad();
  if (!q)
    return;
  set_translate(q, x, y);
  for (int k = 0; k < 4; k++)
    q->c[k] = colour;
  q->w = w;
  q->h = h;
  float st[8] = { crop[0], crop[1], crop[2], crop[1], crop[0], crop[3], crop[2], crop[3] };
  __builtin_memcpy(q->st, st, sizeof(st));
  q->textured = true;
  q->image = (int8_t)i;
}

void gpuui_image_mapped(int i, float x, float y, float w, float h, const float crop[4], gpu_colour_t black,
                        gpu_colour_t white, float alpha)
{
  int m = 0;
  for (; m < n_mappings; m++) {
    gpu_colour_t b = mappings[m].black, w = mappings[m].white;
    if (b.r == black.r && b.g == black.g && b.b == black.b && b.a == black.a && w.r == white.r && w.g == white.g &&
        w.b == white.b && w.a == white.a)
      break;
  }
  if (m == n_mappings) {
    if (n_mappings == MAX_MATERIALS)
      return;
    mappings[n_mappings].black = black;
    mappings[n_mappings].white = white;
    n_mappings++;
  }
  int before = n_quads;
  gpuui_image(i, x, y, w, h, crop, (gpu_colour_t){ 1, 1, 1, alpha });
  if (n_quads > before)
    quads[n_quads - 1].material = (uint8_t)(m + 1);
}

static void write_element(uint8_t *at, const quad_t *q)
{
  float e[ELEMENT_WORDS] = { 0 };
  __builtin_memcpy(e, q->m, sizeof(q->m));
  if (q->scale != 1) { /* the matrix, then the scale about its point */
    for (int i = 0; i < 3; i++)
      e[i] *= q->scale, e[4 + i] *= q->scale;
    e[3] = q->sx + (e[3] - q->sx) * q->scale;
    e[7] = q->sy + (e[7] - q->sy) * q->scale;
  }
  for (int i = 0; i < 4; i++) {
    e[12 + 4 * i] = q->c[i].r;
    e[13 + 4 * i] = q->c[i].g;
    e[14 + 4 * i] = q->c[i].b;
    e[15 + 4 * i] = q->c[i].a * q->alpha;
  }
  e[28] = q->w;
  e[29] = q->h;
  if (q->textured)
    __builtin_memcpy(&e[32], q->st, sizeof(q->st));
  e[40] = 1; /* the texture matrix: identity */
  e[43] = 1;
  __builtin_memcpy(at, e, sizeof(e));
}

/* the state a frame's passes start from */
static void bind_frame(nvn_obj_t *cmd, size_t view_at)
{
  g.cmd_blend(cmd, &blend);
  g.cmd_colour(cmd, &colour);
  g.cmd_channels(cmd, &channels);
  g.cmd_multisample(cmd, &multisample);
  g.cmd_polygon(cmd, &polygon);
  g.cmd_depth(cmd, &depth);
  g.cmd_attribs(cmd, 2, attribs);
  g.cmd_streams(cmd, 1, streams);
  g.cmd_vertex_buffer(cmd, 0, data_gpu + off_vertices, 32);
  g.cmd_uniform(cmd, STAGE_VERTEX, 1, data_gpu + view_at, UNIFORM_SIZE);
  g.cmd_uniform(cmd, STAGE_VERTEX, 2, data_gpu + off_shadow, 64);
  g.cmd_uniform(cmd, STAGE_FRAGMENT, 4, data_gpu + off_material, 64);
  g.cmd_texture_pool(cmd, &texture_pool);
  g.cmd_sampler_pool(cmd, &sampler_pool);
  g.cmd_texture(cmd, STAGE_FRAGMENT, 8, atlas_handle);
}

/* what a pass has bound: its program, its picture (-1 the atlas), its material */
typedef struct { int program, image, material; } draw_state_t;

/* quad q, its element at `at`; false: out of command memory */
static bool draw_quad(nvn_obj_t *cmd, const quad_t *q, size_t at, size_t materials, draw_state_t *st)
{
  if (g.cmd_free(cmd) < CMD_RESERVE)
    return false;
  int program = q->textured ? UI_PROGRAM_TEXTURE : UI_PROGRAM_SOLID;
  if (program != st->program) {
    g.cmd_program(cmd, &programs[program], STAGES_GRAPHICS);
    st->program = program;
  }
  if (q->material != st->material) { /* the plain material (bound by bind_frame), a mapping or Link's mask's */
    st->material = q->material;
    nvn_addr_t m = st->material == MATERIAL_MASK ? data_gpu + off_mask
                   : st->material ? data_gpu + materials + (size_t)(st->material - 1) * UNIFORM_SIZE
                                  : data_gpu + off_material;
    g.cmd_uniform(cmd, STAGE_FRAGMENT, 4, m, 64);
  }
  if (q->textured && q->image != st->image) { /* the atlas (bound by bind_frame), a picture or the lines' layer */
    st->image = q->image;
    g.cmd_texture(cmd, STAGE_FRAGMENT, 8,
                  st->image == IMAGE_LAYER ? layer_handle : st->image < 0 ? atlas_handle : image_handle[st->image]);
  }
  write_element(data_mem + at, q);
  g.cmd_storage(cmd, STAGE_VERTEX, 6, data_gpu + at, ELEMENT_SIZE);
  g.cmd_draw(cmd, PRIM_TRIANGLE_STRIP, 0, 4);
  return true;
}

bool gpuui_end(nvn_obj_t *cmd, const void *game_texture_pool, const void *game_sampler_pool)
{
  if (!ready || !n_quads)
    return false;
  icons_up(cmd);
  if (descriptors_new) { /* the pictures' descriptors, read fresh */
    g.cmd_barrier(cmd, BARRIER_INVALIDATE_TEXTURE_DESCRIPTOR);
    descriptors_new = false;
  }
  /* the rows the frame's new glyphs touched, up before they're drawn */
  if (dirty_bottom > dirty_top) {
    g.cmd_copy_stride(cmd, 0);
    nvn_region_t rows = { 0, dirty_top, 0, ATLAS_W, dirty_bottom - dirty_top, 1 };
    g.cmd_copy_to_texture(cmd, data_gpu + off_staging + (nvn_addr_t)dirty_top * ATLAS_W, &atlas, 0, &rows, 0);
    g.cmd_barrier(cmd, BARRIER_ORDER_PRIMITIVES | BARRIER_ORDER_FRAGMENTS | BARRIER_INVALIDATE_TEXTURE);
    dirty_top = ATLAS_H;
    dirty_bottom = 0;
  }
  /* the view: the visible area's pixels, y down, to clip space (column-major); the lines' layer is the same
   * area at its own size */
  float sw = (float)frame_area.w, sh = (float)frame_area.h;
  float view[16] = { 2 / sw, 0, 0, 0, 0, -2 / sh, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1 };
  size_t view_at = off_view + (size_t)frame_slot * UNIFORM_SIZE;
  __builtin_memcpy(data_mem + view_at, view, sizeof(view));

  /* the frame's colour mappings, in its slot's place: black and white of the texture to two colours */
  size_t materials = off_materials + (size_t)frame_slot * MAX_MATERIALS * UNIFORM_SIZE;
  for (int m = 0; m < n_mappings; m++) {
    gpu_colour_t b = mappings[m].black, w = mappings[m].white;
    float words[16] = { w.r - b.r, w.g - b.g, w.b - b.b, w.a - b.a, b.r, b.g, b.b, b.a, 1, 1 };
    uint32_t discard_clear = 0x2;
    __builtin_memcpy(&words[10], &discard_clear, 4);
    __builtin_memcpy(data_mem + materials + (size_t)m * UNIFORM_SIZE, words, sizeof(words));
  }
  size_t elements = off_elements + (size_t)frame_slot * MAX_QUADS * ELEMENT_SIZE;

  bool layered = frame_depth && (n_layer_colours || frame_depth_clear);
  int vy = frame_lower_left ? frame_texture_height - frame_area.y - frame_area.h : frame_area.y;
  g.cmd_render_enable(cmd, true);
  /* the lines' layer a colour at a time: cleared, its depth tested quads drawn into it against that depth, then
   * over the picture */
  /* Only Link hides the lines: the scene's depth is made his alone first. Into the layer, his mask: what of the
   * depth is in his box (in the rectangle, between the depths in front of and behind him); then the depth goes
   * to the far plane wherever the mask isn't (a quad over it all at the far plane, discarded on the mask) */
  nvn_texture_t *layer_target = (nvn_texture_t *)&layer;
  static const float clear[4] = { 0, 0, 0, 0 };
  size_t extra = elements + (size_t)(MAX_QUADS - LAYER_COLOURS - MASK_QUADS) * ELEMENT_SIZE;
  if (layered) {
    g.cmd_barrier(cmd, BARRIER_ORDER_PRIMITIVES | BARRIER_ORDER_FRAGMENTS | BARRIER_INVALIDATE_ZCULL);
    g.cmd_targets(cmd, 1, &layer_target, 0, frame_depth, 0);
    g.cmd_viewport(cmd, 0, 0, layer_w, layer_h);
    g.cmd_scissor(cmd, 0, 0, layer_w, layer_h);
    gd.cmd_depth_range(cmd, 0, 1);
    gd.cmd_clear_colour(cmd, 0, clear, CLEAR_ALL_CHANNELS);
    if (frame_depth_clear) /* new: the far plane, before the game's depth is first copied in */
      gd.cmd_clear_depth(cmd, 1, true, 0, 0);
    bind_frame(cmd, view_at);
    draw_state_t st = { -1, -1, 0 };
    float rx = frame_only.x0 * sw, ry = frame_only.y0 * sh, rw = (frame_only.x1 - frame_only.x0) * sw,
          rh = (frame_only.y1 - frame_only.y0) * sh;
    quad_t in_front = { .c = { { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 } }, .w = rw, .h = rh,
                        .scale = 1, .alpha = 1 };
    set_translate(&in_front, rx, ry);
    quad_t behind = in_front, ground;
    for (int i = 0; i < 4; i++)
      behind.c[i] = (gpu_colour_t){ 0, 0, 0, 1 };
    ground = behind;
    plane_z(&in_front, frame_only.front); /* white where the depth is at his front or beyond */
    plane_z(&behind, frame_only.back);    /* black again where it's past his back */
    plane_z(&ground, frame_only.ground);  /* and where it's at the ground or under it */
    g.cmd_depth(cmd, &depth_tested);
    draw_quad(cmd, &in_front, extra, materials, &st);
    g.cmd_depth(cmd, &depth_beyond);
    draw_quad(cmd, &behind, extra + ELEMENT_SIZE, materials, &st);
    g.cmd_depth(cmd, &depth_tested);
    draw_quad(cmd, &ground, extra + 3 * ELEMENT_SIZE, materials, &st);
    g.cmd_barrier(cmd, BARRIER_ORDER_PRIMITIVES | BARRIER_ORDER_FRAGMENTS | BARRIER_INVALIDATE_TEXTURE);
    g.cmd_targets(cmd, 0, 0, 0, frame_depth, 0); /* the depth alone: the layer is read */
    g.cmd_viewport(cmd, 0, 0, layer_w, layer_h);
    g.cmd_scissor(cmd, 0, 0, layer_w, layer_h);
    quad_t far = { .c = { { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 } }, .w = sw, .h = sh,
                   .textured = true, .image = IMAGE_LAYER, .material = MATERIAL_MASK, .scale = 1, .alpha = 1 };
    set_translate(&far, 0, 0);
    float t0 = frame_lower_left ? 1 : 0, t1 = 1 - t0;
    float layer_st[8] = { 0, t0, 1, t0, 0, t1, 1, t1 };
    __builtin_memcpy(far.st, layer_st, sizeof(layer_st));
    far.m[11] = clip_z(1);
    g.cmd_depth(cmd, &depth_far);
    draw_quad(cmd, &far, extra + 2 * ELEMENT_SIZE, materials, &st);
    g.cmd_depth(cmd, &depth);
    g.cmd_barrier(cmd, BARRIER_ORDER_PRIMITIVES | BARRIER_ORDER_FRAGMENTS | BARRIER_INVALIDATE_ZCULL);
  }
  for (int k = 0; layered && k < n_layer_colours; k++) {
    if (k) /* the layer's last colour has been laid over the picture */
      g.cmd_barrier(cmd, BARRIER_ORDER_PRIMITIVES | BARRIER_ORDER_FRAGMENTS);
    g.cmd_targets(cmd, 1, &layer_target, 0, frame_depth, 0);
    g.cmd_viewport(cmd, 0, 0, layer_w, layer_h);
    g.cmd_scissor(cmd, 0, 0, layer_w, layer_h);
    gd.cmd_clear_colour(cmd, 0, clear, CLEAR_ALL_CHANNELS);
    bind_frame(cmd, view_at);
    g.cmd_depth(cmd, &depth_tested);
    draw_state_t st = { -1, -1, 0 };
    for (int i = 0; i < n_quads; i++)
      if (quads[i].depth_tested && quads[i].layer == k &&
          !draw_quad(cmd, &quads[i], elements + (size_t)i * ELEMENT_SIZE, materials, &st))
        break;
    g.cmd_depth(cmd, &depth);
    g.cmd_barrier(cmd, BARRIER_ORDER_PRIMITIVES | BARRIER_ORDER_FRAGMENTS | BARRIER_INVALIDATE_TEXTURE);
    /* over the picture, in its colour, the whole area */
    g.cmd_targets(cmd, 1, &frame_target, 0, 0, 0);
    g.cmd_viewport(cmd, frame_area.x, vy, frame_area.w, frame_area.h);
    g.cmd_scissor(cmd, frame_area.x, vy, frame_area.w, frame_area.h);
    gpu_colour_t c = layer_colours[k];
    c.a = 1;
    quad_t q = { .c = { c, c, c, c }, .w = sw, .h = sh, .textured = true, .image = IMAGE_LAYER, .scale = 1, .alpha = 1 };
    set_translate(&q, 0, 0);
    float t0 = frame_lower_left ? 1 : 0, t1 = 1 - t0;
    float layer_st[8] = { 0, t0, 1, t0, 0, t1, 1, t1 };
    __builtin_memcpy(q.st, layer_st, sizeof(layer_st));
    st = (draw_state_t){ -1, -1, 0 };
    draw_quad(cmd, &q, elements + (size_t)(MAX_QUADS - LAYER_COLOURS + k) * ELEMENT_SIZE, materials, &st);
  }

  g.cmd_targets(cmd, 1, &frame_target, 0, 0, 0);
  g.cmd_viewport(cmd, frame_area.x, vy, frame_area.w, frame_area.h);
  g.cmd_scissor(cmd, frame_area.x, vy, frame_area.w, frame_area.h);
  bind_frame(cmd, view_at);
  draw_state_t st = { -1, -1, 0 };
  for (int i = 0; i < n_quads; i++) /* the rest (the depth tested ones went in the layer) */
    if (!(quads[i].depth_tested && layered) &&
        !draw_quad(cmd, &quads[i], elements + (size_t)i * ELEMENT_SIZE, materials, &st))
      break;
  /* the game's descriptor pools back, for its next frame's commands */
  if (game_texture_pool)
    g.cmd_texture_pool(cmd, game_texture_pool);
  if (game_sampler_pool)
    g.cmd_sampler_pool(cmd, game_sampler_pool);
  return layered;
}
