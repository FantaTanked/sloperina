#include "link.h"

#include "sys.h"
#include "z64hd.h"

#define MEMORY_TYPE_MASK 0xFF
#define MEMORY_TYPE_NORMAL 0x05 /* ordinary heap memory */

/* Link's actor */
#define ACTOR_STATE_PTR 0x8       /* actor +8: an object holding... */
#define STATE_PARAMS_PTR 0x6A0    /* ...at +0x6A0, the parameter objects */
#define PARAMS_HEALTH_CUR 0x1C10  /* parameter object: current health */
#define PARAMS_HEALTH_MAX 0x1C18  /* parameter object: maximum health */
#define PARAM_VALUE 0x8           /* a parameter object's value */

/* the drawn-position object */
#define DISP_POSITION 0x29C

/* the camera manager's active camera */
#define CAMERA_MANAGER_CAMERA 0x68

/* equipment */
#define EQUIP_COMPONENT_LINK 0x18 /* component +0x18: its owner actor (Link) */
#define EQUIP_OWNER_COMPONENT 0x470
#define LINK_NEARBY_BYTES 0x2000  /* how much of Link's actor to look through for pointers */

uintptr_t link_actor(void)
{
  const struct z64hd_link_path *paths = z64hd_game->link_paths;
  for (unsigned i = 0; i < sizeof(z64hd_game->link_paths) / sizeof(*paths) && paths[i].global; i++) {
    uintptr_t holder = sys_ptr(MAIN(paths[i].global));
    uintptr_t actor = holder ? sys_ptr(holder + paths[i].member) : 0;
    if (actor && sys_ptr(actor) == MAIN(z64hd_game->vt_linkobj))
      return actor;
  }
  return 0;
}

/* Health: [[link+8]+0x6A0] holds parameter objects (vtable main+VT_PARAM) at +0x1C10 (current)
 * and +0x1C18 (max); the value is at +8 (oot26-tools). */
bool link_health(int32_t **cur, int32_t **max)
{
  uintptr_t actor = link_actor();
  uintptr_t state = actor ? sys_ptr(actor + ACTOR_STATE_PTR) : 0;
  uintptr_t params = state ? sys_ptr(state + STATE_PARAMS_PTR) : 0;
  if (!params)
    return false;
  uintptr_t cur_param = sys_ptr(params + PARAMS_HEALTH_CUR);
  uintptr_t max_param = sys_ptr(params + PARAMS_HEALTH_MAX);
  if (!cur_param || !max_param || sys_ptr(cur_param) != MAIN(z64hd_game->vt_param) ||
      sys_ptr(max_param) != MAIN(z64hd_game->vt_param))
    return false;
  *cur = (int32_t *)(cur_param + PARAM_VALUE);
  *max = (int32_t *)(max_param + PARAM_VALUE);
  return true;
}

bool link_drawn_pos(vec3f_t *out)
{
  uintptr_t disp = sys_ptr(MAIN(z64hd_game->disp_ptr));
  disp = disp ? sys_ptr(disp) : 0;
  if (!disp || !sys_mapped(disp + DISP_POSITION, 12, SYS_PERM_R))
    return false;
  *out = *(vec3f_t *)(disp + DISP_POSITION);
  return true;
}

/* Link's facing as a unit (x, z): actor +0x328 cos, +0x330 sin (oot26-tools; 0 = +z) */
bool link_facing(double *fx, double *fz)
{
  uintptr_t actor = link_actor();
  if (!actor || !sys_mapped(actor + 0x328, 12, SYS_PERM_R))
    return false;
  *fz = *(float *)(actor + 0x328);
  *fx = *(float *)(actor + 0x330);
  return true;
}

uintptr_t camera(void)
{
  uintptr_t manager = sys_ptr(MAIN(z64hd_game->camera_ptr));
  return manager ? sys_ptr(manager + CAMERA_MANAGER_CAMERA) : 0;
}

uint32_t frame_counter(void)
{
  return *(volatile uint32_t *)MAIN(z64hd_game->frame_counter);
}

/* --- heap search ------------------------------------------------------------------------- */

struct find_ctx {
  uint64_t value;
  bool (*ok)(uintptr_t, void *);
  void *ctx;
  uintptr_t found;
};

static void find_in_region(uintptr_t base, size_t size, void *ctx)
{
  struct find_ctx *find = ctx;
  if (find->found)
    return;
  const volatile uint64_t *words = (const uint64_t *)base;
  size_t n_words = size / 8;
  for (size_t i = 0; i < n_words; i++)
    if (words[i] == find->value && find->ok((uintptr_t)&words[i], find->ctx)) {
      find->found = (uintptr_t)&words[i];
      return;
    }
}

static void find_in_heap(uintptr_t base, size_t size, void *ctx)
{
  sys_meminfo_t info;
  if (sys_query_memory(&info, base) == 0 && (info.type & MEMORY_TYPE_MASK) == MEMORY_TYPE_NORMAL)
    find_in_region(base, size, ctx);
}

uintptr_t mem_find_u64(uint64_t value, bool (*ok)(uintptr_t, void *), void *ctx)
{
  struct find_ctx find = { value, ok, ctx, 0 };
  uint64_t start_tick = sys_tick();
  sys_regions(SYS_PERM_R | SYS_PERM_W, find_in_heap, &find);
  log_printf("heap search for %llx: %s in %llu ms", (unsigned long long)value,
             find.found ? "found" : "not found",
             (unsigned long long)((sys_tick() - start_tick) * 1000 / sys_tick_freq()));
  return find.found;
}

uintptr_t mem_find_u64_step(mem_scan_t *scan, uint64_t value, bool (*ok)(uintptr_t, void *), void *ctx,
                            size_t budget)
{
  const uint32_t rw = SYS_PERM_R | SYS_PERM_W;
  for (int guard = 0; guard < 4096 && budget >= 8; guard++) {
    /* asked again every call: the game maps and unmaps memory between frames */
    sys_meminfo_t info;
    if (sys_query_memory(&info, scan->pos) != 0 || info.size == 0 || info.addr + info.size <= scan->pos) {
      scan->done = true;
      return 0;
    }
    uintptr_t end = (uintptr_t)(info.addr + info.size);
    if ((info.type & MEMORY_TYPE_MASK) != MEMORY_TYPE_NORMAL || (info.perm & rw) != rw || (info.attr & 1)) {
      scan->pos = end;
      continue;
    }
    uintptr_t stop = end - scan->pos > budget ? scan->pos + budget : end;
    for (uintptr_t p = scan->pos; p + 8 <= stop; p += 8)
      if (*(const volatile uint64_t *)p == value && ok(p, ctx)) {
        scan->pos = p + 8;
        return p;
      }
    budget -= stop - scan->pos;
    scan->pos = stop;
  }
  return 0;
}

/* --- equipment ---------------------------------------------------------------------------- */

static uintptr_t cached_equip_component, cached_equip_owner;

static bool is_link_component(uintptr_t component, uintptr_t link)
{
  return sys_ptr(component) == MAIN(z64hd_game->vt_equip) && sys_ptr(component + EQUIP_COMPONENT_LINK) == link;
}

static bool is_links_component_hit(uintptr_t addr, void *link)
{
  return sys_ptr(addr + EQUIP_COMPONENT_LINK) == (uintptr_t)link;
}

/* Pointers can only be 8-byte aligned and non-null. */
static bool looks_like_pointer(uintptr_t value)
{
  return value && !(value & 7);
}

uintptr_t link_equip_component(void)
{
  uintptr_t link = link_actor();
  if (!link)
    return 0;
  if (cached_equip_component && is_link_component(cached_equip_component, link))
    return cached_equip_component;
  cached_equip_component = 0;
  /* near Link first: objects his actor points to (cheap) */
  if (sys_mapped(link, LINK_NEARBY_BYTES, SYS_PERM_R))
    for (uintptr_t offset = 0; offset < LINK_NEARBY_BYTES; offset += 8) {
      uintptr_t candidate = *(uintptr_t *)(link + offset);
      if (looks_like_pointer(candidate) && is_link_component(candidate, link)) {
        cached_equip_component = candidate;
        return cached_equip_component;
      }
    }
  cached_equip_component = mem_find_u64(MAIN(z64hd_game->vt_equip), is_links_component_hit, (void *)link);
  return cached_equip_component;
}

static bool is_owner_hit(uintptr_t addr, void *component)
{
  (void)component;
  return sys_ptr(addr - EQUIP_OWNER_COMPONENT) == MAIN(z64hd_game->vt_equip_owner);
}

static bool is_equip_owner_of(uintptr_t owner, uintptr_t component)
{
  return sys_ptr(owner) == MAIN(z64hd_game->vt_equip_owner) && sys_ptr(owner + EQUIP_OWNER_COMPONENT) == component;
}

uintptr_t link_equip_owner(void)
{
  uintptr_t component = link_equip_component();
  if (!component)
    return 0;
  if (cached_equip_owner && is_equip_owner_of(cached_equip_owner, component))
    return cached_equip_owner;
  uintptr_t link = link_actor();
  if (link && sys_ptr(link) && sys_mapped(link, LINK_NEARBY_BYTES, SYS_PERM_R))
    for (uintptr_t offset = 0; offset < LINK_NEARBY_BYTES; offset += 8) {
      uintptr_t candidate = *(uintptr_t *)(link + offset);
      if (looks_like_pointer(candidate) && is_equip_owner_of(candidate, component)) {
        cached_equip_owner = candidate;
        return cached_equip_owner;
      }
    }
  uintptr_t hit = mem_find_u64(component, is_owner_hit, (void *)component);
  cached_equip_owner = hit ? hit - EQUIP_OWNER_COMPONENT : 0;
  return cached_equip_owner;
}
