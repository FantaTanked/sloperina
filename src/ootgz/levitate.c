/* Levitate and 2x speed: both move Link through his physics record.
 *
 * Link's position lives in a physics record: doubles (x, y, z) 0.585 above his drawn position,
 * followed by motion state without which the game ignores the position (FINDINGS §5.9-§5.15).
 * game.py found the record by searching the heap and test-moving the look-alikes (moving the wrong one
 * once broke the camera). Here the heap is scanned nearest Link's actor first, a slice per frame;
 * each record that matches his position is test-lifted (kept only if his drawn position follows, put
 * back at once otherwise). The address that works is logged.
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

/* the physics record: position (3 doubles), then motion state (12 words) */
#define RECORD_POS_SIZE 24
#define MOTION_SIZE 48
#define MOTION_WORDS 12
#define RECORD_SIZE (RECORD_POS_SIZE + MOTION_SIZE)
#define RECORD_REQUESTED_VEL 0x24 /* requested velocity (z, x, y floats) */
#define RECORD_COLLIDED_VEL 0x3C  /* the same after collision */

#define PHYS_Y 0.585            /* the record's height above Link's drawn position */
#define MATCH 0.3               /* per axis: how close a record must be to Link's position */
#define FOLLOW_TOLERANCE 3      /* per axis: the found record still belongs to Link */
#define COPY_TOLERANCE 10       /* per axis: a second position this close after the first is a copy */
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
#define SAME_WAY_MIN 0.7        /* cosine of the angle within which two directions are the same way */
#define FROZEN_FRAMES 3         /* frames with neither the record nor the drawn position moving */
#define FROZEN_AFTER 0.3        /* s of floating before a freeze can be told from the float starting */
#define STILL_SPEED 0.05        /* below this Link isn't moving (and keeps his heading) */
#define TINY_VELOCITY 0.01      /* the velocity written when he isn't moving, so he keeps facing */
#define SPEED_MAX_STEP_SQ 4.0   /* 2x speed: a bigger move (squared) is a warp or a respawn */
#define MAX_DT 0.1              /* s: longer frames count as this */
#define STICK_DEAD 7849
#define STICK_MAX 32767.0
#define FLOAT_TRACE_FRAMES 40   /* frames of each float that are logged */
#define SPEED_TRACE_FRAMES 12   /* frames of 2x speed that are logged (once) */

#define MEMORY_TYPE_MASK 0xFF
#define MEMORY_TYPE_NORMAL 0x05 /* ordinary heap memory */

#define FLOAT_EXPONENT_MASK 0x7F800000u
#define MOTION_FLOAT_MAX 10000.0f

/* Link's actor: his facing as cos and sin (0 = +z) */
#define LINK_FACING_COS 0x328
#define LINK_FACING_SIN 0x330

/* the game's camera */
#define CAM_POSITION 0x38
#define CAM_LOOK_AT 0x44

/* motion state captured from Link mid-run (game.py WAKE) */
static const uint8_t captured_motion[MOTION_SIZE] = {
  0x00, 0x00, 0x00, 0x00, 0x25, 0x67, 0x15, 0x41, 0x00, 0x00, 0x00, 0x00, 0xce, 0xcf, 0x8d, 0x40,
  0xfb, 0xf9, 0x9f, 0x3e, 0x26, 0x3c, 0xa1, 0xbf, 0x00, 0x00, 0x00, 0x00, 0x24, 0x67, 0x15, 0x41,
  0x00, 0x00, 0x00, 0x00, 0xcc, 0xcf, 0x8d, 0x40, 0xfc, 0xf9, 0x9f, 0x3e, 0x24, 0x3c, 0xa1, 0xbf,
};

static enum {
  SEARCH_IDLE,
  SEARCH_FINDING, /* scanning the heap */
  SEARCH_TESTING, /* test-lifting a match */
  SEARCH_READY,   /* `record` is Link's */
  SEARCH_FAILED,  /* waiting before searching again */
} search_state;
static uintptr_t record;
/* the 48 bytes after the position are game.py's motion state (floats); in the record this search
 * finds they're more position doubles, and writing motion state over them crashed the game */
static bool record_has_motion;
static bool quiet_search; /* started by the speed cheat: no notices */
/* the record API (levitate.h): a search asked for by position.c, and frames in which a move it made
 * may still be settling (the record ahead of the drawn position isn't "lost") */
static bool requested;
static int hold_frames;
static int retry_wait_frames;
static int heal_frames_left;
static int32_t heal_to;
static uint64_t last_tick;

static double abs_double(double v)
{
  return v < 0 ? -v : v;
}

/* --- finding the record: a heap scan, nearest Link's actor first -----------------------------------
 * (game.py's method. A pointer search from the actor only reached copies the game rewrites from the
 * real record every frame: FINDINGS §5.19.) Each frame scans SCAN_PER_FRAME bytes, so the game keeps
 * running; each match is test-lifted as soon as it's found, and the scan carries on if it isn't his. */

#define MAX_REGIONS 16384
#define SCAN_PER_FRAME (16u << 20)
#define MAX_TESTS_PER_SEARCH 6
static struct { uintptr_t lo, hi; } heap_regions[MAX_REGIONS];
static int n_heap_regions;
static int scan_region;   /* index into heap_regions */
static uintptr_t scan_pos; /* the next address to look at */
static int n_tested;
static uintptr_t scan_origin;
static uintptr_t last_found; /* where the record was last time: the next search starts there */
static size_t bytes_scanned;

typedef struct { uintptr_t addr; double error; } candidate_t;
static candidate_t candidate;
static uint8_t saved_record[RECORD_SIZE]; /* the candidate before the test lift */
static bool test_wrote_motion;
static int test_frames;
static vec3f_t search_pos;      /* Link's drawn position */
static vec3f_t test_start_pos;  /* Link's drawn position when the test lift started */

static void add_heap_region(uintptr_t base, size_t size, void *ctx)
{
  (void)ctx;
  sys_meminfo_t info;
  if (n_heap_regions < MAX_REGIONS && sys_query_memory(&info, base) == 0 &&
      (info.type & MEMORY_TYPE_MASK) == MEMORY_TYPE_NORMAL) {
    heap_regions[n_heap_regions].lo = base;
    heap_regions[n_heap_regions].hi = base + size;
    n_heap_regions++;
  }
}

static uintptr_t region_distance(int index)
{
  uintptr_t lo = heap_regions[index].lo;
  uintptr_t hi = heap_regions[index].hi;
  if (scan_origin < lo)
    return lo - scan_origin;
  if (scan_origin >= hi)
    return scan_origin - hi;
  return 0;
}

/* nearest the scan origin first (shell sort: there can be thousands) */
static void sort_regions(void)
{
  for (int gap = n_heap_regions / 2; gap > 0; gap /= 2)
    for (int i = gap; i < n_heap_regions; i++)
      for (int j = i; j >= gap && region_distance(j - gap) > region_distance(j); j -= gap) {
        __typeof__(heap_regions[0]) swap = heap_regions[j];
        heap_regions[j] = heap_regions[j - gap];
        heap_regions[j - gap] = swap;
      }
}

/* The 48 bytes after Link's position are motion state: all zero while he stands, otherwise floats
 * of modest size (velocities). Anything else (pointers, garbage) means this isn't his record, and
 * writing there is what crashed the game: game.py's warning about look-alikes, FINDINGS §5.9. */
static bool plausible_motion(uintptr_t motion, bool *all_zero)
{
  *all_zero = true;
  for (int i = 0; i < MOTION_WORDS; i++) {
    uint32_t word = ((volatile uint32_t *)motion)[i];
    if (!word)
      continue;
    *all_zero = false;
    float value;
    memcpy(&value, &word, 4);
    if (!(value == value) || value > MOTION_FLOAT_MAX || value < -MOTION_FLOAT_MAX ||
        (word & FLOAT_EXPONENT_MASK) == 0) /* NaN, huge, denormal */
      return false;
  }
  return true;
}

/* Another x, y, z (as doubles, near the first) right after the position: one of the copies, not the
 * record (FINDINGS §5.19). */
static bool followed_by_copy(uintptr_t addr)
{
  const volatile double *d = (const volatile double *)addr;
  return abs_double(d[3] - d[0]) < COPY_TOLERANCE && abs_double(d[4] - d[1]) < COPY_TOLERANCE &&
         abs_double(d[5] - d[2]) < COPY_TOLERANCE;
}

static void search_start(bool quiet)
{
  uintptr_t link = link_actor();
  if (!link || !link_drawn_pos(&search_pos))
    return;
  n_heap_regions = 0;
  sys_regions(SYS_PERM_R | SYS_PERM_W, add_heap_region, 0);
  /* records are made again in each area, near where they were (game.py POS_NEAR) */
  scan_origin = last_found ? last_found : link;
  sort_regions();
  scan_region = 0;
  scan_pos = n_heap_regions ? heap_regions[0].lo : 0;
  bytes_scanned = 0;
  n_tested = 0;
  quiet_search = quiet;
  search_state = SEARCH_FINDING;
  log_printf("levitate: scanning %d heap regions for Link's record, nearest %#lx first", n_heap_regions,
             (unsigned long)scan_origin);
}

/* Could the record at addr be Link's? If so, *error is how far it is from his position. */
static bool record_matches(uintptr_t addr, double *error)
{
  double x = *(volatile double *)addr;
  if (abs_double(x - search_pos.x) >= MATCH)
    return false;
  double y = *(volatile double *)(addr + 8);
  double z = *(volatile double *)(addr + 16);
  bool all_zero;
  if (abs_double(y - search_pos.y - PHYS_Y) < MATCH && abs_double(z - search_pos.z) < MATCH &&
      !followed_by_copy(addr) && plausible_motion(addr + RECORD_POS_SIZE, &all_zero)) {
    *error = abs_double(x - search_pos.x) + abs_double(y - search_pos.y - PHYS_Y) + abs_double(z - search_pos.z);
    return true;
  }
  return false;
}

/* Scan on until a match (returns true, in `candidate`) or this frame's budget is used. */
static bool scan_step(void)
{
  size_t budget = SCAN_PER_FRAME;
  link_drawn_pos(&search_pos); /* where Link is now (he may be moving) */
  while (scan_region < n_heap_regions && budget) {
    uintptr_t end = heap_regions[scan_region].hi - RECORD_SIZE;
    uintptr_t stop = scan_pos + budget < end ? scan_pos + budget : end;
    for (uintptr_t p = scan_pos; p < stop; p += 8) {
      double error;
      if (record_matches(p, &error)) {
        candidate.addr = p;
        candidate.error = error;
        bytes_scanned += p + 8 - scan_pos;
        scan_pos = p + 8;
        return true;
      }
    }
    budget -= stop - scan_pos;
    bytes_scanned += stop - scan_pos;
    if (stop >= end) {
      if (++scan_region < n_heap_regions)
        scan_pos = heap_regions[scan_region].lo;
    } else {
      scan_pos = stop;
    }
  }
  return false;
}

/* Position, then (if write_motion) the motion state with our velocity: requested velocity (z, x, y)
 * at record +0x24 and the same after collision at +0x3C (FINDINGS §5.10); the rest as captured
 * (game.py WAKE). */
static void write_record(uintptr_t addr, double x, double y, double z, float vx, float vy, float vz,
                         bool write_motion)
{
  volatile double *pos = (volatile double *)addr;
  pos[0] = x;
  pos[1] = y;
  pos[2] = z;
  if (!write_motion)
    return;
  /* The game turns Link to face his velocity; a zero velocity has no direction and set him
   * spinning. Not moving: a tiny one along the way he already faces. */
  double fx, fz;
  if (vx * vx + vz * vz < STILL_SPEED * STILL_SPEED && link_facing(&fx, &fz)) {
    vx = (float)(fx * TINY_VELOCITY);
    vz = (float)(fz * TINY_VELOCITY);
  }
  uint8_t motion[sizeof(captured_motion)];
  memcpy(motion, captured_motion, sizeof(motion));
  const float velocity[3] = { vz, vx, vy };
  memcpy(motion + (RECORD_REQUESTED_VEL - RECORD_POS_SIZE), velocity, sizeof(velocity));
  memcpy(motion + (RECORD_COLLIDED_VEL - RECORD_POS_SIZE), velocity, sizeof(velocity));
  memcpy((void *)(addr + RECORD_POS_SIZE), motion, sizeof(motion));
}

static void search_failed(void)
{
  requested = false; /* position.c sees REC_FAILED and says so */
  if (!quiet_search)
    gfx_notice("levitate: Link's position not found, try again");
  search_state = SEARCH_FAILED;
  retry_wait_frames = quiet_search ? RETRY_FRAMES * 2 : RETRY_FRAMES;
}

/* Lift the height only; the motion state (which the game needs to take the position while Link
 * stands) is written only where it's all zero, never over other data. */
static void test_start(void)
{
  uintptr_t addr = candidate.addr;
  bool motion_zero;
  memcpy(saved_record, (void *)addr, sizeof(saved_record));
  plausible_motion(addr + RECORD_POS_SIZE, &motion_zero);
  link_drawn_pos(&test_start_pos);
  const volatile uint32_t *motion = (const volatile uint32_t *)(addr + RECORD_POS_SIZE);
  log_printf("levitate: testing %#lx (off by %.3f, motion %s: %08x %08x %08x %08x)", (unsigned long)addr,
             candidate.error, motion_zero ? "zero" : "set", motion[0], motion[1], motion[2], motion[3]);
  test_wrote_motion = motion_zero;
  const volatile double *pos = (const volatile double *)addr;
  write_record(addr, pos[0], pos[1] + TEST_LIFT, pos[2], 0, 0, 0, test_wrote_motion);
  test_frames = 0;
  n_tested++;
  search_state = SEARCH_TESTING;
}

static void test_step(void)
{
  vec3f_t now;
  if (link_drawn_pos(&now) && now.y - test_start_pos.y > TEST_LIFT / 2) {
    record = candidate.addr;
    record_has_motion = true;
    last_found = record;
    log_printf("levitate: Link's physics record is %#lx (moved him; scanned %u MiB)", (unsigned long)record,
               (unsigned)(bytes_scanned >> 20));
    search_state = SEARCH_READY;
    return;
  }
  if (++test_frames < TEST_FRAMES)
    return;
  /* not Link's: put it back unless the game has rewritten it */
  uintptr_t addr = candidate.addr;
  double was_y;
  memcpy(&was_y, saved_record + 8, 8);
  if (abs_double(((volatile double *)addr)[1] - (was_y + TEST_LIFT)) < 1e-9)
    memcpy((void *)addr, saved_record, test_wrote_motion ? sizeof(saved_record) : RECORD_POS_SIZE);
  if (n_tested >= MAX_TESTS_PER_SEARCH) {
    log_printf("levitate: %d matches didn't move Link", n_tested);
    search_failed();
    return;
  }
  search_state = SEARCH_FINDING; /* carry on scanning */
}

static bool record_follows_link(void)
{
  vec3f_t drawn;
  if (!record || !link_drawn_pos(&drawn) || !sys_mapped(record, RECORD_SIZE, SYS_PERM_R | SYS_PERM_W))
    return false;
  volatile double *pos = (volatile double *)record;
  return abs_double(pos[0] - drawn.x) < FOLLOW_TOLERANCE && abs_double(pos[2] - drawn.z) < FOLLOW_TOLERANCE &&
         abs_double(pos[1] - drawn.y - PHYS_Y) < FOLLOW_TOLERANCE;
}

/* --- shared helpers ----------------------------------------------------------------------------- */

/* v moved towards target by at most step */
static double approach(double v, double target, double step)
{
  if (v < target)
    return v + step < target ? v + step : target;
  return v - step > target ? v - step : target;
}

static bool same_way(double ax, double az, double bx, double bz)
{
  double lengths = (double)hypotf((float)ax, (float)az) * hypotf((float)bx, (float)bz);
  return lengths > 0 && (ax * bx + az * bz) / lengths > SAME_WAY_MIN;
}

/* left stick, -1..1, dead zone applied */
static void stick(double *sx, double *sy)
{
  int32_t lx, ly, rx, ry;
  input_sticks(&lx, &ly, &rx, &ry);
  double length = hypotf((float)lx, (float)ly);
  if (length < STICK_DEAD) {
    *sx = 0;
    *sy = 0;
    return;
  }
  double scale = (length - STICK_DEAD) / (STICK_MAX - STICK_DEAD) / length;
  if (scale * length > 1)
    scale = 1 / length;
  *sx = lx * scale;
  *sy = ly * scale;
}

/* the camera's view direction on the ground, as a unit (x, z) */
static bool camera_forward(double *fx, double *fz)
{
  uintptr_t cam = camera();
  if (!cam || !sys_mapped(cam + CAM_POSITION, 24, SYS_PERM_R))
    return false;
  float *pos = (float *)(cam + CAM_POSITION);
  float *look = (float *)(cam + CAM_LOOK_AT);
  double dx = look[0] - pos[0];
  double dz = look[2] - pos[2];
  double length = hypotf((float)dx, (float)dz);
  if (length < 1e-3)
    return false;
  *fx = dx / length;
  *fz = dz / length;
  return true;
}

/* --- levitate --------------------------------------------------------------------------------- */

static bool floating;              /* L3 turned it on */
static bool moving_link;           /* we're writing Link's position */
static double pos_x, pos_y, pos_z; /* our position for him */
static double vel_x, vel_y, vel_z; /* and velocity */
static double heading_x, heading_z;            /* last direction he moved in */
static double wall_x, wall_z, wall_until;      /* a wall last pushed him this way, until this time */
static double past_headings[2][2];             /* his heading a moment ago and the moment before */
static double next_heading_sample;
static double float_clock;                     /* s of gameplay since the float started */
static double last_written[3];
static vec3f_t last_drawn;
static int frozen_frames;
static int float_trace; /* frames of float logged (diagnostics) */

static void drop(void)
{
  if (!floating)
    return;
  floating = false;
  if (moving_link) {
    moving_link = false;
    int32_t *cur, *max;
    if (link_health(&cur, &max)) {
      heal_to = *cur;
      heal_frames_left = HEAL_FRAMES;
    }
  }
}

static void float_start(void)
{
  volatile double *pos = (volatile double *)record;
  pos_x = pos[0];
  pos_y = pos[1];
  pos_z = pos[2];
  vel_x = vel_y = vel_z = 0;
  heading_x = heading_z = 0;
  wall_x = wall_z = 0;
  wall_until = next_heading_sample = float_clock = 0;
  memset(past_headings, 0, sizeof(past_headings));
  frozen_frames = 0;
  link_drawn_pos(&last_drawn);
  last_written[0] = pos_x;
  last_written[1] = pos_y;
  last_written[2] = pos_z;
  moving_link = true;
  float_trace = 0;
}

/* Gameplay frozen (pause screen, item menus; the frame counter keeps going there): neither has the
 * record moved off what we wrote nor Link been redrawn (game.py LEV_PAUSED_AFTER). */
static void track_frozen(double cx, double cy, double cz, vec3f_t drawn)
{
  bool moved = cx != last_written[0] || cy != last_written[1] || cz != last_written[2] ||
               drawn.x != last_drawn.x || drawn.y != last_drawn.y || drawn.z != last_drawn.z;
  frozen_frames = moved ? 0 : frozen_frames + 1;
  last_drawn = drawn;
}

/* The left stick and R3 move our velocity towards what they ask for; the rise is applied. */
static void steer(double dt)
{
  float_clock += dt;
  bool hold_height = input_held() & BTN_RSTICK;
  double rise = levitate_speed;
  double move = levitate_move_speed;
  vel_y = approach(vel_y, hold_height ? 0 : rise, rise / (hold_height ? STOP_RAMP : RISE_RAMP) * dt);
  double sx, sy, fx, fz;
  double target_x = 0, target_z = 0;
  stick(&sx, &sy);
  if ((sx || sy) && camera_forward(&fx, &fz)) {
    double rx = -fz, rz = fx; /* camera's right */
    target_x = (fx * sy + rx * sx) * move;
    target_z = (fz * sy + rz * sx) * move;
  }
  double step = move / MOVE_RAMP * dt;
  vel_x = approach(vel_x, target_x, step);
  vel_z = approach(vel_z, target_z, step);
  pos_y += vel_y * dt;
}

/* (cx, cz): where the record is now. Accept the game's push-backs out of walls and stop pushing into
 * a wall that has just pushed back. */
static void respect_walls(double cx, double cz)
{
  double game_dx = cx - pos_x, game_dz = cz - pos_z; /* how the game moved him since our last write */
  bool wall = float_clock < wall_until;
  if (float_clock >= next_heading_sample) {
    past_headings[1][0] = past_headings[0][0];
    past_headings[1][1] = past_headings[0][1];
    past_headings[0][0] = heading_x;
    past_headings[0][1] = heading_z;
    next_heading_sample = float_clock + STEADY_TIME;
  }
  bool steady = same_way(heading_x, heading_z, past_headings[0][0], past_headings[0][1]) &&
                same_way(heading_x, heading_z, past_headings[1][0], past_headings[1][1]);
  bool against_heading = game_dx * heading_x + game_dz * heading_z < 0;
  if (steady && against_heading && !(wall && game_dx * wall_x + game_dz * wall_z < 0)) {
    /* the game pushed him out of a wall: keep that */
    pos_x = cx;
    pos_z = cz;
    double length = hypotf((float)game_dx, (float)game_dz);
    if (length > PUSHBACK_MIN) {
      wall_x = game_dx / length;
      wall_z = game_dz / length;
      wall_until = float_clock + WALL_MEMORY;
      wall = true;
    }
  }
  if (wall) {
    double into = vel_x * wall_x + vel_z * wall_z;
    if (into < 0) {
      vel_x -= into * wall_x;
      vel_z -= into * wall_z;
    }
  }
}

static void float_step(double dt, bool menu_open)
{
  volatile double *pos = (volatile double *)record;
  double cx = pos[0], cy = pos[1], cz = pos[2];
  vec3f_t drawn;
  link_drawn_pos(&drawn);
  track_frozen(cx, cy, cz, drawn);
  if (float_trace < FLOAT_TRACE_FRAMES) { /* what the game did with our last write */
    double fx = 0, fz = 0;
    link_facing(&fx, &fz);
    log_printf("levitate f%d: record y %.3f (we wrote %.3f) x %.3f z %.3f | drawn y %.3f | facing %.3f,%.3f | "
               "frozen %d clock %.2f", float_trace, cy, last_written[1], cx, cz, (double)drawn.y, fx, fz,
               frozen_frames, float_clock);
    float_trace++;
  }
  if ((frozen_frames >= FROZEN_FRAMES && float_clock > FROZEN_AFTER) || menu_open) {
    vel_x = vel_y = vel_z = 0;
  } else {
    steer(dt);
  }
  if (!levitate_through_walls)
    respect_walls(cx, cz);
  pos_x += vel_x * dt;
  pos_z += vel_z * dt;
  if (hypotf((float)vel_x, (float)vel_z) > STILL_SPEED) {
    heading_x = vel_x;
    heading_z = vel_z;
  }
  /* Position every frame; the motion state only when the game has cleared it (it ignores the
   * position then), as game.py did. Rewriting all of it every frame kept Link spinning. */
  bool cleared = true;
  for (int i = 0; i < MOTION_WORDS; i++)
    cleared &= ((volatile uint32_t *)(record + RECORD_POS_SIZE))[i] == 0;
  write_record(record, pos_x, pos_y, pos_z, (float)vel_x, (float)vel_y, (float)vel_z, record_has_motion && cleared);
  last_written[0] = pos_x;
  last_written[1] = pos_y;
  last_written[2] = pos_z;
}

/* --- 2x speed ------------------------------------------------------------------------------------ */

static bool speed_known;
static double speed_x, speed_z; /* where we put him last frame */
static int speed_trace;

static void speed_step(void)
{
  volatile double *pos = (volatile double *)record;
  double sx, sy;
  stick(&sx, &sy);
  if (!(sx || sy)) {
    speed_known = false;
    return;
  }
  double cx = pos[0], cz = pos[2];
  if (speed_known) {
    double dx = cx - speed_x, dz = cz - speed_z; /* how far the game moved him this frame */
    if (speed_trace < SPEED_TRACE_FRAMES) {
      log_printf("speed f%d: game moved him %.3f, %.3f (record %.3f, %.3f; we put him %.3f, %.3f)", speed_trace, dx,
                 dz, cx, cz, speed_x, speed_z);
      speed_trace++;
    }
    if (dx * dx + dz * dz < SPEED_MAX_STEP_SQ) { /* not a warp or a respawn */
      pos[0] = cx + dx;
      pos[2] = cz + dz;
      cx += dx;
      cz += dz;
    }
  }
  speed_x = cx;
  speed_z = cz;
  speed_known = true;
}

/* --- frame --------------------------------------------------------------------------------------- */

/* seconds since the last frame, at most MAX_DT */
static double frame_seconds(void)
{
  uint64_t now = sys_tick();
  double dt = last_tick ? (double)(now - last_tick) / (double)sys_tick_freq() : 0;
  last_tick = now;
  if (dt > MAX_DT)
    dt = MAX_DT;
  return dt;
}

/* After a drop: undo any health lost landing. */
static void heal_after_drop(void)
{
  if (heal_frames_left > 0) {
    heal_frames_left--;
    int32_t *cur, *max;
    if (link_health(&cur, &max) && *cur < heal_to)
      *cur = heal_to;
  }
}

/* L3: start / stop */
static void handle_l3(bool menu_open)
{
  if (!levitate_enabled) {
    drop();
  } else if (!menu_open && (input_pressed() & BTN_LSTICK)) {
    if (floating) {
      drop();
      gfx_notice("levitate off");
    } else {
      floating = true;
      quiet_search = false;
      gfx_notice("levitate on");
    }
  }
}

/* Advance a running search. True while one runs: nothing else happens on those frames. */
static bool search_running(void)
{
  if (search_state == SEARCH_FINDING) {
    if (scan_step()) {
      test_start();
    } else if (scan_region >= n_heap_regions) {
      log_printf("levitate: no match in %u MiB of heap (%d tested)", (unsigned)(bytes_scanned >> 20), n_tested);
      search_failed();
    }
    if (search_state == SEARCH_FAILED)
      floating = false;
    return true;
  }
  if (search_state == SEARCH_TESTING) {
    test_step();
    if (search_state == SEARCH_FAILED)
      floating = false;
    return true;
  }
  return false;
}

/* --- Link's record, for position.c ------------------------------------------------------------- */

link_record_status_t link_record_status(void)
{
  switch (search_state) {
    case SEARCH_READY: return REC_READY;
    case SEARCH_FINDING: case SEARCH_TESTING: return REC_SEARCHING;
    case SEARCH_FAILED: return REC_FAILED;
    default: return requested ? REC_SEARCHING : REC_NONE;
  }
}

void link_record_request(void)
{
  if (search_state == SEARCH_READY)
    return;
  if (search_state == SEARCH_FAILED) /* asked again: search again now */
    search_state = SEARCH_IDLE;
  requested = true;
}

/* Position, plus the motion state (record_has_motion: the test that found it wrote there safely): the
 * game ignores a new position while Link stands unless that's written too, and standing leaves small
 * leftovers in it, so it's rarely all zero (game.py's move wrote it every time too). */
bool link_record_move(double x, double y, double z, float face_vx, float face_vz, int settle_frames)
{
  if (search_state != SEARCH_READY || !sys_mapped(record, RECORD_SIZE, SYS_PERM_R | SYS_PERM_W))
    return false;
  write_record(record, x, y + PHYS_Y, z, face_vx, 0, face_vz, record_has_motion);
  if (moving_link) { /* floating: carry on from there */
    pos_x = x;
    pos_y = y + PHYS_Y;
    pos_z = z;
    vel_x = vel_y = vel_z = 0;
    last_written[0] = pos_x;
    last_written[1] = pos_y;
    last_written[2] = pos_z;
  }
  speed_known = false;
  if (settle_frames > hold_frames)
    hold_frames = settle_frames;
  return true;
}

void link_record_resync(void)
{
  vec3f_t drawn;
  if (search_state != SEARCH_READY || !link_drawn_pos(&drawn) || !sys_mapped(record, 24, SYS_PERM_W))
    return;
  volatile double *pos = (volatile double *)record;
  pos[0] = drawn.x;
  pos[1] = drawn.y + PHYS_Y;
  pos[2] = drawn.z;
  if (moving_link) {
    pos_x = drawn.x;
    pos_y = drawn.y + PHYS_Y;
    pos_z = drawn.z;
    last_written[0] = pos_x;
    last_written[1] = pos_y;
    last_written[2] = pos_z;
  }
  hold_frames = 0;
}

/* --- frame --------------------------------------------------------------------------------------- */

void levitate_frame(bool menu_open)
{
  input_hide_from_game = (levitate_enabled ? BTN_LSTICK : 0) | (floating ? BTN_RSTICK : 0);
  if (hold_frames > 0)
    hold_frames--;
  double dt = frame_seconds();
  heal_after_drop();
  handle_l3(menu_open);

  if (search_running())
    return;
  if (search_state == SEARCH_FAILED && --retry_wait_frames <= 0)
    search_state = SEARCH_IDLE;

  if (search_state == SEARCH_READY && !hold_frames && !record_follows_link() && !(moving_link && frozen_frames)) {
    log_printf("levitate: lost Link's record (new area?)");
    record = 0;
    search_state = SEARCH_IDLE;
    moving_link = false;
    floating = false;
    speed_known = false;
  }

  bool want_speed = cheats[CHEAT_SPEED] && !floating && !menu_open;
  if (search_state == SEARCH_READY)
    requested = false;
  if (search_state == SEARCH_IDLE && (floating || want_speed || requested)) {
    search_start(!floating);
    return;
  }
  if (search_state != SEARCH_READY) {
    speed_known = false;
    return;
  }
  if (floating) {
    if (!moving_link)
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
  switch (search_state) {
    case SEARCH_FINDING:
    case SEARCH_TESTING:
      return "finding Link";
    case SEARCH_FAILED:
      return "not found";
    default:
      if (moving_link)
        return "floating";
      if (floating)
        return "starting";
      return record ? "ready" : "off";
  }
}
