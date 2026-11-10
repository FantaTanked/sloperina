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
#include "age.h"
#include "cheats.h"
#include "gamedata.h"
#include "gamefont.h"
#include "gameimage.h"
#include "uishader.h"
#include "gfx.h"
#include "input.h"
#include "job.h"
#include "link.h"
#include "menu.h"
#include "menus.h"
#include "airwall.h"
#include "enemyhp.h"
#include "overlayui.h"
#include "levitate.h"
#include "settings.h"
#include "warp.h"
#include "watches.h"
#include "nvn.h"
#include "got.h"
#include "ootgz_version.h"
#include "pouch.h"
#include "position.h"
#include "lock.h"
#include "sys.h"
#include "z64hd.h"
#include "z64hd_builds.h"

uintptr_t z64hd_main;
const struct z64hd_build *z64hd_game;

/* exported by main */
extern void nnMain(void);

typedef uint64_t (*gd_unlock_t)(void *lock);

static bool started;
static uint32_t frames_run;

#define MENU_COMBO (BTN_R | BTN_RS_DOWN) /* gz: R + C-down */
#define BACK_COMBO (BTN_R | BTN_DLEFT)   /* up one level; closes at the top */
#define BACK_BUTTON BTN_B

/* --- frame advance ---------------------------------------------------------------------------
 * With it on (settings > frame advance), a tap of its button (L or R, menu closed) pauses the game:
 * this hook, on the game's thread, holds it here, as a debugger's breakpoint would. While held, a
 * tap lets one frame through, and the button + L3 lets it run again; the menu still opens (R + right
 * stick down) and works as usual. gz's pause icon shows at the top left. The game never sees the
 * button while it's on.
 * As in gz, everything else can be held or pressed meanwhile (the stick, A, Z...): the frame let
 * through reads the controller as it is, plus what was buffered while paused. Only R's shortcuts
 * (R + right stick down, R + d-pad...) don't count as a tap, so they don't pause the game; L has none. */
bool frame_advance = true;
bool pause_display = true;
int frame_advance_button; /* 0 L, 1 R */

/* Held (not as a shortcut) for FA_HOLD_MS, the button advances a frame every FA_REPEAT_MS
 * until it's let go. The wait leaves room for a tap that's a little long. */
#define FA_HOLD_MS 400
#define FA_REPEAT_MS 66

/* The menu uses the button too (L selects; a warp closes the menu on the same press). While the menu
 * is open, on the frame it closes, until the button is let go and for FA_AFTER_MENU_MS after, the
 * button isn't frame advance's. */
#define FA_AFTER_MENU_MS 250

/* While paused, how long to sleep between polls of the controller (about one 60 Hz frame). */
#define FA_POLL_NS (16 * 1000 * 1000)

static bool fa_paused;
static bool fa_button_down;     /* the button went down and hasn't been let go yet */
static bool fa_not_a_tap;       /* something else was pressed while it was down */
static uint64_t fa_button_down_at;
static bool fa_repeating;       /* held long enough that it's stepping frames on its own */
static uint64_t fa_last_repeat_at;
static bool fa_menu_was_open;   /* the menu was open at the start of this frame */
static bool fa_blocked_after_menu;
static uint64_t fa_menu_closed_at;

/* What the frame advance button did this frame. */
typedef enum {
  FA_NOTHING = 0,
  FA_TAP = 1,    /* pressed and let go on its own */
  FA_RUN = 2,    /* the button + L3: run again */
  FA_REPEAT = 3, /* held while paused: another frame */
} fa_event_t;

static uint32_t fa_button_bit(void) { return frame_advance_button ? BTN_R : BTN_L; }

/* What, held or pressed with the button, makes it a shortcut rather than a tap. */
static uint32_t fa_shortcut_keys(void)
{
  return frame_advance_button ? (BTN_DUP | BTN_DDOWN | BTN_DLEFT | BTN_DRIGHT | BTN_RS_DOWN) : 0;
}

static fa_event_t fa_read_button(void)
{
  uint32_t button = fa_button_bit();
  uint32_t pressed = input_pressed();
  uint32_t held = input_held();
  uint64_t ticks_per_ms = sys_tick_freq() / 1000;
  uint64_t now = sys_tick();

  if (menu_current || fa_menu_was_open) {
    fa_button_down = false;
    fa_blocked_after_menu = true;
    fa_menu_closed_at = now;
    return FA_NOTHING;
  }
  if (fa_blocked_after_menu) {
    if ((held & button) || now - fa_menu_closed_at < FA_AFTER_MENU_MS * ticks_per_ms)
      return FA_NOTHING;
    fa_blocked_after_menu = false;
  }

  if (pressed & button) {
    fa_button_down = true;
    fa_not_a_tap = (held & fa_shortcut_keys()) != 0;
    fa_button_down_at = sys_tick();
    fa_repeating = false;
  }
  if (!fa_button_down)
    return FA_NOTHING;

  bool run_combo_pressed = (pressed & button) || (pressed & BTN_LSTICK);
  if ((held & button) && (held & BTN_LSTICK) && run_combo_pressed) {
    fa_not_a_tap = true;
    return FA_RUN;
  }
  if (pressed & fa_shortcut_keys())
    fa_not_a_tap = true;

  if (!(held & button)) {
    bool tap = !fa_not_a_tap && !fa_repeating; /* a hold already advanced: no extra frame */
    fa_button_down = false;
    fa_repeating = false;
    return tap ? FA_TAP : FA_NOTHING;
  }

  /* Held: after FA_HOLD_MS, a frame every FA_REPEAT_MS (only while paused, in fa_hold_game). */
  bool held_long_enough = now - fa_button_down_at >= FA_HOLD_MS * ticks_per_ms;
  bool repeat_due = !fa_repeating || now - fa_last_repeat_at >= FA_REPEAT_MS * ticks_per_ms;
  if (fa_paused && !fa_not_a_tap && held_long_enough && repeat_due) {
    fa_repeating = true;
    fa_last_repeat_at = now;
    return FA_REPEAT;
  }
  return FA_NOTHING;
}

/* Opens, closes and drives the menu. Returns true if the input was the menu's. */
static bool menu_controls(void)
{
  if (!menu_current && input_combo(MENU_COMBO))
    menu_open(&menu_root);
  else if (menu_current && input_combo(MENU_COMBO))
    menu_close(); /* the open combo toggles it */
  else if (menu_current && (input_combo(BACK_COMBO) || (input_pressed() & BACK_BUTTON)))
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
#define BUTTON_BITS 0xFFFF /* the buttons, not the stick-direction bits above them */

static uint32_t buffered_buttons;
static bool buffered_stick_moved[2];   /* left, right */
static int32_t buffered_stick_axes[4]; /* lx, ly, rx, ry */

static void fa_buffer_input(void)
{
  if (menu_current) /* what's pressed for the menu isn't for the game */
    return;
  uint32_t held = input_held();
  uint32_t button = fa_button_bit();
  uint32_t for_game = held & ~button & BUTTON_BITS;
  if (held & button)
    for_game &= ~BTN_LSTICK; /* L3 with the button means "run", not something for the game */
  buffered_buttons |= for_game;

  int32_t axes[4];
  input_sticks(&axes[0], &axes[1], &axes[2], &axes[3]);
  for (int stick = 0; stick < 2; stick++) {
    int32_t x = axes[2 * stick];
    int32_t y = axes[2 * stick + 1];
    if (x > FA_DEADZONE || x < -FA_DEADZONE || y > FA_DEADZONE || y < -FA_DEADZONE) {
      buffered_stick_moved[stick] = true;
      buffered_stick_axes[2 * stick] = x;
      buffered_stick_axes[2 * stick + 1] = y;
    }
  }
}

/* Hand the buffered input to the game for the one frame that's let through. */
static void fa_release_buffered_input(void)
{
  input_force_buttons = buffered_buttons;
  input_force_sticks[0] = buffered_stick_moved[0];
  input_force_sticks[1] = buffered_stick_moved[1];
  memcpy(input_force_stick, buffered_stick_axes, sizeof(input_force_stick));
  input_force_active = buffered_buttons || buffered_stick_moved[0] || buffered_stick_moved[1];
}

/* Holds the game's thread here while paused, running the menu and overlays, until a frame is let
 * through or the game is set running again. */
static void fa_hold_game(void)
{
  uint32_t presents_at_start = gfx_presents;
  uint64_t paused_at = sys_tick();
  bool logged = false;
  /* the frame let through has been played: its buffered input is done with */
  input_force_active = false;
  buffered_buttons = 0;
  buffered_stick_moved[0] = false;
  buffered_stick_moved[1] = false;
  /* The game presents nothing while it's held here (0 presents, measured), so what shows while
   * paused is the last frame let through: it gets the pause icon (set before it's presented). */
  gfx_pause_icon = pause_display ? 1 : 0;
  for (;;) {
    sys_sleep_ns(FA_POLL_NS);
    sys_frame();
    input_update();
    /* The button + L3 always runs again, even with the menu open: nothing is drawn while paused, so a
     * menu opened here can't be seen, and it would take the button. Checked before the menu gets it. */
    uint32_t held = input_held();
    uint32_t pressed = input_pressed();
    uint32_t button = fa_button_bit();
    if ((held & button) && (held & BTN_LSTICK) && (pressed & (button | BTN_LSTICK))) {
      if (menu_current)
        menu_close();
      fa_button_down = false;
      fa_repeating = false;
      fa_blocked_after_menu = true; /* letting go of the button isn't a tap */
      fa_menu_closed_at = sys_tick();
      fa_paused = false;
      gfx_pause_icon = 0;
      gfx_notice("frame advance: running");
      break;
    }
    menu_controls();
    fa_event_t event = fa_read_button();
    if (event == FA_RUN) {
      fa_paused = false;
      gfx_pause_icon = 0;
      gfx_notice("frame advance: running");
      break;
    }
    if (event == FA_TAP || event == FA_REPEAT) { /* one frame, with what was pressed meanwhile */
      fa_release_buffered_input();
      break;
    }
    fa_buffer_input();
    if (menu_current) {
      watches_sync();
      menu_draw();
    }
    watches_frame(menu_current != 0);
    gfx_frame();
    settings_frame(menu_current != 0);
    settings_log_frame();
    /* does the picture still get presented? */
    if (!logged && sys_tick() - paused_at > sys_tick_freq()) {
      logged = true;
      log_printf("frame advance: %u presents in the first second paused",
                 (unsigned)(gfx_presents - presents_at_start));
    }
  }
}

static void fa_frame(void)
{
  if (!frame_advance) {
    fa_paused = false;
    fa_button_down = false;
    gfx_pause_icon = 0;
    input_force_active = false;
    return;
  }
  input_hide_from_game |= fa_button_bit();
  if (!fa_paused)
    input_force_active = false;
  if (!fa_paused && fa_read_button() == FA_TAP) {
    fa_paused = true;
    gfx_notice(frame_advance_button ? "frame advance: R a frame, R + L3 to run"
                                    : "frame advance: L a frame, L + L3 to run");
    /* The game shows nothing new while it's held, so this frame is let through first, with the pause
     * icon on it: the screen shows the game is paused (and hasn't crashed). Held from the next one. */
    gfx_pause_icon = pause_display ? 1 : 0;
    return;
  }
  if (fa_paused)
    fa_hold_game();
}

/* --- frame hook -------------------------------------------------------------------------- */

static void first_frame(void)
{
  menus_build();
  settings_load();
  gamefont_load(); /* the game's type for the panels (nativeui.c); gz's pixel font if it can't */
  uishader_load(); /* the game's UI shader, to draw them on the GPU (gpuui.c) */
  log_printf("running on game %s (patch v%d, main %#lx)", z64hd_game->version,
             OOTGZ_PATCH_VERSION, (unsigned long)z64hd_main);
  menus_splash();
}

static void frame(void)
{
  if (!started) {
    started = true;
    first_frame();
  }
  frames_run++;
  sys_frame();
  input_update();

  fa_menu_was_open = menu_current != 0; /* an L that closes the menu isn't frame advance's */
  menu_controls();
  if (menu_current) {
    watches_sync();
    menu_draw();
  }
  bool overlays_on_gpu = overlayui_on_gpu(); /* lines and bars for the GPU, else rectangles */
  airwall_frame(overlays_on_gpu);
  enemyhp_frame(overlays_on_gpu);
  menus_research_frame();
  watches_frame(menu_current != 0);
  levitate_frame(menu_current != 0);
  position_frame(menu_current != 0);
  lock_frame();
  warp_frame();
  age_frame();
  gfx_frame();

  cheats_frame();
  pouch_frame();
  job_frame();
  if (gfx_style == GFX_STYLE_GAME)
    gameimage_frame(); /* the game's pictures (its controllers, its menus' parts), a little each frame */
  settings_frame(menu_current != 0);
  settings_log_frame();
  fa_frame(); /* last: the frame's work is done before the game is held */
}

/* Called by the IPS stub instead of the GameData unlock (x0 = the lock), on the game's thread.
 * Unlock first, exactly as the site did, then do our work. */
__attribute__((used)) uint64_t ootgz_frame_entry(void *lock)
{
  uint64_t result = MAIN_FN(gd_unlock_t, z64hd_game->gd_unlock)(lock);
  frame();
  return result;
}

/* --- module init (DT_INIT) --------------------------------------------------------------- */

#define A64_B_OPCODE     0x14000000u /* B <label> */
#define A64_B_IMM26_MASK 0x3FFFFFF   /* word offset, 26 bits */

/* The AArch64 "b" instruction at `from` that jumps to `to`. */
static uint32_t encode_branch(uintptr_t from, uintptr_t to)
{
  return A64_B_OPCODE | (uint32_t)(((intptr_t)(to - from) >> 2) & A64_B_IMM26_MASK);
}

/* nnMain is resolved by rtld. Check the matching main header before reading any game offsets. */
static const struct z64hd_build *detect_build(uintptr_t nnmain)
{
  for (size_t i = 0; i < sizeof(z64hd_builds) / sizeof(*z64hd_builds); i++) {
    const struct z64hd_build *build = &z64hd_builds[i];
    uintptr_t base = nnmain - build->nnmain;
    uint32_t mod0, magic;
    if (sys_read32(base + 4, &mod0) && mod0 == build->mod0 &&
        sys_read32(base + mod0, &magic) && magic == 0x30444F4D) /* MOD0 */
      return build;
  }
  return 0;
}

/* Runs inside rtld's DT_INIT: no SDK calls, just checked reads and the slot. */
static const char *patch_problem(void)
{
  const uint8_t *cave = (const uint8_t *)MAIN(z64hd_game->cave_start);
  if (!sys_mapped((uintptr_t)cave, 16, SYS_PERM_R))
    return "patch marker is not mapped";
  const char *marker = OOTGZ_MARKER;
  for (int i = 0; i < 8; i++)
    if (cave[i] != (uint8_t)marker[i])
      return "no ootgz patch in main: install the .ips next to subsdk9";
  if (*(const uint32_t *)(cave + 8) != OOTGZ_PATCH_VERSION ||
      *(const uint32_t *)(cave + 12) != z64hd_game->revision)
    return "patch version or game revision mismatch: install the matching .ips";
  uintptr_t site_addr = MAIN(z64hd_game->frame_site);
  uintptr_t stub = MAIN(z64hd_game->cave_start + OOTGZ_STUB_OFF);
  uint32_t site;
  if (!sys_read32(site_addr, &site) || site != encode_branch(site_addr, stub))
    return "frame site not patched (another mod on the same site, e.g. ootmenu?)";
  uintptr_t slot = MAIN(z64hd_game->hook_slot);
  if (!sys_mapped(slot, sizeof(uintptr_t), SYS_PERM_R | SYS_PERM_W) ||
      *(volatile uintptr_t *)slot != 0)
    return "hook slot in main's bss is unavailable";
  return 0;
}

__attribute__((used)) void ootgz_module_init(void)
{
  z64hd_game = detect_build((uintptr_t)&nnMain);
  if (!z64hd_game) {
    log_raw("unsupported Ocarina of Time build");
    return;
  }
  z64hd_main = (uintptr_t)&nnMain - z64hd_game->nnmain;
  const char *problem = patch_problem();
  if (problem) {
    log_raw(problem);
    return;
  }
  *(volatile uintptr_t *)MAIN(z64hd_game->hook_slot) = (uintptr_t)ootgz_frame_entry;
  log_raw(z64hd_game->version);
  log_raw("frame hook installed");

  /* main's imports: NVN for drawing, nn::hid to hide input while the menu is open */
  const char *error = got_hook(z64hd_main, "nvnBootstrapLoader", nvn_bootstrap_hook);
  log_raw(error ? "NVN hook failed" : "NVN hook installed");
  if (error)
    log_raw(error);
  error = input_hook_game(z64hd_main);
  log_raw(error ? "input hook failed" : "input hook installed");
  if (error)
    log_raw(error);
}
