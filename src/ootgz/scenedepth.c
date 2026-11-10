/* The game's scene depth (scenedepth.h).
 *
 * Each frame of gameplay the game copies its scene depth (a Depth32F texture at the render resolution:
 * 1536x864, or smaller under dynamic resolution) into a second one right after its depth pre-pass, with
 * nvnCommandBufferCopyTextureToTexture; a half-size depth is copied the same way after it. Our version of
 * that call adds to the game's command buffer, right after its copy of the full-size one (the barrier the
 * game put before its copy covers ours):
 *   - for the air walls: a copy of the depth into our own Depth32F texture (scaled to it, as the size of the
 *     game's changes under dynamic resolution). At present time gpuui makes it Link's alone (his box,
 *     airwall.c), draws the lines into a layer of that size against it (LEQUAL, no writes) and lays the layer
 *     over the picture. Not at the picture's size: a depth that big (15 MB at 2560x1440, 33 MB at 4K) is more
 *     than the game's heap gives.
 *   - for the health bars: 1x1 copies of the texels at a few points on each enemy (enemyhp.c) into a small
 *     buffer the CPU reads two or more presents later, behind a fence. A point is hidden where the scene
 *     is nearer than the depth enemyhp.c gave it (its own, moved toward the camera by a margin); a bar
 *     fades out while all of its enemy's points are hidden, and back in when one shows.
 * Not at present time: on the console the game puts small HUD targets over the depth's memory (the same
 * pool offset) once it's done with it, before the present. The depth's texture object is new every frame or
 * two, so it's never kept: each copy uses the one the game's call has. Without these copies (menus, the
 * title) the lines and the bars show as before. The depth is GL style (near -w, cleared to 1, LESS); see
 * FINDINGS.md 9.6, and docs/NATIVE_UI.md "Hidden behind Link and the scenery". */
#include "scenedepth.h"

#include <stddef.h>

#include "gfx.h"
#include "publish.h"
#include "sys.h"

void *aligned_alloc(size_t align, size_t size); /* from main */
void free(void *p);                             /* from main */

#define PAGE 0x1000
#define MIN_SCENE_W 256       /* a depth narrower than this isn't the scene's */
#define SCENE_GONE 4          /* presents without the scene's depth: the next depth copied is taken for it */
#define LINES_KEEP 30         /* presents the lines' copy goes on after they last asked for it */
#define LINES_PIXELS (1536 * 864) /* the lines' depth and layer: at most this many pixels (the game's scene) */
#define LINES_FRESH 2         /* presents a line copy is good for (the game may record a frame ahead) */
#define SLOTS 8               /* texel copies in flight (two or three presents each) */
#define MAX_ASKED 16          /* enemies looked at: the nearest */
#define MAX_POINTS (MAX_ASKED * EH_PROBES)
#define POINT_BYTES 16        /* a texel's place in the results */
#define RESULT_SIZE (SLOTS * MAX_POINTS * POINT_BYTES)
#define UNWRITTEN 0x7FC0DEADu /* a NaN: no depth ever reads this */
#define READ_AFTER 2          /* presents after its fence before a slot is read */
#define MAX_TRIES 30          /* fences a slot waits for its copies before it's given up */
#define NO_DEPTH_MS 500       /* without any results this long, the bars show as before */
#define PENDING_MS 300        /* a new bar with no result yet shows after this */
#define FADE_MS 140           /* a bar's fade in or out */
#define FORGET_MS 1000        /* a bar not drawn this long is forgotten */
#define MAX_KNOWN 32

/* NVN values (nxnative's include/nvn.h) */
enum {
  FMT_DEPTH32F = 0x34, TARGET_2D = 1, TEXTURE_COMPRESSIBLE = 0x8,
  POOL_CPU_NO_ACCESS = 0x1, POOL_CPU_UNCACHED = 0x2, POOL_GPU_UNCACHED = 0x10, POOL_GPU_CACHED = 0x20,
  POOL_COMPRESSIBLE = 0x80,
  SYNC_ALL_GPU_COMMANDS_COMPLETE = 0, SYNC_FLUSH_FOR_CPU = 0x1, SYNC_TIMEOUT_EXPIRED = 2,
  ORIGIN_LOWER_LEFT = 0,
};

static struct {
  void (*pool_builder_defaults)(nvn_obj_t *);
  void (*pool_builder_device)(nvn_obj_t *, nvn_device_t *);
  void (*pool_builder_flags)(nvn_obj_t *, int);
  void (*pool_builder_storage)(nvn_obj_t *, void *, size_t);
  bool (*pool_init)(nvn_obj_t *, const nvn_obj_t *);
  nvn_addr_t (*pool_address)(const nvn_obj_t *);
  void (*tb_device)(void *, nvn_device_t *);
  void (*tb_defaults)(void *);
  void (*tb_target)(void *, int);
  void (*tb_format)(void *, int);
  void (*tb_size)(void *, int, int);
  void (*tb_flags)(void *, int);
  size_t (*tb_storage_size)(const void *);
  size_t (*tb_storage_align)(const void *);
  void (*tb_storage)(void *, const nvn_obj_t *, ptrdiff_t);
  bool (*texture_init)(void *, const void *);
  int (*tex_format)(const nvn_texture_t *);
  int (*tex_width)(const nvn_texture_t *);
  int (*tex_height)(const nvn_texture_t *);
  void (*copy_to_buffer)(void *, const nvn_texture_t *, const void *, const nvn_region_t *, nvn_addr_t, int);
  bool (*sync_init)(nvn_obj_t *, nvn_device_t *);
  void (*queue_fence)(nvn_queue_t *, nvn_obj_t *, int, int);
  int (*sync_wait)(const nvn_obj_t *, uint64_t);
  int (*window_origin)(const nvn_device_t *);
} d;

static const char *const d_names[] = {
  "nvnMemoryPoolBuilderSetDefaults", "nvnMemoryPoolBuilderSetDevice", "nvnMemoryPoolBuilderSetFlags",
  "nvnMemoryPoolBuilderSetStorage", "nvnMemoryPoolInitialize", "nvnMemoryPoolGetBufferAddress",
  "nvnTextureBuilderSetDevice", "nvnTextureBuilderSetDefaults", "nvnTextureBuilderSetTarget",
  "nvnTextureBuilderSetFormat", "nvnTextureBuilderSetSize2D", "nvnTextureBuilderSetFlags",
  "nvnTextureBuilderGetStorageSize", "nvnTextureBuilderGetStorageAlignment", "nvnTextureBuilderSetStorage",
  "nvnTextureInitialize", "nvnTextureGetFormat", "nvnTextureGetWidth", "nvnTextureGetHeight",
  "nvnCommandBufferCopyTextureToBuffer", "nvnSyncInitialize", "nvnQueueFenceSync", "nvnSyncWait",
  "nvnDeviceGetWindowOriginMode",
};
_Static_assert(sizeof(d_names) / sizeof(*d_names) == sizeof(d) / sizeof(void *), "scenedepth function table");

typedef void (*copy_t2t_t)(void *cmd, const nvn_texture_t *src, const void *src_view, const nvn_region_t *src_region,
                           const nvn_texture_t *dst, const void *dst_view, const nvn_region_t *dst_region, int flags);
static copy_t2t_t real_copy;

static nvn_device_t *device;
static volatile bool ready; /* the table and the buffers are made (render thread): the hook may use them */
static bool failed;
static bool lower_left;     /* the window's origin: texel rows count from the picture's bottom */
static volatile uint32_t presents;

/* the scene's depth: the largest depth copied lately (the half-size one comes after it) */
static uint32_t scene_area, scene_seen;

/* --- the health bars' points -------------------------------------------------------------------------- */

typedef struct {
  int n, n_bars;
  float x[MAX_POINTS], y[MAX_POINTS], depth[MAX_POINTS];
  uint8_t bar[MAX_POINTS];
  uintptr_t id[MAX_ASKED];
} ask_t;

/* the render thread fills them (scenedepth_ask_bars), the hook takes the newest */
static ask_t asks[PUBLISH_BUFFERS];
static publish_t ask_published = PUBLISH_INIT;

enum { SLOT_FREE, SLOT_RECORDED, SLOT_FENCED };
static volatile int slot_state[SLOTS]; /* the hook: free -> recorded; the render thread: on, and back to free */
static ask_t slot_ask[SLOTS];
static uint32_t slot_fenced[SLOTS];
static int slot_tries[SLOTS];
static nvn_obj_t slot_sync[SLOTS], result_pool;
static uint8_t *result_mem;

static volatile uint32_t *result_at(int slot, int i)
{
  return (volatile uint32_t *)(result_mem + ((size_t)slot * MAX_POINTS + i) * POINT_BYTES);
}

/* what the render thread knows of each bar, by its enemy */
static struct {
  uintptr_t id;
  bool told, hidden;
  float shown;
  uint64_t born, drawn;
} known[MAX_KNOWN];
static uint64_t last_result;

/* --- the lines' depth ------------------------------------------------------------------------------------- */

static nvn_obj_t line_pool, line_texture;
static uint8_t *line_mem;
static int line_w, line_h;
static bool line_failed;
static volatile bool line_primed;    /* cleared once (render thread), so the copies may draw into it */
static volatile uint32_t lines_asked; /* the present the lines last wanted it at */
static volatile uint32_t line_copied; /* the present of the last copy into it */
static volatile bool line_any;

static float ms_between(uint64_t then, uint64_t now) { return (float)(now - then) * 1000.0f / (float)sys_tick_freq(); }

/* --- the hook --------------------------------------------------------------------------------------------- */

/* whether the game's copy src -> dst is of the scene's depth (a whole Depth32F into one the same size, the
 * largest lately) */
static bool scene_copy(const nvn_texture_t *src, const nvn_region_t *sr, const nvn_texture_t *dst, int *w, int *h)
{
  if (!src || !dst || d.tex_format(src) != FMT_DEPTH32F || d.tex_format(dst) != FMT_DEPTH32F)
    return false;
  *w = d.tex_width(src), *h = d.tex_height(src);
  if (*w < MIN_SCENE_W || d.tex_width(dst) != *w || d.tex_height(dst) != *h)
    return false;
  if (sr && (sr->x || sr->y || sr->w != *w || sr->h != *h))
    return false;
  uint32_t area = (uint32_t)*w * (uint32_t)*h, now = presents;
  if (now - scene_seen > SCENE_GONE)
    scene_area = 0;
  if (area * 2 < scene_area) /* the half-size one */
    return false;
  scene_area = area; /* a new size under dynamic resolution follows it */
  scene_seen = now;
  return true;
}

/* the bars' points: 1x1 copies into a free slot's results */
static void copy_points(void *cmd, const nvn_texture_t *src, const void *view, int w, int h)
{
  int taken = publish_take(&ask_published);
  if (taken < 0 || !asks[taken].n) {
    publish_release(&ask_published);
    return;
  }
  const ask_t *a = &asks[taken];
  int s = 0;
  while (s < SLOTS && __atomic_load_n(&slot_state[s], __ATOMIC_ACQUIRE) != SLOT_FREE)
    s++;
  if (s == SLOTS) { /* all in flight: this frame isn't looked at */
    publish_release(&ask_published);
    return;
  }
  slot_ask[s] = *a;
  publish_release(&ask_published);
  a = &slot_ask[s];
  nvn_addr_t results = d.pool_address(&result_pool) + (nvn_addr_t)s * MAX_POINTS * POINT_BYTES;
  for (int i = 0; i < a->n; i++) {
    int px = (int)(a->x[i] * w), py = (int)((lower_left ? 1 - a->y[i] : a->y[i]) * h);
    px = px < 0 ? 0 : px >= w ? w - 1 : px;
    py = py < 0 ? 0 : py >= h ? h - 1 : py;
    nvn_region_t texel = { px, py, 0, 1, 1, 1 }; /* one texel: the copy row stride (the game's) doesn't count */
    d.copy_to_buffer(cmd, src, view, &texel, results + (nvn_addr_t)i * POINT_BYTES, 0);
  }
  __atomic_store_n(&slot_state[s], SLOT_RECORDED, __ATOMIC_RELEASE);
}

static void hook_copy(void *cmd, const nvn_texture_t *src, const void *sv, const nvn_region_t *sr,
                      const nvn_texture_t *dst, const void *dv, const nvn_region_t *dr, int flags)
{
  real_copy(cmd, src, sv, sr, dst, dv, dr, flags);
  int w, h;
  if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE) || !cmd || !scene_copy(src, sr, dst, &w, &h))
    return;
  uint32_t now = presents;
  if (line_primed && !line_failed && now - lines_asked <= LINES_KEEP) {
    nvn_region_t from = { 0, 0, 0, w, h, 1 }, into = { 0, 0, 0, line_w, line_h, 1 };
    real_copy(cmd, src, sv, &from, (const nvn_texture_t *)&line_texture, 0, &into, 0); /* scaled if need be, nearest */
    line_copied = now;
    line_any = true;
  }
  copy_points(cmd, src, sv, w, h);
}

void *scenedepth_wrap(const char *name, void *proc)
{
  static const char want[] = "nvnCommandBufferCopyTextureToTexture";
  int i = 0;
  while (want[i] && name[i] == want[i])
    i++;
  if (!proc || want[i] || name[i])
    return 0;
  real_copy = (copy_t2t_t)proc;
  return (void *)hook_copy;
}

/* --- set up ------------------------------------------------------------------------------------------------ */

static bool make_pool(nvn_obj_t *pool, void *mem, size_t size, int flags)
{
  nvn_obj_t builder;
  __builtin_memset(&builder, 0, sizeof(builder));
  d.pool_builder_defaults(&builder);
  d.pool_builder_device(&builder, device);
  d.pool_builder_flags(&builder, flags);
  d.pool_builder_storage(&builder, mem, size);
  return d.pool_init(pool, &builder);
}

static bool init(getproc_t getproc, nvn_device_t *dev)
{
  if (!getproc || !dev || !real_copy)
    return false; /* not yet */
  device = dev;
  void **slots = (void **)&d;
  for (unsigned i = 0; i < sizeof(d_names) / sizeof(*d_names); i++)
    if (!(slots[i] = getproc(dev, d_names[i]))) {
      log_printf("scene depth: NVN has no %s; the lines and bars show through as before", d_names[i]);
      failed = true;
      return false;
    }
  result_mem = aligned_alloc(PAGE, RESULT_SIZE);
  bool ok = result_mem && make_pool(&result_pool, result_mem, RESULT_SIZE, POOL_CPU_UNCACHED | POOL_GPU_UNCACHED);
  for (int s = 0; s < SLOTS && ok; s++)
    ok = d.sync_init(&slot_sync[s], device);
  if (!ok) {
    log_printf("scene depth: no memory or sync for the bars' read-back; they show through the scenery");
    failed = true;
    return false;
  }
  for (int s = 0; s < SLOTS; s++)
    for (int i = 0; i < MAX_POINTS; i++)
      *result_at(s, i) = UNWRITTEN;
  lower_left = d.window_origin(device) == ORIGIN_LOWER_LEFT;
  __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
  log_printf("scene depth ready (origin %s)", lower_left ? "lower left" : "upper left");
  return true;
}

/* --- the bars ---------------------------------------------------------------------------------------------- */

static int known_slot(uintptr_t id, uint64_t now, bool make)
{
  int free_slot = -1;
  for (int i = 0; i < MAX_KNOWN; i++) {
    if (known[i].drawn && known[i].id == id)
      return i;
    if (free_slot < 0 && (!known[i].drawn || ms_between(known[i].drawn, now) > FORGET_MS))
      free_slot = i;
  }
  if (!make || free_slot < 0)
    return -1;
  known[free_slot].id = id;
  known[free_slot].told = known[free_slot].hidden = false;
  known[free_slot].shown = 0;
  known[free_slot].born = known[free_slot].drawn = now;
  return free_slot;
}

/* slot s's results, if its copies have all landed: each bar hidden while all its points are */
static bool read_slot(int s, uint64_t now)
{
  const ask_t *a = &slot_ask[s];
  bool shows[MAX_ASKED] = { false };
  for (int i = 0; i < a->n; i++) {
    uint32_t bits = *result_at(s, i);
    if (bits == UNWRITTEN)
      return false; /* the command buffer with its copies wasn't in before the fence */
    float depth;
    __builtin_memcpy(&depth, &bits, 4);
    if (!(depth < a->depth[i])) /* nothing nearer there than the point (with its margin) */
      shows[a->bar[i]] = true;
  }
  for (int b = 0; b < a->n_bars; b++) {
    int k = known_slot(a->id[b], now, false);
    if (k >= 0) {
      known[k].told = true;
      known[k].hidden = !shows[b];
    }
  }
  last_result = now;
  return true;
}

void scenedepth_present(getproc_t getproc, nvn_device_t *dev, nvn_queue_t *queue)
{
  presents++;
  if (!ready && (failed || !init(getproc, dev)))
    return;
  uint64_t now = sys_tick();
  for (int s = 0; s < SLOTS; s++) {
    int state = __atomic_load_n(&slot_state[s], __ATOMIC_ACQUIRE);
    if (state == SLOT_RECORDED) {
      d.queue_fence(queue, &slot_sync[s], SYNC_ALL_GPU_COMMANDS_COMPLETE, SYNC_FLUSH_FOR_CPU);
      slot_fenced[s] = presents;
      slot_tries[s] = 0;
      __atomic_store_n(&slot_state[s], SLOT_FENCED, __ATOMIC_RELEASE);
    } else if (state == SLOT_FENCED && presents - slot_fenced[s] >= READ_AFTER) {
      if (d.sync_wait(&slot_sync[s], 0) >= SYNC_TIMEOUT_EXPIRED)
        continue;
      if (!read_slot(s, now) && ++slot_tries[s] < MAX_TRIES) { /* fenced again, behind the copies' commands */
        d.queue_fence(queue, &slot_sync[s], SYNC_ALL_GPU_COMMANDS_COMPLETE, SYNC_FLUSH_FOR_CPU);
        slot_fenced[s] = presents;
        continue;
      }
      for (int i = 0; i < MAX_POINTS; i++)
        *result_at(s, i) = UNWRITTEN;
      __atomic_store_n(&slot_state[s], SLOT_FREE, __ATOMIC_RELEASE);
    }
  }
}

void scenedepth_ask_bars(const eh_bar_t *bars, int n)
{
  if (!ready)
    return;
  int f = publish_next(&ask_published);
  ask_t *a = &asks[f];
  a->n = a->n_bars = 0;
  /* the nearest MAX_ASKED: picked one by one (n is small) */
  bool taken[64] = { false };
  n = n < 64 ? n : 64;
  while (a->n_bars < MAX_ASKED) {
    int best = -1;
    for (int i = 0; i < n; i++)
      if (!taken[i] && bars[i].n_probes > 0 && (best < 0 || bars[i].distance < bars[best].distance))
        best = i;
    if (best < 0)
      break;
    taken[best] = true;
    const eh_bar_t *b = &bars[best];
    for (int p = 0; p < b->n_probes && a->n < MAX_POINTS; p++) {
      a->x[a->n] = b->probe_x[p];
      a->y[a->n] = b->probe_y[p];
      a->depth[a->n] = b->probe_depth[p];
      a->bar[a->n] = (uint8_t)a->n_bars;
      a->n++;
    }
    a->id[a->n_bars++] = b->id;
  }
  publish_set(&ask_published, f);
}

float scenedepth_bar_alpha(uintptr_t id, uint64_t now)
{
  if (!ready || !last_result || ms_between(last_result, now) > NO_DEPTH_MS)
    return 1; /* no depth to go by */
  int k = known_slot(id, now, true);
  if (k < 0)
    return 1;
  float dt = ms_between(known[k].drawn, now);
  known[k].drawn = now;
  bool show = known[k].told ? !known[k].hidden : ms_between(known[k].born, now) > PENDING_MS;
  float step = dt / FADE_MS, v = known[k].shown + (show ? step : -step);
  known[k].shown = v = v < 0 ? 0 : v > 1 ? 1 : v;
  return v * v * (3 - 2 * v);
}

/* --- the lines ---------------------------------------------------------------------------------------------- */

static bool make_line_texture(int w, int h)
{
  nvn_obj_t builder;
  __builtin_memset(&builder, 0, sizeof(builder));
  d.tb_device(&builder, device);
  d.tb_defaults(&builder);
  d.tb_target(&builder, TARGET_2D);
  d.tb_format(&builder, FMT_DEPTH32F);
  d.tb_size(&builder, w, h);
  d.tb_flags(&builder, TEXTURE_COMPRESSIBLE); /* as the game's own depth */
  size_t size = d.tb_storage_size(&builder), align = d.tb_storage_align(&builder);
  if (align < PAGE)
    align = PAGE;
  size = (size + align - 1) / align * align;
  line_mem = aligned_alloc(align, size);
  if (!line_mem || !make_pool(&line_pool, line_mem, size, POOL_CPU_NO_ACCESS | POOL_GPU_CACHED | POOL_COMPRESSIBLE)) {
    log_printf("scene depth: no memory for the lines' depth (%u KiB); they show through Link",
               (unsigned)(size / 1024));
    gfx_notice_later("air walls: no memory to hide them behind Link"); /* render thread */
    free(line_mem);
    line_mem = 0;
    return false;
  }
  d.tb_storage(&builder, &line_pool, 0);
  if (!d.texture_init(&line_texture, &builder)) {
    log_printf("scene depth: the lines' depth texture didn't initialise");
    return false;
  }
  line_w = w, line_h = h;
  log_printf("scene depth: the lines' depth %dx%d (%u KiB)", w, h, (unsigned)(size / 1024));
  return true;
}

const nvn_texture_t *scenedepth_lines(int area_w, int area_h, int *w, int *h, bool *prime)
{
  *prime = false;
  uint32_t now = presents;
  lines_asked = now;
  if (!ready || line_failed || area_w <= 0 || area_h <= 0)
    return 0;
  if (!line_mem) { /* the area's shape, at most LINES_PIXELS: made once */
    float k = 1;
    while ((float)area_w * k * (float)area_h * k > (float)LINES_PIXELS)
      k *= 0.99f;
    if (!make_line_texture((int)(area_w * k) & ~7, (int)(area_h * k) & ~7)) {
      line_failed = true;
      return 0;
    }
  }
  *w = line_w, *h = line_h;
  if (!line_primed) {
    *prime = true;
    return (const nvn_texture_t *)&line_texture;
  }
  return line_any && now - line_copied <= LINES_FRESH ? (const nvn_texture_t *)&line_texture : 0;
}

void scenedepth_primed(void)
{
  line_primed = true;
}

void scenedepth_lines_give_up(void)
{
  line_failed = true;
}
