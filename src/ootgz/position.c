/* Save / load position (gz).
 *
 * Saving reads Link's drawn position, his facing and the map. Moving him back goes through his
 * physics record (levitate.c), which has to be found first in each area: a heap scan that test-lifts
 * the matches, a second or two. The first save (or load) in an area starts that search and says so
 * ("Locating Link's starting position"), rather than acting later out of the blue; the next press works.
 * Saving needs the record so that the load after it is instant.
 *
 * A load waits until R and the d-pad of the shortcut are let go: holding R brings up the game's item
 * wheel, which freezes gameplay, and a frozen Link ignores a new position (every load made while they
 * were held failed). "loading position" shows at once, as "position saved" does (saving only reads, so
 * it doesn't wait), and "position loaded" when he's there. It's then checked: done once
 * Link is drawn there; otherwise written again a few times, and if it never takes, the record is put
 * back where Link is (so it isn't "lost" and searched for again). While it settles, the game's stores
 * can't move X, Y or Z (lock.c), so the game can't pull him off the write. */
#include "position.h"

#include "gamedata.h"
#include "gfx.h"
#include "input.h"
#include "levitate.h"
#include "link.h"
#include "lock.h"
#include "sys.h"

#define SAVE_COMBO (BTN_R | BTN_DUP)
#define LOAD_COMBO (BTN_R | BTN_DDOWN)
#define SETTLE 20       /* frames for a load to show: checked each frame, then given up */
#define MAX_WRITES 4    /* written again every RETRY_EVERY frames until Link is there */
#define RETRY_EVERY 3
#define NEAR 1.0        /* units per axis: Link counts as there */
#define FACE_SPEED 0.06 /* the velocity that turns him to face the saved way (above levitate's "none") */

static bool saved;
static double sx, sy, sz, sfx, sfz; /* drawn position; facing as a unit (x, z) */
static char smap[64];

static bool finding;  /* a press asked levitate.c for the record: say when it's found or not */
static bool pending;  /* load pressed: waiting for the buttons to be let go */
static int settle;    /* frames left to see Link arrive */
static int writes;

static double absd(double v) { return v < 0 ? -v : v; }

static bool same_str(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

static bool in_saved_map(void)
{
  const char *m = gd_get_str(GD_STR_MAP);
  return !m || !smap[0] || same_str(m, smap);
}

/* the record isn't known yet: ask for it and say so; the next press, once it's found, works */
static bool need_record(void)
{
  if (link_record_status() == REC_READY)
    return false;
  link_record_request();
  finding = true;
  gfx_notice("Locating Link's starting position, please try again");
  return true;
}

void position_save(void)
{
  if (need_record())
    return;
  vec3f_t p;
  if (!link_drawn_pos(&p)) {
    gfx_notice("save position: Link not found");
    return;
  }
  sx = p.x;
  sy = p.y;
  sz = p.z;
  sfx = sfz = 0;
  double fx, fz;
  if (link_facing(&fx, &fz)) {
    sfx = fx;
    sfz = fz;
  }
  const char *m = gd_get_str(GD_STR_MAP);
  snprintf(smap, sizeof(smap), "%s", m ? m : "");
  saved = true;
  log_printf("position saved: %.3f %.3f %.3f facing %.3f,%.3f in %s", sx, sy, sz, sfx, sfz, smap);
  gfx_notice("position saved");
}

void position_load(void)
{
  if (!saved) {
    gfx_notice("No position saved yet");
    return;
  }
  if (!in_saved_map()) {
    gfx_notice("the saved position is in another map");
    return;
  }
  if (need_record())
    return;
  pending = true;
  gfx_notice("loading position"); /* now, while the wheel's up; "position loaded" once he's there */
}

static bool write_saved(void)
{
  writes++;
  return link_record_move(sx, sy, sz, (float)(sfx * FACE_SPEED), (float)(sfz * FACE_SPEED), SETTLE + 2);
}

static bool link_there(void)
{
  vec3f_t d;
  return link_drawn_pos(&d) && absd(d.x - sx) < NEAR && absd(d.y - sy) < NEAR && absd(d.z - sz) < NEAR;
}

/* the load is over one way or another: the game's stores can move Link again */
static void settle_done(void)
{
  settle = 0;
  lock_hold_set(false);
}

/* each frame after a load's first write */
static void settle_step(void)
{
  if (!in_saved_map() || link_record_status() != REC_READY) {
    settle_done(); /* another map now (the record is gone): leave it alone */
    log_printf("position load abandoned (map changed)");
    return;
  }
  if (link_there()) {
    log_printf("position loaded (%d write%s)", writes, writes == 1 ? "" : "s");
    gfx_notice("position loaded");
    settle_done();
    return;
  }
  if (--settle == 0) {
    lock_hold_set(false);
    link_record_resync();
    log_printf("position load didn't take after %d writes", writes);
    gfx_notice("load position didn't work, try again");
    return;
  }
  if (writes < MAX_WRITES && (SETTLE - settle) % RETRY_EVERY == 0)
    write_saved();
}

void position_frame(bool menu_open)
{
  if (!menu_open && input_combo(SAVE_COMBO))
    position_save();
  else if (!menu_open && input_combo(LOAD_COMBO))
    position_load();

  link_record_status_t st = link_record_status();
  if (finding && st == REC_READY) {
    finding = false;
    gfx_notice("found Link's position: save / load position ready");
  } else if (finding && st == REC_FAILED) {
    finding = pending = false;
    gfx_notice("Link's position not found, move around and try again");
  }

  if (pending && st == REC_READY && !(input_held() & LOAD_COMBO)) {
    pending = false;
    writes = 0;
    lock_hold_set(true); /* the write holds: the game's stores can't pull him off it while it settles */
    if (write_saved()) {
      settle = SETTLE;
      log_printf("position load: writing %.3f %.3f %.3f", sx, sy, sz);
    } else {
      lock_hold_set(false);
    }
  } else if (settle) {
    settle_step();
  }
}
