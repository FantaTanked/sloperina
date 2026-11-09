#include "link.h"

#include "sys.h"
#include "z64hd.h"

static const struct { uint32_t a, b; } linkobj_ptrs[] = Z64HD_LINKOBJ_PTRS;

uintptr_t link_actor(void)
{
  for (unsigned i = 0; i < sizeof(linkobj_ptrs) / sizeof(*linkobj_ptrs); i++) {
    uintptr_t st = sys_ptr(MAIN(linkobj_ptrs[i].a));
    uintptr_t o = st ? sys_ptr(st + linkobj_ptrs[i].b) : 0;
    if (o && sys_ptr(o) == MAIN(Z64HD_VT_LINKOBJ))
      return o;
  }
  return 0;
}

/* Health: [[link+8]+0x6A0] holds parameter objects (vtable main+VT_PARAM) at +0x1C10 (current)
 * and +0x1C18 (max); the value is at +8 (oot26-tools). */
bool link_health(int32_t **cur, int32_t **max)
{
  uintptr_t o = link_actor();
  uintptr_t s = o ? sys_ptr(o + 8) : 0;
  uintptr_t o2 = s ? sys_ptr(s + 0x6A0) : 0;
  if (!o2)
    return false;
  uintptr_t p = sys_ptr(o2 + 0x1C10), m = sys_ptr(o2 + 0x1C18);
  if (!p || !m || sys_ptr(p) != MAIN(Z64HD_VT_PARAM) || sys_ptr(m) != MAIN(Z64HD_VT_PARAM))
    return false;
  *cur = (int32_t *)(p + 8);
  *max = (int32_t *)(m + 8);
  return true;
}

bool link_drawn_pos(vec3f_t *out)
{
  uintptr_t q = sys_ptr(MAIN(Z64HD_DISP_PTR));
  q = q ? sys_ptr(q) : 0;
  if (!q || !sys_mapped(q + 0x29C, 12, SYS_PERM_R))
    return false;
  *out = *(vec3f_t *)(q + 0x29C);
  return true;
}

/* actor +0x328 cos, +0x330 sin (oot26-tools) */
bool link_facing(double *fx, double *fz)
{
  uintptr_t o = link_actor();
  if (!o || !sys_mapped(o + 0x328, 12, SYS_PERM_R))
    return false;
  *fz = *(float *)(o + 0x328), *fx = *(float *)(o + 0x330);
  return true;
}

uintptr_t camera(void)
{
  uintptr_t p = sys_ptr(MAIN(Z64HD_CAMERA_PTR));
  return p ? sys_ptr(p + 0x68) : 0;
}

uint32_t frame_counter(void)
{
  return *(volatile uint32_t *)MAIN(Z64HD_FRAME_COUNTER);
}

/* --- heap search ------------------------------------------------------------------------- */

struct find_ctx {
  uint64_t value;
  bool (*ok)(uintptr_t, void *);
  void *ctx;
  uintptr_t found;
};

static void find_in_region(uintptr_t base, size_t size, void *p)
{
  struct find_ctx *f = p;
  if (f->found)
    return;
  const volatile uint64_t *w = (const uint64_t *)base;
  size_t n = size / 8;
  for (size_t i = 0; i < n; i++)
    if (w[i] == f->value && f->ok((uintptr_t)&w[i], f->ctx)) {
      f->found = (uintptr_t)&w[i];
      return;
    }
}

static void find_in_heap(uintptr_t base, size_t size, void *p)
{
  sys_meminfo_t m;
  if (sys_query_memory(&m, base) == 0 && (m.type & 0xFF) == 0x05 /* Normal (heap) */)
    find_in_region(base, size, p);
}

uintptr_t mem_find_u64(uint64_t value, bool (*ok)(uintptr_t, void *), void *ctx)
{
  struct find_ctx f = { value, ok, ctx, 0 };
  uint64_t t0 = sys_tick();
  sys_regions(SYS_PERM_R | SYS_PERM_W, find_in_heap, &f);
  log_printf("heap search for %llx: %s in %llu ms", (unsigned long long)value,
             f.found ? "found" : "not found",
             (unsigned long long)((sys_tick() - t0) * 1000 / sys_tick_freq()));
  return f.found;
}

/* --- equipment ---------------------------------------------------------------------------- */

static uintptr_t equip_comp, equip_owner;

static bool is_link_component(uintptr_t c, uintptr_t link)
{
  return sys_ptr(c) == MAIN(Z64HD_VT_EQUIP) && sys_ptr(c + 0x18) == link;
}

static bool comp_ok(uintptr_t addr, void *link)
{
  return sys_ptr(addr + 0x18) == (uintptr_t)link;
}

uintptr_t link_equip_component(void)
{
  uintptr_t link = link_actor();
  if (!link)
    return 0;
  if (equip_comp && is_link_component(equip_comp, link))
    return equip_comp;
  equip_comp = 0;
  /* near Link: objects his actor points to (cheap) */
  if (sys_mapped(link, 0x2000, SYS_PERM_R))
    for (uintptr_t o = 0; o < 0x2000; o += 8) {
      uintptr_t c = *(uintptr_t *)(link + o);
      if (c && !(c & 7) && is_link_component(c, link))
        return equip_comp = c;
    }
  return equip_comp = mem_find_u64(MAIN(Z64HD_VT_EQUIP), comp_ok, (void *)link);
}

static bool owner_ok(uintptr_t addr, void *comp)
{
  (void)comp;
  return sys_ptr(addr - 0x470) == MAIN(Z64HD_VT_EQUIP_OWNER);
}

uintptr_t link_equip_owner(void)
{
  uintptr_t comp = link_equip_component();
  if (!comp)
    return 0;
  if (equip_owner && sys_ptr(equip_owner) == MAIN(Z64HD_VT_EQUIP_OWNER) &&
      sys_ptr(equip_owner + 0x470) == comp)
    return equip_owner;
  uintptr_t link = link_actor();
  if (link && sys_ptr(link) && sys_mapped(link, 0x2000, SYS_PERM_R))
    for (uintptr_t o = 0; o < 0x2000; o += 8) {
      uintptr_t c = *(uintptr_t *)(link + o);
      if (c && !(c & 7) && sys_ptr(c) == MAIN(Z64HD_VT_EQUIP_OWNER) && sys_ptr(c + 0x470) == comp)
        return equip_owner = c;
    }
  uintptr_t at = mem_find_u64(comp, owner_ok, (void *)comp);
  return equip_owner = at ? at - 0x470 : 0;
}
