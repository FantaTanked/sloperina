/* ootgz: a gz-style practice menu for Ocarina of Time (Switch 2), loaded by the game itself.
 *
 * How it gets control (BUILDING.md has the details):
 *   1. The game's exefs gets this module as subsdk9. rtld loads it with the game's other
 *      modules, links its imports (nnMain from main; nn::hid, vsnprintf... from sdk) and
 *      calls ootgz_module_init (DT_INIT) before the game starts.
 *   2. An IPS patch on main redirects the per-frame GameData unlock (FINDINGS §3) to a stub
 *      in main's code cave. The stub loads a pointer from a slot in main's bss and jumps to
 *      it if it's set, or runs the original unlock otherwise.
 *   3. ootgz_module_init checks the patch is the matching one and stores ootgz_frame_entry
 *      in the slot. From then on the module runs on the game's thread once per frame, right
 *      where the old patch handled the menu's requests, so the game's own functions are
 *      safe to call (give items, redraw screens...).
 */
#include "cheats.h"
#include "gamedata.h"
#include "gfx.h"
#include "input.h"
#include "job.h"
#include "link.h"
#include "menu.h"
#include "menus.h"
#include "airwall.h"
#include "levitate.h"
#include "settings.h"
#include "warp.h"
#include "watches.h"
#include "nvn.h"
#include "got.h"
#include "ootgz_version.h"
#include "pouch.h"
#include "sys.h"
#include "z64hd.h"

uintptr_t z64hd_main;

/* exported by main */
extern void nnMain(void);

typedef uint64_t (*gd_unlock_t)(void *lock);

static bool started;
static uint32_t frames;

#define MENU_COMBO (BTN_R | BTN_RS_DOWN) /* gz: R + C-down */
#define BACK_COMBO (BTN_R | BTN_DLEFT)   /* up one level; closes at the top */

/* --- frame hook -------------------------------------------------------------------------- */

/* --- frame advance ---------------------------------------------------------------------------
 * With it on (settings > frame advance), a tap of its button (L or R, menu closed) pauses the game:
 * this hook, on the game's thread, holds it here, as a debugger's breakpoint would. While held, a
 * tap lets one frame through, and the button + L3 lets it run again; the menu still opens (R + right
 * stick down) and works as usual. gz's pause icon shows at the top left. The game never sees the
 * button while it's on.
 * As in gz, everything else can be held or pressed meanwhile (the stick, A, Z...): the frame let
 * through reads the controller as it is, plus what was buffered while paused. Only R's shortcuts
 * (R + right stick down, R + d-pad...) don't count as a tap, so they don't pause the game; L has none. */
bool frame_advance = true, pause_display = true;
int frame_advance_button; /* 0 L, 1 R */
static bool fa_paused, fa_down, fa_dirty;
/* held (not as a shortcut) for FA_HOLD_MS, the button advances a frame every FA_REPEAT_MS until
 * it's let go: the wait leaves room for a tap that's a little long */
#define FA_HOLD_MS 400
#define FA_REPEAT_MS 66
static uint64_t fa_down_at, fa_last_step;
static bool fa_repeating;
/* The menu uses the button too (L selects; a warp closes the menu on the same press): while the menu is
 * open, on the frame it closes, until the button is let go and for FA_AFTER_MENU_MS after, it isn't
 * frame advance's. */
#define FA_AFTER_MENU_MS 250
static bool fa_menu_was_open, fa_blocked;
static uint64_t fa_menu_closed_at;

static uint32_t fa_bit(void) { return frame_advance_button ? BTN_R : BTN_L; }
/* what, held or pressed with the button, makes it a shortcut rather than a tap */
static uint32_t fa_shortcut_keys(void)
{
  return frame_advance_button ? (BTN_DUP | BTN_DDOWN | BTN_DLEFT | BTN_DRIGHT | BTN_RS_DOWN) : 0;
}

/* the button's state this frame: 1 tapped, 2 + L3 (run), 3 held (another frame), 0 nothing */
static int fa_button(void)
{
  uint32_t b = fa_bit(), pressed = input_pressed(), held = input_held();
  uint64_t ms = sys_tick_freq() / 1000, now = sys_tick();
  if (menu_current || fa_menu_was_open) {
    fa_down = false, fa_blocked = true, fa_menu_closed_at = now;
    return 0;
  }
  if (fa_blocked) {
    if ((held & b) || now - fa_menu_closed_at < FA_AFTER_MENU_MS * ms)
      return 0;
    fa_blocked = false;
  }
  if (pressed & b)
    fa_down = true, fa_dirty = (held & fa_shortcut_keys()) != 0, fa_down_at = sys_tick(), fa_repeating = false;
  if (!fa_down)
    return 0;
  if ((held & b) && (held & BTN_LSTICK) && ((pressed & b) || (pressed & BTN_LSTICK))) {
    fa_dirty = true;
    return 2;
  }
  if (pressed & fa_shortcut_keys())
    fa_dirty = true;
  if (!(held & b)) {
    bool tap = !fa_dirty && !fa_repeating; /* a hold already advanced: no extra frame */
    fa_down = fa_repeating = false;
    return tap ? 1 : 0;
  }
  /* held: after FA_HOLD_MS, a frame every FA_REPEAT_MS (only while paused: fa_hold) */
  if (fa_paused && !fa_dirty && now - fa_down_at >= FA_HOLD_MS * ms &&
      (!fa_repeating || now - fa_last_step >= FA_REPEAT_MS * ms)) {
    fa_repeating = true, fa_last_step = now;
    return 3;
  }
  return 0;
}

static bool menu_controls(void)
{
  if (!menu_current && input_combo(MENU_COMBO))
    menu_open(&menu_root);
  else if (menu_current && input_combo(MENU_COMBO))
    menu_close(); /* the open combo toggles it */
  else if (menu_current && input_combo(BACK_COMBO))
    menu_back();
  else if (menu_current) {
    watches_sync();
    menu_input();
  } else
    return false;
  return true;
}

/* What's pressed while paused is buffered and given to the game on the frame that's let through:
 * every button pressed (not the frame advance button, nor L3 pressed with it), and each stick's last
 * direction away from the middle. */
#define FA_DEADZONE 6000
static uint32_t fa_buf_buttons;
static bool fa_buf_stick[2];
static int32_t fa_buf_sticks[4];

static void fa_buffer(void)
{
  if (menu_current) /* what's pressed for the menu isn't for the game */
    return;
  uint32_t held = input_held(), b = fa_bit();
  uint32_t game = held & ~b & 0xFFFF; /* the buttons (not the stick-direction bits) */
  if (held & b)
    game &= ~BTN_LSTICK; /* L3 with the button: run, not for the game */
  fa_buf_buttons |= game;
  int32_t st[4];
  input_sticks(&st[0], &st[1], &st[2], &st[3]);
  for (int k = 0; k < 2; k++) {
    int32_t x = st[2 * k], y = st[2 * k + 1];
    if (x > FA_DEADZONE || x < -FA_DEADZONE || y > FA_DEADZONE || y < -FA_DEADZONE)
      fa_buf_stick[k] = true, fa_buf_sticks[2 * k] = x, fa_buf_sticks[2 * k + 1] = y;
  }
}

static void fa_hold(void)
{
  uint32_t presents = gfx_presents;
  uint64_t since = sys_tick();
  bool logged = false;
  /* the frame let through has been played: its buffered input is done with */
  input_force_active = false;
  fa_buf_buttons = 0, fa_buf_stick[0] = fa_buf_stick[1] = false;
  /* The game presents nothing while it's held here (0 presents, measured), so what shows while
   * paused is the last frame let through: it gets the pause icon (set before it's presented). */
  gfx_pause_icon = pause_display ? 1 : 0;
  for (;;) {
    sys_sleep_ns(16 * 1000 * 1000);
    input_update();
    menu_controls();
    int b = fa_button();
    if (b == 2) { /* the button + L3: run */
      fa_paused = false;
      gfx_pause_icon = 0;
      gfx_notice("frame advance: running");
      break;
    }
    if (b == 1 || b == 3) { /* one frame (a tap, or the button held), with what was pressed meanwhile */
      input_force_buttons = fa_buf_buttons;
      input_force_sticks[0] = fa_buf_stick[0], input_force_sticks[1] = fa_buf_stick[1];
      memcpy(input_force_stick, fa_buf_sticks, sizeof(input_force_stick));
      input_force_active = fa_buf_buttons || fa_buf_stick[0] || fa_buf_stick[1];
      break;
    }
    fa_buffer();
    if (menu_current) {
      watches_sync();
      menu_draw();
    }
    watches_frame(menu_current != 0);
    gfx_frame();
    settings_frame(menu_current != 0);
    settings_log_frame();
    if (!logged && sys_tick() - since > sys_tick_freq()) { /* does the picture still get presented? */
      logged = true;
      log_printf("frame advance: %u presents in the first second paused", (unsigned)(gfx_presents - presents));
    }
  }
}

static void fa_frame(void)
{
  if (!frame_advance) {
    fa_paused = fa_down = false;
    gfx_pause_icon = 0;
    input_force_active = false;
    return;
  }
  input_hide_from_game |= fa_bit();
  if (!fa_paused)
    input_force_active = false;
  if (!fa_paused && fa_button() == 1) {
    fa_paused = true;
    gfx_notice(frame_advance_button ? "frame advance: R a frame, R + L3 to run" : "frame advance: L a frame, L + L3 to run");
    /* The game shows nothing new while it's held, so this frame is let through first, with the pause
     * icon on it: the screen shows the game is paused (and hasn't crashed). Held from the next one. */
    gfx_pause_icon = pause_display ? 1 : 0;
    return;
  }
  if (fa_paused)
    fa_hold();
}

static void frame(void)
{
  if (!started) {
    started = true;
    menus_build();
    settings_load();
    log_printf("running (patch v%d, main %#lx)", OOTGZ_PATCH_VERSION, (unsigned long)z64hd_main);
    menus_splash();
  }
  frames++;
  input_update();

  fa_menu_was_open = menu_current != 0; /* an L that closes the menu isn't frame advance's */
  menu_controls();
  if (menu_current) {
    watches_sync();
    menu_draw();
  }
  airwall_frame();
  menus_research_frame();
  watches_frame(menu_current != 0);
  levitate_frame(menu_current != 0);
  warp_frame();
  gfx_frame();

  cheats_frame();
  pouch_frame();
  job_frame();
  settings_frame(menu_current != 0);
  settings_log_frame();
  fa_frame(); /* last: the frame's work is done before the game is held */
}

/* Called by the IPS stub instead of the GameData unlock (x0 = the lock), on the game's thread.
 * Unlock first, exactly as the site did, then do our work. */
__attribute__((used)) uint64_t ootgz_frame_entry(void *lock)
{
  uint64_t r = MAIN_FN(gd_unlock_t, Z64HD_GD_UNLOCK)(lock);
  frame();
  return r;
}

/* --- module init (DT_INIT) --------------------------------------------------------------- */

static uint32_t insn_b(uintptr_t from, uintptr_t to)
{
  return 0x14000000u | (uint32_t)(((intptr_t)(to - from) >> 2) & 0x3FFFFFF);
}

/* Runs inside rtld's DT_INIT: no sdk calls (log_raw only), just checks and the slot. */
static const char *patch_problem(void)
{
  const uint8_t *cave = (const uint8_t *)MAIN(Z64HD_CAVE_START);
  const char *marker = OOTGZ_MARKER;
  for (int i = 0; i < 8; i++)
    if (cave[i] != (uint8_t)marker[i])
      return "no ootgz patch in main: install the .ips next to subsdk9";
  if (*(const uint32_t *)(cave + 8) != OOTGZ_PATCH_VERSION)
    return "the ootgz patch in main is a different version: install the matching .ips";
  uint32_t site = *(const uint32_t *)MAIN(Z64HD_FRAME_SITE);
  if (site != insn_b(MAIN(Z64HD_FRAME_SITE), MAIN(Z64HD_CAVE_START + OOTGZ_STUB_OFF)))
    return "frame site not patched (another mod on the same site, e.g. ootmenu?)";
  if (*(volatile uintptr_t *)MAIN(Z64HD_HOOK_SLOT) != 0)
    return "hook slot in main's bss is not empty; not hooking (update Z64HD_HOOK_SLOT)";
  return 0;
}

__attribute__((used)) void ootgz_module_init(void)
{
  z64hd_main = (uintptr_t)&nnMain - Z64HD_NNMAIN;
  const char *problem = patch_problem();
  if (problem) {
    log_raw(problem);
    return;
  }
  *(volatile uintptr_t *)MAIN(Z64HD_HOOK_SLOT) = (uintptr_t)ootgz_frame_entry;
  log_raw("frame hook installed");
  /* main's imports: NVN for drawing, nn::hid to hide input while the menu is open */
  const char *e = got_hook(z64hd_main, "nvnBootstrapLoader", nvn_bootstrap_hook);
  log_raw(e ? "NVN hook failed" : "NVN hook installed");
  if (e)
    log_raw(e);
  e = input_hook_game(z64hd_main);
  log_raw(e ? "input hook failed" : "input hook installed");
  if (e)
    log_raw(e);
}
