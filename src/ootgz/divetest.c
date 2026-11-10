/* Dive test (debug > dive test): how deep Link goes into water by how he falls in.
 *
 * Start it with Link swimming at the surface where the drops should land (and facing open water: the
 * moving test sends him that way). It takes the surface's height there (Link held still for a moment),
 * then, for each drop in the test's list (RUNS times each), puts him that far above the surface with no
 * vertical speed, and the drop's speed the way he faces (his physics record, as save / load position do),
 * lets him fall, and logs (log.txt in full, "dive test: ..."; a short line on screen):
 *   - how long he fell, and his vertical and horizontal speed as he went in (the last frame above the
 *     surface; units/s);
 *   - the deepest he got below the surface, how long after going in, and how far he'd gone sideways.
 * The heights test drops him from 1 to 192 up; the moving test from 16 and 96 up at a few speeds. Choosing
 * either again stops it. It waits while the menu is open. */
#include "divetest.h"

#include "gfx.h"
#include "levitate.h"
#include "link.h"
#include "sys.h"

#define LINK_VELOCITY 0x388 /* Link's actor: velocity x, y, z (floats, units/s), as the y speed watch */

typedef struct { float height, speed; } drop_t; /* units above the surface; units/s the way he faces */
static const drop_t height_drops[] = {
  /* doubling, then closer steps where his fall stops speeding up */
  { 1, 0 }, { 2, 0 }, { 4, 0 }, { 8, 0 }, { 16, 0 }, { 32, 0 }, { 48, 0 }, { 64, 0 }, { 80, 0 }, { 96, 0 },
  { 112, 0 }, { 128, 0 }, { 160, 0 }, { 192, 0 },
};
static const drop_t moving_drops[] = {
  /* below his top falling speed and at it; a jump attack's is about 6.8 */
  { 16, 0 }, { 16, 3.4f }, { 16, 6.8f }, { 16, 13.6f }, { 96, 0 }, { 96, 3.4f }, { 96, 6.8f }, { 96, 13.6f },
};
static const struct { const drop_t *drops; int n; } tests[DIVETEST_COUNT] = {
  [DIVETEST_HEIGHTS] = { height_drops, (int)(sizeof(height_drops) / sizeof(height_drops[0])) },
  [DIVETEST_MOVING] = { moving_drops, (int)(sizeof(moving_drops) / sizeof(moving_drops[0])) },
};
#define RUNS 2

#define STILL_SECONDS 1.5f  /* Link this still at the surface, to measure it */
#define STILL_SPREAD 0.35f  /* ... his height within this (he bobs) */
#define FALL_TIMEOUT 15.0f  /* seconds: he didn't reach the water (landed on something?) */
#define BOTTOM_SECONDS 1.5f /* no deeper for this long: that was the deepest */
#define RISE 0.5f           /* ... or he's come this far back up from it */
#define UNDER_TIMEOUT 20.0f

static enum { DT_OFF, DT_FIND, DT_SURFACE, DT_FALL, DT_UNDER } state;
static int test;                /* DIVETEST_* */
static double ax, az, surface;  /* where the drops are, and the surface's height there */
static double fx, fz;           /* the way Link faces */
static int step;                /* the drop: drops[step / RUNS], run step % RUNS */
static uint64_t since;          /* when this part started (sys_tick) */
static float still_min, still_max;
static double still_sum;
static int still_n;
static float last_vy, last_vh;  /* the frame before */
static float entry_vy, entry_vh, fall_s; /* going in */
static float deepest_y, deepest_s, peak_down, deepest_far; /* under water */

static const drop_t *drop_now(void)
{
  return &tests[test].drops[step / RUNS];
}

static float seconds_since(uint64_t t)
{
  return (float)(sys_tick() - t) / (float)sys_tick_freq();
}

/* a square root by the CPU's instruction (the compiler's calls sqrtf for errno, which the module doesn't
 * import: as nativeui.c's root) */
static float root(float v)
{
  float r;
  __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(v > 0 ? v : 0));
  return r;
}

/* his vertical speed and his speed across */
static bool speeds(float *vy, float *vh)
{
  uintptr_t link = link_actor();
  if (!link || !sys_mapped(link + LINK_VELOCITY, 12, SYS_PERM_R))
    return false;
  volatile float *v = (volatile float *)(link + LINK_VELOCITY);
  *vy = v[1];
  *vh = root(v[0] * v[0] + v[2] * v[2]);
  return true;
}

static void stop(const char *why)
{
  state = DT_OFF;
  gfx_logf("dive test: %s", why);
}

bool divetest_running(void)
{
  return state != DT_OFF;
}

void divetest_toggle(int which)
{
  if (state != DT_OFF) {
    stop("stopped");
    return;
  }
  test = which >= 0 && which < DIVETEST_COUNT ? which : DIVETEST_HEIGHTS;
  state = DT_FIND;
  step = 0;
  link_record_request();
  gfx_logf("dive test: close the menu with Link swimming at the surface");
}

static void start_surface(void)
{
  state = DT_SURFACE;
  since = sys_tick();
  still_min = 1e30f, still_max = -1e30f, still_sum = 0, still_n = 0;
}

static void drop(void)
{
  const drop_t *d = drop_now();
  /* the drop's speed the way he faces (a little, to keep him facing it, when it has none) */
  float speed = d->speed > 0 ? d->speed : 0.06f;
  if (!link_record_move(ax, surface + d->height, az, (float)(fx * speed), (float)(fz * speed), 2)) {
    stop("couldn't move Link (his record was lost); start it again");
    return;
  }
  char note[64];
  snprintf(note, sizeof(note), "dive test: drop %d of %d (%.0f up, %.1f u/s)", step + 1, tests[test].n * RUNS,
           (double)d->height, (double)d->speed);
  gfx_notice(note);
  state = DT_FALL;
  since = sys_tick();
  last_vy = 0;
  last_vh = d->speed;
}

static void next_drop(void)
{
  if (++step >= tests[test].n * RUNS) {
    stop("done");
    return;
  }
  drop();
}

void divetest_frame(bool menu_open)
{
  if (state == DT_OFF || menu_open)
    return;
  vec3f_t pos;
  float vy = 0, vh = 0;
  if (state != DT_FIND && (!link_drawn_pos(&pos) || !speeds(&vy, &vh)))
    return;
  switch (state) {
    case DT_FIND:
      if (link_record_status() == REC_READY) {
        start_surface();
      } else if (link_record_status() == REC_FAILED) {
        stop("couldn't find Link's physics record; try again after moving a little");
      } else {
        link_record_request();
      }
      break;
    case DT_SURFACE: /* Link still at the surface: its height there */
      still_min = pos.y < still_min ? pos.y : still_min;
      still_max = pos.y > still_max ? pos.y : still_max;
      if (still_max - still_min > STILL_SPREAD) { /* he's moving: again */
        start_surface();
        break;
      }
      still_sum += pos.y;
      still_n++;
      if (seconds_since(since) >= STILL_SECONDS && still_n > 0) {
        surface = still_sum / still_n;
        ax = pos.x;
        az = pos.z;
        if (!link_facing(&fx, &fz))
          fx = 0, fz = 1;
        log_printf("dive test: %s test; surface at y %.2f (x %.2f, z %.2f; bobbing %.2f; facing %.2f, %.2f)",
                   test == DIVETEST_MOVING ? "moving" : "heights", surface, ax, az, (double)(still_max - still_min),
                   fx, fz);
        drop();
      }
      break;
    case DT_FALL:
      if (pos.y <= surface && seconds_since(since) > 0.05f) { /* going in */
        entry_vy = last_vy;
        entry_vh = last_vh;
        fall_s = seconds_since(since);
        deepest_y = pos.y;
        deepest_s = 0;
        peak_down = vy < 0 ? -vy : 0;
        deepest_far = root((float)((pos.x - ax) * (pos.x - ax) + (pos.z - az) * (pos.z - az)));
        state = DT_UNDER;
        since = sys_tick();
      } else if (seconds_since(since) > FALL_TIMEOUT) {
        log_printf("dive test: %.0f up, %.1f u/s: didn't reach the water (landed at y %.2f?)",
                   (double)drop_now()->height, (double)drop_now()->speed, (double)pos.y);
        next_drop();
        return;
      }
      break;
    case DT_UNDER: {
      float t = seconds_since(since);
      if (pos.y < deepest_y) {
        deepest_y = pos.y;
        deepest_s = t;
        deepest_far = root((float)((pos.x - ax) * (pos.x - ax) + (pos.z - az) * (pos.z - az)));
      }
      if (-vy > peak_down)
        peak_down = -vy;
      if (pos.y > deepest_y + RISE || t - deepest_s > BOTTOM_SECONDS || t > UNDER_TIMEOUT) {
        const drop_t *d = drop_now();
        log_printf("dive test: %.0f up, %.1f u/s (run %d): fell %.2f s, in at %.2f down %.2f across, deepest %.2f "
                   "below after %.2f s (fastest down %.2f; %.1f across from the drop)",
                   (double)d->height, (double)d->speed, step % RUNS + 1, (double)fall_s, (double)entry_vy,
                   (double)entry_vh, surface - deepest_y, (double)deepest_s, (double)peak_down, (double)deepest_far);
        char note[64];
        snprintf(note, sizeof(note), "dive test: %.0f up, %.1f u/s: %.2f deep", (double)d->height, (double)d->speed,
                 surface - deepest_y);
        gfx_notice(note);
        next_drop();
        return;
      }
      break;
    }
    default:
      break;
  }
  last_vy = vy;
  last_vh = vh;
}
