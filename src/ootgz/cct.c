/* Use the same physics APIs as OneShotPhysicsCctChangePosition and
 * OneshotChangeForceNoHitMode. Writing a scanned position record alone leaves the
 * collision solver active and can disagree with the controller's other transforms.
 * This module changes neither code nor collision geometry. */
#include "cct.h"

#include "sys.h"
#include "z64hd.h"

typedef struct { uintptr_t actor, property, body; } controller_t;
static controller_t owner;
/* The controller has a body per stance and the game swaps which is current (a ledge grab, a climb),
 * so each attached body's collision mode from before the bypass is kept. */
#define MAX_BODIES 8
static struct { uintptr_t body; bool no_hit; } saved[MAX_BODIES];
static unsigned n_saved;
static bool restore_wait_logged;

static uintptr_t next(uintptr_t p, size_t offset)
{
  return p && p <= UINTPTR_MAX - offset ? sys_ptr(p + offset) : 0;
}

static bool writable(uintptr_t p, size_t size)
{
  return p && !(p & 7) && sys_mapped(p, size, SYS_PERM_R | SYS_PERM_W);
}

static bool resolve(controller_t *out)
{
  /* not known for this build: nothing to call */
  if (!z64hd_game || !Z64HD_VT_CCT_PROPERTY || !Z64HD_VT_CCT_BODY || !Z64HD_CCT_FORCE_NO_HIT ||
      !Z64HD_CCT_SET_POSITION || !Z64HD_CCT_SET_VELOCITY)
    return false;
  uintptr_t actor = link_actor(), p = actor;
  static const unsigned offsets[] = { 0x270, 0x50, 0x20, 0x10, 8 };
  for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); i++)
    p = next(p, offsets[i]);
  uintptr_t body = next(p, 0x20);
  if (!writable(p, 0x60) || !writable(body, 0x320) ||
      sys_ptr(p) != MAIN(Z64HD_VT_CCT_PROPERTY) || sys_ptr(body) != MAIN(Z64HD_VT_CCT_BODY))
    return false;
  /* Position setting visits the controller's attached-body lists. Do not call it
   * during partial construction/teardown or with an unreadable list. */
  uintptr_t lists = next(p, 0x58);
  if (!writable(lists, 0x60))
    return false;
  for (unsigned off = 0x10; off <= 0x50; off += 0x10) {
    uint32_t count;
    if (!sys_read32(lists + off, &count) || count > 128)
      return false;
    uintptr_t entries = next(lists, off + 8);
    if (count && (!entries || !sys_mapped(entries, count * sizeof(uintptr_t), SYS_PERM_R)))
      return false;
    for (unsigned i = 0; i < count; i++)
      if (!writable(sys_ptr(entries + i * sizeof(uintptr_t)), off == 0x10 ? 0x320 : 0x38))
        return false;
  }
  uintptr_t pending = next(body, 0x60);
  if (pending && !writable(pending, 0x108))
    return false;
  *out = (controller_t){ actor, p, body };
  return true;
}

/* The object just before the property in the controller chain holds the body's ID; a body entry's
 * handle is that ID under a fixed tag. */
#define BODY_ID 0x160
#define BODY_HANDLE_TAG 0x5F7F3DCDull

uint64_t cct_body_handle(void)
{
  if (!z64hd_game || !Z64HD_VT_CCT_PROPERTY)
    return 0;
  uintptr_t p = link_actor();
  static const unsigned offsets[] = { 0x270, 0x50, 0x20, 0x10 };
  for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); i++)
    p = next(p, offsets[i]);
  uint32_t id;
  if (!p || sys_ptr(next(p, 8)) != MAIN(Z64HD_VT_CCT_PROPERTY) || !sys_read32(p + BODY_ID, &id) || !id)
    return 0;
  return BODY_HANDLE_TAG << 32 | id;
}

static bool same(controller_t a, controller_t b)
{
  return a.actor == b.actor && a.property == b.property && a.body == b.body;
}

static bool no_hit(uintptr_t body)
{
  /* The setter checks the queued flag first: an enable/disable may not have
   * reached the physics thread yet. +0x100 bit 19 makes +0xF8 bit 0 authoritative. */
  uintptr_t pending = next(body, 0x60);
  if (pending && (*(volatile uint64_t *)(pending + 0x100) & (1ull << 19)))
    return (*(volatile uint32_t *)(pending + 0xF8) & 1) != 0;
  return (*(volatile uint64_t *)(body + 0x70) & (1ull << 11)) != 0;
}

static void set_no_hit(uintptr_t body, bool enabled)
{
  MAIN_FN(void (*)(uintptr_t, bool), Z64HD_CCT_FORCE_NO_HIT)(body, enabled);
}

bool cct_reset_vertical_velocity(uintptr_t expected_actor)
{
  controller_t current;
  if (!expected_actor || !resolve(&current) || current.actor != expected_actor)
    return false;
  /* The game's vertical-gravity reducer reads this same applied velocity and
   * uses this setter (main+0x278AA90). The setter locks and queues the change;
   * writing the scanned Havok record leaves this velocity at terminal speed. */
  vec3f_t v = *(volatile vec3f_t *)(current.body + 0xE0);
  if (!(v.x > -10000 && v.x < 10000 && v.z > -10000 && v.z < 10000))
    return false;
  v.y = 0;
  MAIN_FN(void (*)(uintptr_t, const vec3f_t *), Z64HD_CCT_SET_VELOCITY)(current.body, &v);
  return true;
}

bool cct_on_ground(uintptr_t expected_actor, bool *grounded)
{
  controller_t current;
  if (!expected_actor || !resolve(&current) || current.actor != expected_actor)
    return false;
  /* QueryPhysicsCctIsOnGround (main+0x2AABA20): current result in a
   * three-slot buffer, state 0 means supported. Never infer landing from a
   * stationary position: pauses and midair hover are stationary too. */
  uintptr_t results = next(current.property, 0x48);
  uint32_t slot, state;
  if (!results || !sys_read32(results + 0x58, &slot) || slot >= 3)
    return false;
  uintptr_t result = next(results, 0x40 + slot * 8);
  if (!result || !sys_read32(result + 0x10, &state))
    return false;
  *grounded = state == 0;
  return true;
}

bool cct_clear_fall_distance(uintptr_t expected_actor)
{
  uintptr_t actor = link_actor();
  if (!expected_actor || actor != expected_actor)
    return false;
  uintptr_t components = next(actor, 0x270);
  uintptr_t player = next(components, 0x358), base = next(components, 0x2B8);
  if (!writable(player, 0x1910) || !writable(base, 0x18A0) ||
      next(player, 0x18) != actor || next(base, 0x1340) != actor ||
      !sys_mapped(actor + 0x320, 4, SYS_PERM_R))
    return false;
  float y = *(volatile float *)(actor + 0x320);
  if (!(y > -100000 && y < 100000))
    return false;
  /* main+0x5FCF6C computes player+0x190C from +0x18D4 minus actor Y;
   * +0x5FD128 maintains the peak at +0x18E0. +0x5FD19C feeds the base
   * fall origin, whose distance is computed at +0x484A10. Reset history,
   * never the actor's position, gravity, health, or JS Height (+0x18E8). */
  *(volatile float *)(player + 0x18D4) = y;
  *(volatile float *)(player + 0x18E0) = y;
  *(volatile float *)(player + 0x190C) = 0;
  *(volatile float *)(base + 0x1898) = y;
  *(volatile float *)(base + 0x189C) = 0;
  return true;
}

bool cct_noclip_active(void) { return owner.body != 0; }

static unsigned attached_bodies(uintptr_t property, uintptr_t *out, unsigned max)
{
  uintptr_t lists = next(property, 0x58);
  uint32_t count;
  if (!lists || !sys_read32(lists + 0x10, &count))
    return 0;
  uintptr_t entries = next(lists, 0x18);
  unsigned n = 0;
  for (unsigned i = 0; i < count && n < max; i++)
    out[n++] = sys_ptr(entries + i * sizeof(uintptr_t));
  return n;
}

static bool body_switched(controller_t current)
{
  return owner.body && current.actor == owner.actor && current.property == owner.property &&
         current.body != owner.body;
}

static void follow_body(controller_t current)
{
  log_printf("levitate: controller body %#lx -> %#lx (another stance), collision bypass follows",
             (unsigned long)owner.body, (unsigned long)current.body);
  owner = current;
  if (!no_hit(owner.body))
    set_no_hit(owner.body, true);
}

bool cct_noclip_begin(void)
{
  controller_t current;
  if (!resolve(&current))
    return false;
  if (owner.body) {
    if (body_switched(current))
      follow_body(current);
    return same(owner, current);
  }
  uintptr_t bodies[MAX_BODIES];
  n_saved = attached_bodies(current.property, bodies, MAX_BODIES);
  bool original = no_hit(current.body);
  for (unsigned i = 0; i < n_saved; i++)
    saved[i].body = bodies[i], saved[i].no_hit = no_hit(bodies[i]);
  owner = current;
  restore_wait_logged = false;
  if (!original)
    set_no_hit(owner.body, true);
  log_printf("levitate: controller %#lx, body %#lx of %u, collision bypass on (was %d)",
             (unsigned long)owner.property, (unsigned long)owner.body, n_saved, original);
  return true;
}

bool cct_noclip_move(const vec3f_t *position)
{
  controller_t current;
  if (!owner.body || !resolve(&current))
    return false;
  if (body_switched(current))
    follow_body(current);
  if (!same(owner, current) ||
      !(position->x > -100000 && position->x < 100000 &&
        position->y > -100000 && position->y < 100000 &&
        position->z > -100000 && position->z < 100000))
    return false;
  if (!no_hit(owner.body))
    set_no_hit(owner.body, true);
  MAIN_FN(void (*)(uintptr_t, const vec3f_t *), Z64HD_CCT_SET_POSITION)(owner.property, position);
  return true;
}

void cct_noclip_end(void)
{
  if (!owner.body)
    return;
  controller_t current;
  if (!resolve(&current)) {
    if (!restore_wait_logged)
      log_printf("levitate: controller unavailable; collision restore pending");
    restore_wait_logged = true;
    return; /* Retry while idle; never forget a temporarily unreadable owner. */
  }
  if (current.actor == owner.actor && current.property == owner.property) {
    /* Two passes: restoring one body's mode can carry over to another's. */
    uintptr_t bodies[MAX_BODIES];
    unsigned n = attached_bodies(current.property, bodies, MAX_BODIES), restored = 0;
    for (int pass = 0; pass < 2; pass++)
      for (unsigned i = 0; i < n_saved; i++)
        for (unsigned j = 0; j < n; j++)
          if (bodies[j] == saved[i].body && no_hit(saved[i].body) != saved[i].no_hit) {
            set_no_hit(saved[i].body, saved[i].no_hit);
            restored++;
          }
    log_printf("levitate: collision bypass off, %u bod%s restored", restored, restored == 1 ? "y" : "ies");
  } else {
    log_printf("levitate: controller changed/unavailable; skipped stale collision restore");
  }
  owner = (controller_t){0};
  n_saved = 0;
}
