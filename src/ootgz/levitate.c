/* Levitate and 2x speed: both move Link through his physics record.
 *
 * Link's position lives in a physics record: doubles (x, y, z) 0.585 above his drawn position,
 * followed by motion state the game ignores the position without (FINDINGS §5.9-§5.15). game.py found
 * the record by searching the heap and test-moving the look-alikes (moving the wrong one once broke
 * the camera). Here the search starts from Link's actor instead: follow its pointers a few levels
 * deep, a few hundred objects per frame, collect records that match his position, then test-lift
 * each (kept only if his drawn position follows; put back at once otherwise). The pointer path to the
 * one that works is logged, so a later version can go straight to it.
 *
 * Levitate (as ootmenu, game.py _float, once per frame): L3 starts floating, L3 again drops him, R3
 * held keeps the height, the left stick moves him relative to the camera. Walls: the game pushes
 * Link back out of them between our writes; a push-back is a move against his heading (after he has
 * held it for a moment, or the momentum of a turn looks like one), and it marks the wall for a
 * moment so we stop pushing into it (FINDINGS §5.10). "through walls" skips all of that.
 *
 * 2x speed: while Link walks or runs on the stick, add the distance the game moved him this frame
 * again (the game's collision still pushes him back out of walls). */
#include "levitate.h"

#include "cheats.h"
#include "gfx.h"
#include "input.h"
#include "link.h"
#include "sys.h"
#include "z64hd.h"

float hypotf(float x, float y); /* from sdk */

bool levitate_enabled;
bool levitate_through_walls = true;
int levitate_speed = 4;       /* rise, units/s */
int levitate_move_speed = 6;  /* stick, units/s */

#define PHYS_Y 0.585
#define MATCH 0.3              /* per axis */
#define TEST_LIFT 0.6
#define TEST_FRAMES 6
#define RETRY_FRAMES 150        /* after a failed search */
#define HEAL_FRAMES 90          /* after dropping: give back health lost landing */
#define RISE_RAMP 0.35          /* s to reach the rise speed (game.py LEV_RISE_RAMP) */
#define STOP_RAMP 0.08
#define MOVE_RAMP 0.15
#define PUSHBACK_MIN 0.01       /* units: a push-back this big marks a wall */
#define WALL_MEMORY 0.15        /* s a wall stays marked */
#define STEADY_TIME 0.1         /* s Link must keep his heading before a push-back counts */
#define FROZEN_FRAMES 3         /* frames with neither the record nor the drawn position moving */
#define STICK_DEAD 7849

/* motion state captured from Link mid-run (game.py WAKE) */
static const uint8_t wake[48] = {
  0x00, 0x00, 0x00, 0x00, 0x25, 0x67, 0x15, 0x41, 0x00, 0x00, 0x00, 0x00, 0xce, 0xcf, 0x8d, 0x40,
  0xfb, 0xf9, 0x9f, 0x3e, 0x26, 0x3c, 0xa1, 0xbf, 0x00, 0x00, 0x00, 0x00, 0x24, 0x67, 0x15, 0x41,
  0x00, 0x00, 0x00, 0x00, 0xcc, 0xcf, 0x8d, 0x40, 0xfc, 0xf9, 0x9f, 0x3e, 0x24, 0x3c, 0xa1, 0xbf,
};

static enum { L_IDLE, L_FINDING, L_TESTING, L_READY, L_FAILED } state;
static uintptr_t record;
/* the 48 bytes after the position are game.py's motion state (floats); in the record this search
 * finds they're more position doubles, and writing motion state over them crashed the game */
static bool record_motion;
static bool quiet_search; /* started by the speed cheat: no notices */
static int fail_wait, heal_frames;
static int32_t heal_to;
static uint64_t last_tick;

static double absd(double v) { return v < 0 ? -v : v; }

/* the record API (levitate.h): a search asked for by position.c, and frames in which a move it made
 * may still be settling (the record ahead of the drawn position isn't "lost") */
static bool requested;
static int hold_frames;

/* --- finding the record: a heap scan, nearest Link's actor first -----------------------------------
 * (game.py's method. A pointer search from the actor only reached copies the game rewrites from the
 * real record every frame: FINDINGS §5.19.) Each frame scans SCAN_PER_FRAME bytes, so the game keeps
 * running; each match is test-lifted as soon as it's found, and the scan carries on if it isn't his. */

#define MAX_REGIONS 16384
#define SCAN_PER_FRAME (16u << 20)
#define MAX_TESTS_PER_SEARCH 6
static struct { uintptr_t lo, hi; } heap[MAX_REGIONS];
static int n_heap, scan_r, n_tested;
static uintptr_t scan_p, scan_origin, last_found;
static size_t scanned;

typedef struct { uintptr_t addr; double err; } cand_t;
static cand_t cand;
static uint8_t saved_bytes[24 + 48];
static bool test_wake;
static int test_frames;
static vec3f_t search_pos, test_from;

static void add_heap(uintptr_t base, size_t size, void *ctx)
{
  (void)ctx;
  sys_meminfo_t m;
  if (n_heap < MAX_REGIONS && sys_query_memory(&m, base) == 0 && (m.type & 0xFF) == 0x05 /* Normal */)
    heap[n_heap].lo = base, heap[n_heap].hi = base + size, n_heap++;
}

static uintptr_t distance(int i)
{
  uintptr_t lo = heap[i].lo, hi = heap[i].hi, o = scan_origin;
  return o < lo ? lo - o : o >= hi ? o - hi : 0;
}

/* nearest the actor first (shell sort: there can be thousands) */
static void sort_heap(void)
{
  for (int gap = n_heap / 2; gap > 0; gap /= 2)
    for (int i = gap; i < n_heap; i++)
      for (int j = i; j >= gap && distance(j - gap) > distance(j); j -= gap) {
        __typeof__(heap[0]) t = heap[j];
        heap[j] = heap[j - gap], heap[j - gap] = t;
      }
}

/* The 48 bytes after Link's position are motion state: all zero while he stands, otherwise floats
 * of modest size (velocities). Anything else (pointers, garbage) means this isn't his record, and
 * writing there is what crashed the game: game.py's warning about look-alikes, FINDINGS §5.9. */
static bool plausible_motion(uintptr_t m, bool *zero)
{
  *zero = true;
  for (int i = 0; i < 12; i++) {
    uint32_t u = ((volatile uint32_t *)m)[i];
    if (!u)
      continue;
    *zero = false;
    float f;
    memcpy(&f, &u, 4);
    if (!(f == f) || f > 10000.0f || f < -10000.0f || (u & 0x7F800000) == 0) /* NaN, huge, denormal */
      return false;
  }
  return true;
}

/* Another x, y, z (as doubles, near the first) right after the position: one of the copies, not the
 * record (FINDINGS §5.19). */
static bool doubles_follow(uintptr_t a)
{
  const volatile double *d = (const volatile double *)a;
  return absd(d[3] - d[0]) < 10 && absd(d[4] - d[1]) < 10 && absd(d[5] - d[2]) < 10;
}

static void search_start(bool quiet)
{
  uintptr_t link = link_actor();
  if (!link || !link_drawn_pos(&search_pos))
    return;
  n_heap = 0;
  sys_regions(SYS_PERM_R | SYS_PERM_W, add_heap, 0);
  /* records are made again in each area, near where they were (game.py POS_NEAR) */
  scan_origin = last_found ? last_found : link;
  sort_heap();
  scan_r = 0, scan_p = n_heap ? heap[0].lo : 0, scanned = 0, n_tested = 0;
  quiet_search = quiet;
  state = L_FINDING;
  log_printf("levitate: scanning %d heap regions for Link's record, nearest %#lx first", n_heap,
             (unsigned long)scan_origin);
}

/* Scan on until a match (returns true, in `cand`) or this frame's budget is used. */
static bool scan_step(void)
{
  size_t budget = SCAN_PER_FRAME;
  link_drawn_pos(&search_pos); /* where Link is now (he may be moving) */
  while (scan_r < n_heap && budget) {
    uintptr_t end = heap[scan_r].hi - 72;
    uintptr_t stop = scan_p + budget < end ? scan_p + budget : end;
    for (uintptr_t p = scan_p; p < stop; p += 8) {
      double x = *(volatile double *)p;
      if (absd(x - search_pos.x) >= MATCH)
        continue;
      double y = *(volatile double *)(p + 8), z = *(volatile double *)(p + 16);
      bool zero;
      if (absd(y - search_pos.y - PHYS_Y) < MATCH && absd(z - search_pos.z) < MATCH && !doubles_follow(p) &&
          plausible_motion(p + 24, &zero)) {
        cand.addr = p;
        cand.err = absd(x - search_pos.x) + absd(y - search_pos.y - PHYS_Y) + absd(z - search_pos.z);
        scanned += p + 8 - scan_p;
        scan_p = p + 8;
        return true;
      }
    }
    budget -= stop - scan_p;
    scanned += stop - scan_p;
    if (stop >= end) {
      if (++scan_r < n_heap)
        scan_p = heap[scan_r].lo;
    } else {
      scan_p = stop;
    }
  }
  return false;
}

/* Position, then the motion state with our velocity: requested velocity (z, x, y) at record +0x24
 * and the same after collision at +0x3C (FINDINGS §5.10); the rest as captured (game.py WAKE). */
static void write_record(uintptr_t a, double x, double y, double z, float vx, float vy, float vz, bool motion)
{
  volatile double *d = (volatile double *)a;
  d[0] = x, d[1] = y, d[2] = z;
  if (!motion)
    return;
  /* The game turns Link to face his velocity; a zero velocity has no direction and set him
   * spinning. Not moving: a tiny one along the way he already faces. */
  double fx, fz;
  if (vx * vx + vz * vz < 0.05 * 0.05 && link_facing(&fx, &fz))
    vx = (float)(fx * 0.01), vz = (float)(fz * 0.01);
  uint8_t m[sizeof(wake)];
  memcpy(m, wake, sizeof(m));
  const float v[3] = { vz, vx, vy };
  memcpy(m + (0x24 - 0x18), v, sizeof(v));
  memcpy(m + (0x3C - 0x18), v, sizeof(v));
  memcpy((void *)(a + 24), m, sizeof(m));
}

static void search_failed(void)
{
  requested = false; /* position.c sees L_FAILED and says so */
  if (!quiet_search)
    gfx_notice("levitate: Link's position not found, try again");
  state = L_FAILED;
  fail_wait = quiet_search ? RETRY_FRAMES * 2 : RETRY_FRAMES;
}

/* Lift the height only; the motion state (which the game needs to take the position while Link
 * stands) is written only where it's all zero, never over other data. */
static void test_start(void)
{
  uintptr_t a = cand.addr;
  bool zero;
  memcpy(saved_bytes, (void *)a, sizeof(saved_bytes));
  plausible_motion(a + 24, &zero);
  link_drawn_pos(&test_from);
  const volatile uint32_t *m = (const volatile uint32_t *)(a + 24);
  log_printf("levitate: testing %#lx (off by %.3f, motion %s: %08x %08x %08x %08x)", (unsigned long)a, cand.err,
             zero ? "zero" : "set", m[0], m[1], m[2], m[3]);
  test_wake = zero;
  write_record(a, ((volatile double *)a)[0], ((volatile double *)a)[1] + TEST_LIFT, ((volatile double *)a)[2], 0, 0, 0,
               test_wake);
  test_frames = 0;
  n_tested++;
  state = L_TESTING;
}

static void test_step(void)
{
  vec3f_t now;
  if (link_drawn_pos(&now) && now.y - test_from.y > TEST_LIFT / 2) {
    record = cand.addr;
    record_motion = true;
    last_found = record;
    log_printf("levitate: Link's physics record is %#lx (moved him; scanned %u MiB)", (unsigned long)record,
               (unsigned)(scanned >> 20));
    state = L_READY;
    return;
  }
  if (++test_frames < TEST_FRAMES)
    return;
  uintptr_t a = cand.addr; /* not Link's: put it back unless the game has rewritten it */
  double was_y;
  memcpy(&was_y, saved_bytes + 8, 8);
  if (absd(((volatile double *)a)[1] - (was_y + TEST_LIFT)) < 1e-9)
    memcpy((void *)a, saved_bytes, test_wake ? sizeof(saved_bytes) : 24);
  if (n_tested >= MAX_TESTS_PER_SEARCH) {
    log_printf("levitate: %d matches didn't move Link", n_tested);
    search_failed();
    return;
  }
  state = L_FINDING; /* carry on scanning */
}

static bool record_follows_link(void)
{
  vec3f_t d;
  if (!record || !link_drawn_pos(&d) || !sys_mapped(record, 72, SYS_PERM_R | SYS_PERM_W))
    return false;
  volatile double *r = (volatile double *)record;
  return absd(r[0] - d.x) < 3 && absd(r[2] - d.z) < 3 && absd(r[1] - d.y - PHYS_Y) < 3;
}

/* --- shared helpers ----------------------------------------------------------------------------- */

static double approach(double v, double target, double step)
{
  return v < target ? (v + step < target ? v + step : target) : (v - step > target ? v - step : target);
}

static bool same_way(double ax, double az, double bx, double bz)
{
  double n = (double)hypotf((float)ax, (float)az) * hypotf((float)bx, (float)bz);
  return n > 0 && (ax * bx + az * bz) / n > 0.7;
}

/* left stick, -1..1, dead zone applied */
static void stick(double *sx, double *sy)
{
  int32_t lx, ly, rx, ry;
  input_sticks(&lx, &ly, &rx, &ry);
  double m = hypotf((float)lx, (float)ly);
  if (m < STICK_DEAD) {
    *sx = *sy = 0;
    return;
  }
  double k = (m - STICK_DEAD) / (32767.0 - STICK_DEAD) / m;
  if (k * m > 1)
    k = 1 / m;
  *sx = lx * k, *sy = ly * k;
}

static bool camera_forward(double *fx, double *fz)
{
  uintptr_t c = camera();
  if (!c || !sys_mapped(c + 0x38, 24, SYS_PERM_R))
    return false;
  float *p = (float *)(c + 0x38), *l = (float *)(c + 0x44);
  double dx = l[0] - p[0], dz = l[2] - p[2], n = hypotf((float)dx, (float)dz);
  if (n < 1e-3)
    return false;
  *fx = dx / n, *fz = dz / n;
  return true;
}

/* --- levitate --------------------------------------------------------------------------------- */

static bool floating, writing; /* floating: L3 turned it on; writing: we're moving Link */
static double px, py, pz, vx, vy, vz;               /* our position and velocity */
static double head_x, head_z;                       /* last direction he moved in */
static double wall_x, wall_z, wall_until;           /* a wall last pushed him this way, until */
static double refs[2][2], steady_at;                /* headings a moment ago */
static double clock;                                /* s of gameplay since the float started */
static double last_r[3];
static vec3f_t last_drawn;
static int frozen;
static int trace; /* frames of float logged (diagnostics) */

static void drop(void)
{
  if (!floating)
    return;
  floating = false;
  if (writing) {
    writing = false;
    int32_t *cur, *max;
    if (link_health(&cur, &max))
      heal_to = *cur, heal_frames = HEAL_FRAMES;
  }
}

static void float_start(void)
{
  volatile double *r = (volatile double *)record;
  px = r[0], py = r[1], pz = r[2];
  vx = vy = vz = head_x = head_z = wall_x = wall_z = 0;
  wall_until = steady_at = clock = 0;
  memset(refs, 0, sizeof(refs));
  frozen = 0;
  link_drawn_pos(&last_drawn);
  last_r[0] = px, last_r[1] = py, last_r[2] = pz;
  writing = true;
  trace = 0;
}

static void float_step(double dt, bool menu_open)
{
  volatile double *r = (volatile double *)record;
  double cx = r[0], cy = r[1], cz = r[2];
  vec3f_t drawn;
  link_drawn_pos(&drawn);
  /* Gameplay frozen (pause screen, item menus; the frame counter keeps going there): neither the
   * record has moved off what we wrote nor Link been redrawn (game.py LEV_PAUSED_AFTER). */
  bool moved = cx != last_r[0] || cy != last_r[1] || cz != last_r[2] || drawn.x != last_drawn.x ||
               drawn.y != last_drawn.y || drawn.z != last_drawn.z;
  frozen = moved ? 0 : frozen + 1;
  last_drawn = drawn;
  if (trace < 40) { /* what the game did with our last write */
    const volatile double *n = (const volatile double *)(record + 24);
    double fx = 0, fz = 0;
    link_facing(&fx, &fz);
    (void)n;
    log_printf("levitate f%d: record y %.3f (we wrote %.3f) x %.3f z %.3f | drawn y %.3f | facing %.3f,%.3f | "
               "frozen %d clock %.2f", trace, cy, last_r[1], cx, cz, (double)drawn.y, fx, fz, frozen, clock);
    trace++;
  }
  if ((frozen >= FROZEN_FRAMES && clock > 0.3) || menu_open) {
    vx = vy = vz = 0;
  } else {
    clock += dt;
    bool hold = input_held() & BTN_RSTICK;
    double rise = levitate_speed, move = levitate_move_speed;
    vy = approach(vy, hold ? 0 : rise, rise / (hold ? STOP_RAMP : RISE_RAMP) * dt);
    double sx, sy, fx, fz, tx = 0, tz = 0;
    stick(&sx, &sy);
    if ((sx || sy) && camera_forward(&fx, &fz)) {
      double rx = -fz, rz = fx; /* camera's right */
      tx = (fx * sy + rx * sx) * move, tz = (fz * sy + rz * sx) * move;
    }
    double step = move / MOVE_RAMP * dt;
    vx = approach(vx, tx, step), vz = approach(vz, tz, step);
    py += vy * dt;
  }
  if (!levitate_through_walls) {
    double gx = cx - px, gz = cz - pz; /* how the game moved him since our last write */
    bool wall = clock < wall_until;
    if (clock >= steady_at) {
      refs[1][0] = refs[0][0], refs[1][1] = refs[0][1];
      refs[0][0] = head_x, refs[0][1] = head_z;
      steady_at = clock + STEADY_TIME;
    }
    bool steady = same_way(head_x, head_z, refs[0][0], refs[0][1]) &&
                  same_way(head_x, head_z, refs[1][0], refs[1][1]);
    if (steady && gx * head_x + gz * head_z < 0 && !(wall && gx * wall_x + gz * wall_z < 0)) {
      px = cx, pz = cz; /* the game pushed him out of a wall: keep that */
      double m = hypotf((float)gx, (float)gz);
      if (m > PUSHBACK_MIN)
        wall_x = gx / m, wall_z = gz / m, wall_until = clock + WALL_MEMORY, wall = true;
    }
    if (wall) {
      double into = vx * wall_x + vz * wall_z;
      if (into < 0)
        vx -= into * wall_x, vz -= into * wall_z;
    }
  }
  px += vx * dt, pz += vz * dt;
  if (hypotf((float)vx, (float)vz) > 0.05)
    head_x = vx, head_z = vz;
  /* Position every frame; the motion state only when the game has cleared it (it ignores the
   * position then), as game.py did. Rewriting all of it every frame kept Link spinning. */
  bool cleared = true;
  for (int i = 0; i < 12; i++)
    cleared &= ((volatile uint32_t *)(record + 24))[i] == 0;
  write_record(record, px, py, pz, (float)vx, (float)vy, (float)vz, record_motion && cleared);
  last_r[0] = px, last_r[1] = py, last_r[2] = pz;
}

/* --- 2x speed ------------------------------------------------------------------------------------ */

static bool speed_known;
static double speed_x, speed_z; /* where we put him last frame */
static int speed_trace;

static void speed_step(void)
{
  volatile double *r = (volatile double *)record;
  double sx, sy;
  stick(&sx, &sy);
  if (!(sx || sy)) {
    speed_known = false;
    return;
  }
  double cx = r[0], cz = r[2];
  if (speed_known) {
    double dx = cx - speed_x, dz = cz - speed_z; /* how far the game moved him this frame */
    if (speed_trace < 12)
      log_printf("speed f%d: game moved him %.3f, %.3f (record %.3f, %.3f; we put him %.3f, %.3f)", speed_trace++, dx,
                 dz, cx, cz, speed_x, speed_z);
    if (dx * dx + dz * dz < 4.0) {             /* not a warp or a respawn */
      r[0] = cx + dx, r[2] = cz + dz;
      cx += dx, cz += dz;
    }
  }
  speed_x = cx, speed_z = cz, speed_known = true;
}

/* --- Link's record, for position.c ------------------------------------------------------------- */

link_record_status_t link_record_status(void)
{
  switch (state) {
    case L_READY: return REC_READY;
    case L_FINDING: case L_TESTING: return REC_SEARCHING;
    case L_FAILED: return REC_FAILED;
    default: return requested ? REC_SEARCHING : REC_NONE;
  }
}

void link_record_request(void)
{
  if (state == L_READY)
    return;
  if (state == L_FAILED) /* asked again: search again now */
    state = L_IDLE;
  requested = true;
}

/* Position, plus the motion state (record_motion: the test that found it wrote there safely): the game
 * ignores a new position while Link stands unless that's written too, and standing leaves small
 * leftovers in it, so it's rarely all zero (game.py's move wrote it every time too). */
bool link_record_move(double x, double y, double z, float face_vx, float face_vz, int settle_frames)
{
  if (state != L_READY || !sys_mapped(record, 72, SYS_PERM_R | SYS_PERM_W))
    return false;
  write_record(record, x, y + PHYS_Y, z, face_vx, 0, face_vz, record_motion);
  if (writing) /* floating: carry on from there */
    px = x, py = y + PHYS_Y, pz = z, vx = vy = vz = 0, last_r[0] = px, last_r[1] = py, last_r[2] = pz;
  speed_known = false;
  if (settle_frames > hold_frames)
    hold_frames = settle_frames;
  return true;
}

void link_record_resync(void)
{
  vec3f_t d;
  if (state != L_READY || !link_drawn_pos(&d) || !sys_mapped(record, 24, SYS_PERM_W))
    return;
  volatile double *r = (volatile double *)record;
  r[0] = d.x, r[1] = d.y + PHYS_Y, r[2] = d.z;
  if (writing)
    px = d.x, py = d.y + PHYS_Y, pz = d.z, last_r[0] = px, last_r[1] = py, last_r[2] = pz;
  hold_frames = 0;
}

/* --- frame --------------------------------------------------------------------------------------- */

void levitate_frame(bool menu_open)
{
  input_hide_from_game = (levitate_enabled ? BTN_LSTICK : 0) | (floating ? BTN_RSTICK : 0);
  uint64_t now = sys_tick();
  double dt = last_tick ? (double)(now - last_tick) / (double)sys_tick_freq() : 0;
  last_tick = now;
  if (dt > 0.1)
    dt = 0.1;

  if (heal_frames > 0) {
    heal_frames--;
    int32_t *cur, *max;
    if (link_health(&cur, &max) && *cur < heal_to)
      *cur = heal_to;
  }

  /* L3: start / stop */
  if (!levitate_enabled)
    drop();
  else if (!menu_open && (input_pressed() & BTN_LSTICK)) {
    if (floating)
      drop(), gfx_notice("levitate off");
    else
      floating = true, quiet_search = false, gfx_notice("levitate on");
  }

  if (state == L_FINDING) {
    if (scan_step())
      test_start();
    else if (scan_r >= n_heap) {
      log_printf("levitate: no match in %u MiB of heap (%d tested)", (unsigned)(scanned >> 20), n_tested);
      search_failed();
    }
    if (state == L_FAILED)
      floating = false;
    return;
  }
  if (state == L_TESTING) {
    test_step();
    if (state == L_FAILED)
      floating = false;
    return;
  }
  if (state == L_FAILED && --fail_wait <= 0)
    state = L_IDLE;

  if (hold_frames > 0)
    hold_frames--;
  if (state == L_READY && !hold_frames && !record_follows_link() && !(writing && frozen)) {
    log_printf("levitate: lost Link's record (new area?)");
    record = 0;
    state = L_IDLE;
    writing = floating = speed_known = false;
  }

  bool want_speed = cheats[CHEAT_SPEED] && !floating && !menu_open;
  if (state == L_READY)
    requested = false;
  if (state == L_IDLE && (floating || want_speed || requested)) {
    search_start(!floating);
    return;
  }
  if (state != L_READY) {
    speed_known = false;
    return;
  }
  if (floating) {
    if (!writing)
      float_start();
    float_step(dt, menu_open);
    speed_known = false;
  } else if (want_speed) {
    speed_step();
  } else {
    speed_known = false;
  }
}

const char *levitate_state(void)
{
  switch (state) {
    case L_FINDING: case L_TESTING: return "finding Link";
    case L_FAILED: return "not found";
    default: return writing ? "floating" : floating ? "starting" : record ? "ready" : "off";
  }
}
