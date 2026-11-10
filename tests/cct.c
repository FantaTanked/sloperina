/* Real chain/lifetime logic with fake game objects and queued physics API calls. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "z64hd.h"
#include "link.h"

static void fake_no_hit(uintptr_t body, bool on);
static void fake_position(uintptr_t property, const vec3f_t *p);
static void fake_velocity(uintptr_t body, const vec3f_t *v);
static uintptr_t function_at(unsigned offset)
{
  if (offset == Z64HD_CCT_FORCE_NO_HIT) return (uintptr_t)fake_no_hit;
  if (offset == Z64HD_CCT_SET_VELOCITY) return (uintptr_t)fake_velocity;
  assert(offset == Z64HD_CCT_SET_POSITION);
  return (uintptr_t)fake_position;
}
#undef MAIN_FN
#define MAIN_FN(type, off) ((type)function_at(off))
#include "../src/ootgz/cct.c"

static uint64_t objects[12][1024]; /* 11: a second body */
#define A(n) ((uintptr_t)objects[n])
uintptr_t z64hd_main = 0x100000000ull;
static const struct z64hd_build test_build = {
  .vt_cct_property = 0x4C34F10, .vt_cct_body = 0x4C34658, .cct_force_no_hit = 0x158D40,
  .cct_set_position = 0x6B2530, .cct_set_velocity = 0x12DBE0,
};
const struct z64hd_build *z64hd_game = &test_build;
static uintptr_t actor, blocked;
static bool read_only;
static unsigned toggles, toggles2, moves, velocity_resets;
static vec3f_t last_position, last_velocity;
static void putp(int i, unsigned off, uintptr_t p) { memcpy((char *)objects[i] + off, &p, 8); }
static void put32(int i, unsigned off, uint32_t v) { memcpy((char *)objects[i] + off, &v, 4); }
uintptr_t link_actor(void) { return actor; }
bool sys_mapped(uintptr_t a, size_t n, uint32_t perm)
{
  if (n > UINTPTR_MAX - a || (read_only && (perm & SYS_PERM_W))) return false;
  for (unsigned i = 0; i < 12; i++)
    if (a >= A(i) && a + n <= A(i) + sizeof(objects[i]) && A(i) != blocked) return true;
  return false;
}
uintptr_t sys_ptr(uintptr_t a)
{
  uintptr_t p = 0;
  if (sys_mapped(a, 8, SYS_PERM_R)) memcpy(&p, (void *)a, 8);
  return p;
}
bool sys_read32(uintptr_t a, uint32_t *v)
{
  if (!sys_mapped(a, 4, SYS_PERM_R)) return false;
  memcpy(v, (void *)a, 4); return true;
}
void log_printf(const char *fmt, ...) { (void)fmt; }
static void fake_no_hit(uintptr_t body, bool on)
{
  if (body == A(11)) { /* the second body: no queue, applied at once */
    toggles2++;
    putp(11, 0x70, on ? 1ull << 11 : 0);
    return;
  }
  assert(body == A(6));
  toggles++;
  putp(8, 0x100, 1ull << 19);
  put32(8, 0xF8, on); /* queued state, applied body flag deliberately stays unchanged */
}
static void fake_position(uintptr_t property, const vec3f_t *p)
{
  assert(property == A(5)); moves++; last_position = *p;
}
static void fake_velocity(uintptr_t body, const vec3f_t *v)
{
  assert(body == A(6)); velocity_resets++; last_velocity = *v;
}
static void reset(void)
{
  memset(objects, 0, sizeof(objects));
  owner = (controller_t){0}; actor = A(0); blocked = 0; read_only = false;
  toggles = toggles2 = moves = velocity_resets = 0;
  putp(0, 0x270, A(1)); putp(1, 0x50, A(2)); putp(2, 0x20, A(3));
  putp(3, 0x10, A(4)); putp(4, 8, A(5));
  putp(5, 0, MAIN(Z64HD_VT_CCT_PROPERTY)); putp(5, 0x20, A(6)); putp(5, 0x58, A(4));
  putp(6, 0, MAIN(Z64HD_VT_CCT_BODY)); putp(6, 0x60, A(8));
  put32(4, 0x10, 1); putp(4, 0x18, A(7)); putp(7, 0, A(6));
  putp(5, 0x48, A(2)); putp(2, 0x40, A(1));
  putp(1, 0x358, A(9)); putp(1, 0x2B8, A(10));
  putp(9, 0x18, A(0)); putp(10, 0x1340, A(0));
}
static void test_fall(void)
{
  reset();
  vec3f_t velocity = {3, -60, -2};
  memcpy((void *)(A(6) + 0xE0), &velocity, sizeof(velocity));
  assert(cct_reset_vertical_velocity(actor));
  assert(velocity_resets == 1 && last_velocity.x == 3 && last_velocity.y == 0 && last_velocity.z == -2);
  assert(!cct_reset_vertical_velocity(actor + 8) && velocity_resets == 1);
  blocked = A(6); assert(!cct_reset_vertical_velocity(actor)); blocked = 0;
  bool grounded = false;
  assert(cct_on_ground(actor, &grounded) && grounded);
  put32(1, 0x10, 1); assert(cct_on_ground(actor, &grounded) && !grounded);
  put32(2, 0x58, 3); assert(!cct_on_ground(actor, &grounded));

  float y = 90;
  memcpy((void *)(actor + 0x320), &y, 4);
  put32(9, 0x18E8, 0x7F7FFFFF); /* JS Height must remain untouched. */
  assert(cct_clear_fall_distance(actor));
  assert(*(float *)(A(9) + 0x18D4) == y && *(float *)(A(9) + 0x18E0) == y);
  assert(*(float *)(A(9) + 0x190C) == 0 && *(float *)(A(10) + 0x189C) == 0);
  assert(*(float *)(A(10) + 0x1898) == y && *(uint32_t *)(A(9) + 0x18E8) == 0x7F7FFFFF);
  static uint64_t before[12][1024];
  for (int invalid = 0; invalid < 4; invalid++) {
    reset();
    if (invalid == 0) blocked = A(9);
    if (invalid == 1) putp(10, 0x1340, actor + 8);
    if (invalid == 2) read_only = true;
    if (invalid == 3) put32(0, 0x320, 0x7FC00000);
    memcpy(before, objects, sizeof(objects));
    assert(!cct_clear_fall_distance(actor));
    assert(!memcmp(before, objects, sizeof(objects)));
  }
  puts("PASS: terminal velocity reset, horizontal motion preserved, ground state and fall-history guards");
}
int main(void)
{
  test_fall();
  vec3f_t p = { 2, 100, -3 };
  reset(); assert(cct_noclip_begin() && toggles == 1);
  assert(cct_noclip_begin() && toggles == 1);
  assert(cct_noclip_move(&p) && moves == 1 && !memcmp(&p, &last_position, sizeof(p)));
  assert(toggles == 1); /* pending enable prevents repeated requests */
  cct_noclip_end(); assert(!cct_noclip_active() && toggles == 2 && !no_hit(A(6)));
  cct_noclip_end(); assert(toggles == 2);

  for (int queued = 0; queued < 2; queued++) {
    reset();
    if (queued) { putp(8, 0x100, 1ull << 19); put32(8, 0xF8, 1); }
    else putp(6, 0x70, 1ull << 11);
    assert(cct_noclip_begin()); cct_noclip_end();
    assert(toggles == 0); /* preserve already-disabled collision */
  }
  for (int i = 0; i < 9; i++) {
    reset(); blocked = A(i);
    assert(!cct_noclip_begin() && !cct_noclip_active() && toggles == 0);
  }
  reset(); read_only = true; assert(!cct_noclip_begin());
  reset(); putp(5, 0, MAIN(Z64HD_VT_CCT_BODY)); assert(!cct_noclip_begin());
  reset(); putp(6, 0, 0); assert(!cct_noclip_begin());
  reset(); put32(4, 0x10, 129); assert(!cct_noclip_begin());
  reset(); putp(1, 0x50, UINTPTR_MAX); assert(!cct_noclip_begin());
  reset(); assert(cct_noclip_begin());
  blocked = A(1); assert(!cct_noclip_move(&p)); cct_noclip_end();
  assert(cct_noclip_active() && toggles == 1); /* retry temporary unavailability */
  blocked = 0; cct_noclip_end(); assert(!cct_noclip_active() && toggles == 2);
  reset(); assert(cct_noclip_begin());
  actor = A(0) + 8; putp(0, 0x278, A(1)); /* new actor, same-looking reused controller */
  assert(!cct_noclip_move(&p)); cct_noclip_end();
  assert(toggles == 1 && !cct_noclip_active()); /* no writes to a replaced owner */
  /* A ledge grab makes the second body current: the bypass follows, and both get their own modes back. */
  for (int scenario = 0; scenario < 3; scenario++) {
    reset();
    putp(11, 0, MAIN(Z64HD_VT_CCT_BODY)); put32(4, 0x10, 2); putp(7, 8, A(11));
    if (scenario == 2) putp(11, 0x70, 1ull << 11); /* already without collision */
    assert(cct_noclip_begin() && toggles == 1 && toggles2 == 0);
    putp(5, 0x20, A(11));
    if (scenario == 0) { /* turned off before another frame moved him */
      cct_noclip_end();
      assert(!cct_noclip_active() && toggles == 2 && toggles2 == 0 && !no_hit(A(6)) && !no_hit(A(11)));
      continue;
    }
    assert(cct_noclip_move(&p) && cct_noclip_active() && no_hit(A(11)));
    assert(toggles2 == (scenario == 2 ? 0u : 1u));
    putp(5, 0x20, A(6)); /* and back */
    assert(cct_noclip_begin() && cct_noclip_move(&p) && toggles == 1);
    putp(5, 0x20, A(11));
    cct_noclip_end();
    assert(!cct_noclip_active() && toggles == 2 && !no_hit(A(6)));
    assert(no_hit(A(11)) == (scenario == 2) && toggles2 == (scenario == 2 ? 0u : 2u));
  }
  reset(); assert(cct_noclip_begin()); p.y = NAN;
  assert(!cct_noclip_move(&p) && moves == 0);
  p.y = INFINITY; assert(!cct_noclip_move(&p));
  p.y = 100000; assert(!cct_noclip_move(&p));
  puts("PASS: CCT chain/types, queued collision state, restore/retry, body switches, relocation, invalid lists/positions");
  return 0;
}
