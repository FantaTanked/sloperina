#include "watches.h"

#include "airwall.h"
#include "gamedata.h"
#include "gfx.h"
#include "input.h"
#include "lang.h"
#include "link.h"
#include "sys.h"
#include "z64hd.h"

float atan2f(float y, float x); /* from sdk */
float sqrtf(float x);
float hypotf(float x, float y);

/* --- the watch list -------------------------------------------------------------------------- */

const char *const watch_labels[W_COUNT] = {
  "x", "y", "z", "facing", "move angle", "speed", "y speed", "camera angle", "camera pitch",
  "camera dist", "input", "stick", "lag", "timer", "frame", "map", "air wall", "JS Height",
};
const char *const watches_corner_names[GFX_NPLACES] = { "top-left", "top-right", "bottom-left", "bottom-right",
                                                       "top-centre", "bottom-centre", "custom" };

bool watches_shown = true;
watch_entry_t watch_list[WATCH_MAX] = {
  { .id = W_X }, { .id = W_Y }, { .id = W_Z }, { .id = W_FACING }, { .id = W_SPEED }, { .id = W_CAMERA_ANGLE },
  { .id = W_JS_HEIGHT },
};
int watch_count = 7;
uint32_t watch_version;

const char *const memory_type_names[MW_NTYPES] = { "u8", "s8", "x8", "u16", "s16", "x16", "u32", "s32",
                                                   "x32", "f32", "x64" };
const uint8_t memory_type_size[MW_NTYPES] = { 1, 1, 1, 2, 2, 2, 4, 4, 4, 4, 8 };

bool watch_has(watch_t watch)
{
  for (int i = 0; i < watch_count; i++)
    if (watch_list[i].id == (int)watch)
      return true;
  return false;
}

bool watch_add(watch_t watch)
{
  if (watch < 0 || watch >= W_COUNT || watch_has(watch) || watch_count >= WATCH_MAX)
    return false;
  watch_list[watch_count++] = (watch_entry_t){ .id = (int16_t)watch };
  watch_version++;
  return true;
}

bool watch_add_memory(uintptr_t addr, int type)
{
  for (int i = 0; i < watch_count; i++)
    if (watch_list[i].id == W_MEMORY && watch_list[i].addr == addr)
      return false;
  if (watch_count >= WATCH_MAX || type < 0 || type >= MW_NTYPES)
    return false;
  watch_list[watch_count++] = (watch_entry_t){ .id = W_MEMORY, .type = (uint8_t)type, .addr = addr };
  watch_version++;
  return true;
}

void watch_remove(int index)
{
  if (index < 0 || index >= watch_count)
    return;
  for (int i = index; i + 1 < watch_count; i++)
    watch_list[i] = watch_list[i + 1];
  watch_count--;
  watch_version++;
}

void watch_set_list(const watch_t *list, int n)
{
  watch_count = 0;
  for (int i = 0; i < n && watch_count < WATCH_MAX; i++)
    if (list[i] >= 0 && list[i] < W_COUNT && !watch_has(list[i]))
      watch_list[watch_count++] = (watch_entry_t){ .id = (int16_t)list[i] };
  /* Permanent built-in, including for old settings. Existing entries keep their order;
   * settings.c restores labels/visibility afterwards. Hide it to keep it off across boots. */
  watch_add(W_JS_HEIGHT);
  watch_version++;
}

/* --- settings -------------------------------------------------------------------------------- */

enum { INPUT_STYLE_ROW, INPUT_STYLE_CONTROLLER }; /* input_style */

int watches_corner = GFX_BOTTOM_LEFT;
int watches_x = 30, watches_y = 600;
bool watches_moving;
bool input_display;
bool input_display_bg = true;
bool input_show_left = true, input_show_right = true, input_show_buttons = true;
int input_style = INPUT_STYLE_CONTROLLER;
int input_size;

/* --- input display --------------------------------------------------------------------------- */

#define STICK_MAX 32767   /* a stick's raw value at full tilt */
#define FILLER_CHAR 0x7F  /* drawn as nothing, but counts as content: the panel keeps its size */

static const char filler[2] = { FILLER_CHAR, 0 };

/* gfx_input_pad's `clicks` bits: the sticks pressed in, and the d-pad arms held */
enum {
  PAD_LSTICK = 1 << 0,
  PAD_RSTICK = 1 << 1,
  PAD_DUP = 1 << 2,
  PAD_DDOWN = 1 << 3,
  PAD_DLEFT = 1 << 4,
  PAD_DRIGHT = 1 << 5,
};

/* the controller style's grid (columns and rows of text) */
#define PAD_READOUT_ROW 9        /* the stick readouts, under the drawing */
#define PAD_LEFT_READOUT_COL 1
#define PAD_RIGHT_READOUT_COL 11
#define PAD_LAST_COL 19

#define ROW_STICK_SLOT_W 10 /* the row style: each stick's "xxxx yyyy" and a space */

/* the controller: each button's text where it is on a Switch 2 Pro Controller seen from above
 * (nvn.c draws the body and the sticks around them) */
static const struct { uint32_t bit; int col, row; const char *name; } pad_buttons[] = {
  { BTN_ZL, 1, 0, "ZL" }, { BTN_L, 4, 0, "L" }, { BTN_R, 16, 0, "R" }, { BTN_ZR, 18, 0, "ZR" },
  { BTN_MINUS, 8, 2, "-" }, { BTN_PLUS, 12, 2, "+" },
  { BTN_X, 16, 2, "X" }, { BTN_Y, 14, 3, "Y" }, { BTN_A, 18, 3, "A" }, { BTN_B, 16, 4, "B" },
};

/* a stick's raw value as -100..100 */
static int8_t stick_percent(int32_t raw)
{
  int percent = (int)(raw * 100 / STICK_MAX);
  if (percent < -100)
    percent = -100;
  else if (percent > 100)
    percent = 100;
  return (int8_t)percent;
}

static void controller_frame(void)
{
  int32_t left_x, left_y, right_x, right_y;
  input_sticks(&left_x, &left_y, &right_x, &right_y);
  uint32_t held = input_held();
  int8_t sticks[4] = { stick_percent(left_x), stick_percent(left_y), stick_percent(right_x),
                       stick_percent(right_y) };
  if (!input_show_left) {
    sticks[0] = 0;
    sticks[1] = 0;
  }
  if (!input_show_right) {
    sticks[2] = 0;
    sticks[3] = 0;
  }
  uint8_t clicks = 0;
  if (held & BTN_LSTICK)
    clicks |= PAD_LSTICK;
  if (held & BTN_RSTICK)
    clicks |= PAD_RSTICK;
  if (input_show_buttons) { /* the d-pad is drawn (nvn.c), its arms filled while held */
    if (held & BTN_DUP)
      clicks |= PAD_DUP;
    if (held & BTN_DDOWN)
      clicks |= PAD_DDOWN;
    if (held & BTN_DLEFT)
      clicks |= PAD_DLEFT;
    if (held & BTN_DRIGHT)
      clicks |= PAD_DRIGHT;
  }
  gfx_input_pad(sticks, clicks);
  if (input_show_buttons) {
    for (unsigned i = 0; i < sizeof(pad_buttons) / sizeof(*pad_buttons); i++) {
      gfx_style_t style = held & pad_buttons[i].bit ? GFX_NORMAL : GFX_FAINT;
      gfx_input(pad_buttons[i].col, pad_buttons[i].row, style, pad_buttons[i].name);
    }
  }
  /* the readouts under it */
  char readout[16];
  if (input_show_left) {
    snprintf(readout, sizeof(readout), "L%4d%4d", sticks[0], sticks[1]);
    gfx_input(PAD_LEFT_READOUT_COL, PAD_READOUT_ROW, GFX_LOG, readout);
  }
  if (input_show_right) {
    snprintf(readout, sizeof(readout), "R%4d%4d", sticks[2], sticks[3]);
    gfx_input(PAD_RIGHT_READOUT_COL, PAD_READOUT_ROW, GFX_LOG, readout);
  }
  /* fillers in opposite corners: the panel keeps its size */
  gfx_input(0, 0, GFX_NORMAL, filler);
  gfx_input(PAD_LAST_COL, PAD_READOUT_ROW, GFX_NORMAL, filler);
}

/* gz's input display: the sticks (-100..100) in gz's grey, then every button in a slot of its own
 * (so nothing shifts), shown while it's held */
static const struct { uint32_t bit; const char *name; } input_slots[] = {
  { BTN_A, "A" }, { BTN_B, "B" }, { BTN_X, "X" }, { BTN_Y, "Y" }, { BTN_L, "L" }, { BTN_R, "R" },
  { BTN_ZL, "ZL" }, { BTN_ZR, "ZR" }, { BTN_PLUS, "+" }, { BTN_MINUS, "-" }, { BTN_LSTICK, "L3" },
  { BTN_RSTICK, "R3" }, { BTN_DUP, "\x07" }, { BTN_DDOWN, "\x08" }, { BTN_DLEFT, "\x0C" }, { BTN_DRIGHT, "\x0B" },
};

static void input_row_frame(void)
{
  int32_t left_x, left_y, right_x, right_y;
  input_sticks(&left_x, &left_y, &right_x, &right_y);
  char stick[16];
  int col = 0;
  /* each part only if it's on; each in a slot of fixed width */
  if (input_show_left) {
    snprintf(stick, sizeof(stick), "%4d %4d", (int)(left_x * 100 / STICK_MAX), (int)(left_y * 100 / STICK_MAX));
    gfx_input(col, 0, GFX_LOG, stick);
    col += ROW_STICK_SLOT_W;
  }
  if (input_show_right) {
    snprintf(stick, sizeof(stick), "%4d %4d", (int)(right_x * 100 / STICK_MAX), (int)(right_y * 100 / STICK_MAX));
    gfx_input(col, 0, GFX_LOG, stick);
    col += ROW_STICK_SLOT_W;
  }
  if (input_show_buttons) {
    uint32_t held = input_held();
    for (unsigned i = 0; i < sizeof(input_slots) / sizeof(*input_slots); i++) {
      if (held & input_slots[i].bit)
        gfx_input(col, 0, GFX_NORMAL, input_slots[i].name);
      col += (int)strlen(input_slots[i].name) + 1;
    }
  }
  if (col) /* a filler at the end: the panel keeps one width */
    gfx_input(col - 1, 0, GFX_NORMAL, filler);
}

static void input_display_frame(bool menu_open)
{
  if (!input_display || menu_open) /* hidden while the menu is open */
    return;
  gfx_input_background(input_display_bg);
  static const uint8_t size_percent[3] = { 50, 75, 100 }; /* input_size: small, medium, large */
  gfx_input_scale(size_percent[input_size >= 0 && input_size < 3 ? input_size : 0]);
  if (input_style == INPUT_STYLE_CONTROLLER)
    controller_frame();
  else
    input_row_frame();
}

/* Link object fields (oot26-tools, game.py): facing cos/sin, velocity (units/s). */
#define LINK_FACING_COS 0x328
#define LINK_FACING_SIN 0x330
#define LINK_VELOCITY 0x388
/* JS Height: the player object is [[link + 0x270] + 0x358]; the float at +0x18E8 is FLT_MAX after a
 * load and each jump sets it to Link's drawn y at takeoff + 0.7 (measured live in v131072). The
 * jumpslash launch is said to depend on it (Link must be below it). */
#define LINK_PLAYER_PTR 0x270
#define PLAYER_PTR 0x358
#define PLAYER_JS_HEIGHT 0x18E8

/* the camera (link.h): position, then look-at, 3 floats each */
#define CAMERA_POS 0x38
#define CAMERA_AT 0x44

/* the display object (Z64HD_DISP_PTR, two pointers deep): Link's drawn position, 3 floats */
#define DISP_POSITION 0x29C

static const float DEGREES_PER_RADIAN = 57.29577951f;

/* --- timer and lag counter (gz: lag = 60 Hz ticks of real time minus game frames) -------- */

#define LAG_TICK_HZ 60

static bool timer_running;
static uint64_t timer_total, timer_since; /* sys_tick()s */
static uint64_t lag_start_tick;
static uint32_t lag_start_frame;
static bool lag_started;

void timer_toggle(void)
{
  uint64_t now = sys_tick();
  if (timer_running)
    timer_total += now - timer_since;
  timer_running = !timer_running;
  timer_since = now;
}

void timer_reset(void)
{
  timer_total = 0;
  timer_since = sys_tick();
}

void lag_reset(void)
{
  lag_started = false;
}

/* --- measured motion ----------------------------------------------------------------------------
 * Speed from how far Link's drawn position actually moves each frame, not the game's own velocity,
 * so it includes whatever else moves him (2x movement speed, levitate). Smoothed a little. */

#define MOTION_MAX_GAP_S 0.2   /* a longer gap between frames (a pause, loading) isn't measured */
#define MOTION_MAX_STEP_SQ 25.0 /* a step over 5 units (squared) in a frame is a warp, not movement */
#define MOTION_SMOOTHING 0.35  /* how far each frame moves the speed towards the new measurement */
#define MOTION_MIN_SPEED 0.05  /* slower than this, the move angle is meaningless */

static vec3f_t motion_last_pos;
static bool motion_known;
static uint64_t motion_last_tick;
static double motion_speed, motion_vx, motion_vz;

static void measure_motion(void)
{
  vec3f_t pos;
  uint64_t now = sys_tick();
  if (!link_drawn_pos(&pos)) {
    motion_known = false;
    return;
  }
  if (motion_known) {
    double dt = (double)(now - motion_last_tick) / (double)sys_tick_freq();
    double dx = pos.x - motion_last_pos.x;
    double dz = pos.z - motion_last_pos.z;
    if (dt > 0 && dt < MOTION_MAX_GAP_S && dx * dx + dz * dz < MOTION_MAX_STEP_SQ) {
      double vx = dx / dt;
      double vz = dz / dt;
      motion_vx += MOTION_SMOOTHING * (vx - motion_vx);
      motion_vz += MOTION_SMOOTHING * (vz - motion_vz);
      motion_speed = hypotf((float)motion_vx, (float)motion_vz);
    }
  }
  motion_last_pos = pos;
  motion_last_tick = now;
  motion_known = true;
}

/* --- values ---------------------------------------------------------------------------------- */

/* the direction of (x, y) in degrees, 0..360 */
static float direction_degrees(float y, float x)
{
  float angle = atan2f(y, x) * DEGREES_PER_RADIAN;
  return angle < 0 ? angle + 360.0f : angle;
}

static bool link_motion(float *facing_cos, float *facing_sin, float velocity[3])
{
  uintptr_t link = link_actor();
  if (!link || !sys_mapped(link + LINK_FACING_COS, LINK_VELOCITY + 12 - LINK_FACING_COS, SYS_PERM_R))
    return false;
  *facing_cos = *(float *)(link + LINK_FACING_COS);
  *facing_sin = *(float *)(link + LINK_FACING_SIN);
  for (int i = 0; i < 3; i++)
    velocity[i] = ((float *)(link + LINK_VELOCITY))[i];
  return true;
}

/* where the JS Height float is, or 0 (the wrench, and its reset) */
static uintptr_t js_height_addr(void)
{
  uintptr_t o = link_actor();
  uintptr_t q = o ? sys_ptr(o + LINK_PLAYER_PTR) : 0;
  uintptr_t p = q ? sys_ptr(q + PLAYER_PTR) : 0;
  return p && sys_mapped(p + PLAYER_JS_HEIGHT, 4, SYS_PERM_R) ? p + PLAYER_JS_HEIGHT : 0;
}

bool js_height_reset(void)
{
  uintptr_t a = js_height_addr();
  if (!a || !sys_mapped(a, 4, SYS_PERM_W))
    return false;
  *(volatile uint32_t *)a = 0x7F7FFFFFu; /* FLT_MAX */
  return true;
}

static bool camera_look(float pos[3], float at[3])
{
  uintptr_t cam = camera();
  if (!cam || !sys_mapped(cam + CAMERA_POS, CAMERA_AT + 12 - CAMERA_POS, SYS_PERM_R))
    return false;
  for (int i = 0; i < 3; i++) {
    pos[i] = ((float *)(cam + CAMERA_POS))[i];
    at[i] = ((float *)(cam + CAMERA_AT))[i];
  }
  return true;
}

/* Read-only: resolve both 64-bit pointers afresh, including after scene changes.
 * sys_ptr/sys_read32 check alignment and readable mappings before dereferencing.
 * Guard the offset additions too, so a corrupt pointer cannot wrap to another mapping. */
static bool js_height(float *out)
{
  uintptr_t p = link_actor();
  if (!p || p > UINTPTR_MAX - 0x270)
    return false;
  p = sys_ptr(p + 0x270);
  if (!p || p > UINTPTR_MAX - 0x358)
    return false;
  p = sys_ptr(p + 0x358);
  if (!p || p > UINTPTR_MAX - 0x18E8)
    return false;
  uint32_t bits;
  if (!sys_read32(p + 0x18E8, &bits))
    return false;
  memcpy(out, &bits, sizeof(*out));
  return true;
}

static const struct { uint32_t bit; const char *name; } button_names[] = {
  { BTN_A, "A" }, { BTN_B, "B" }, { BTN_X, "X" }, { BTN_Y, "Y" }, { BTN_L, "L" }, { BTN_R, "R" },
  { BTN_ZL, "ZL" }, { BTN_ZR, "ZR" }, { BTN_PLUS, "+" }, { BTN_MINUS, "-" }, { BTN_LSTICK, "L3" },
  { BTN_RSTICK, "R3" }, { BTN_DUP, "Du" }, { BTN_DDOWN, "Dd" }, { BTN_DLEFT, "Dl" }, { BTN_DRIGHT, "Dr" },
};

/* --- display types (gz's watch types, per watch: what suits its value) ---------------------- */

typedef enum { DK_TEXT, DK_FLOAT, DK_ANGLE, DK_COUNT, DK_TIMER, DK_STICK, DK_F32 } dkind_t;

static const dkind_t kind_of[W_COUNT] = {
  [W_X] = DK_FLOAT, [W_Y] = DK_FLOAT, [W_Z] = DK_FLOAT, [W_FACING] = DK_ANGLE, [W_MOVE_ANGLE] = DK_ANGLE,
  [W_SPEED] = DK_FLOAT, [W_Y_SPEED] = DK_FLOAT, [W_CAMERA_ANGLE] = DK_ANGLE, [W_CAMERA_PITCH] = DK_ANGLE,
  [W_CAMERA_DIST] = DK_FLOAT, [W_INPUT] = DK_TEXT, [W_STICK] = DK_STICK, [W_LAG] = DK_COUNT,
  [W_TIMER] = DK_TIMER, [W_FRAME] = DK_COUNT, [W_MAP] = DK_TEXT, [W_AIRWALL] = DK_TEXT,
  [W_JS_HEIGHT] = DK_F32,
};

/* Each kind's types, in the order of its names below. A float: decimal (f32), whole number (s32) or the
 * float's bits (x32). An angle: degrees, the game's 16-bit angle (0x4000 = 90 degrees) as N64 OoT shows
 * it, the same signed, or radians. */
enum { FLOAT_AS_F32, FLOAT_AS_S32, FLOAT_AS_X32, N_FLOAT_TYPES };
enum { ANGLE_AS_DEG, ANGLE_AS_HEX, ANGLE_AS_S16, ANGLE_AS_RAD, N_ANGLE_TYPES };
enum { COUNT_AS_DEC, COUNT_AS_HEX, N_COUNT_TYPES };
enum { TIMER_AS_MIN_SEC, TIMER_AS_SEC, N_TIMER_TYPES };
enum { STICK_AS_PERCENT, STICK_AS_RAW, N_STICK_TYPES };

static const char *const names_float[N_FLOAT_TYPES] = { "f32", "s32", "x32" };
static const char *const names_angle[N_ANGLE_TYPES] = { "deg", "hex", "s16", "rad" };
static const char *const names_count[N_COUNT_TYPES] = { "dec", "hex" };
static const char *const names_timer[N_TIMER_TYPES] = { "m:s", "sec" };
static const char *const names_stick[N_STICK_TYPES] = { "%", "raw" };

#define ANGLE_UNITS_PER_TURN 65536.0f /* the game's binary angles */

uint8_t watch_types[W_COUNT];

const char *const *watch_type_names(watch_t watch, int *n)
{
  switch (watch >= 0 && watch < W_COUNT ? kind_of[watch] : DK_TEXT) {
    case DK_F32: *n = 1; return names_float; /* f32 only; FLT_MAX must not be cast to s32 */
    case DK_FLOAT: *n = N_FLOAT_TYPES; return names_float;
    case DK_ANGLE: *n = N_ANGLE_TYPES; return names_angle;
    case DK_COUNT: *n = N_COUNT_TYPES; return names_count;
    case DK_TIMER: *n = N_TIMER_TYPES; return names_timer;
    case DK_STICK: *n = N_STICK_TYPES; return names_stick;
    default: *n = 0; return 0;
  }
}

void watch_set_type(watch_t watch, int type)
{
  int n_types;
  watch_type_names(watch, &n_types);
  if (watch >= 0 && watch < W_COUNT && n_types > 0) {
    watch_types[watch] = (uint8_t)(type < 0 ? 0 : type % n_types);
    watch_version++;
  }
}

/* the watch's display type, or 0 (the first) if it has none or the setting is out of range */
static int type_of(watch_t watch)
{
  int n_types;
  watch_type_names(watch, &n_types);
  return n_types && watch_types[watch] < n_types ? watch_types[watch] : 0;
}

static void float_text(float value, int type, char *buf, int len)
{
  if (type == FLOAT_AS_S32) {
    snprintf(buf, len, "%d", (int)(value < 0 ? value - 0.5f : value + 0.5f)); /* rounded */
  } else if (type == FLOAT_AS_X32) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    snprintf(buf, len, "0x%08x", (unsigned)bits);
  } else {
    snprintf(buf, len, "%.2f", (double)value);
  }
}

static void angle_text(float degrees, int type, char *buf, int len)
{
  int binary = (int)(degrees / 360.0f * ANGLE_UNITS_PER_TURN + (degrees < 0 ? -0.5f : 0.5f)); /* rounded */
  switch (type) {
    case ANGLE_AS_HEX: snprintf(buf, len, "0x%04x", (unsigned)(binary & 0xFFFF)); break;
    case ANGLE_AS_S16: snprintf(buf, len, "%d", (int)(int16_t)(binary & 0xFFFF)); break;
    case ANGLE_AS_RAD: snprintf(buf, len, "%.3f", (double)(degrees / DEGREES_PER_RADIAN)); break;
    default: snprintf(buf, len, "%.1f", (double)degrees); break;
  }
}

static void count_text(int64_t count, int type, char *buf, int len)
{
  if (type == COUNT_AS_HEX)
    snprintf(buf, len, "0x%x", (unsigned)(uint32_t)count);
  else
    snprintf(buf, len, "%lld", (long long)count);
}

/* the buttons held, by name ("." for none) */
static void input_text(char *buf, int len)
{
  int used = 0;
  buf[0] = 0;
  for (unsigned i = 0; i < sizeof(button_names) / sizeof(*button_names); i++)
    if (input_held() & button_names[i].bit)
      used += snprintf(buf + used, used < len ? len - used : 0, "%s%s", used ? " " : "", button_names[i].name);
  if (!used)
    snprintf(buf, len, ".");
}

static void stick_text(int type, char *buf, int len)
{
  int32_t left_x, left_y, right_x, right_y;
  input_sticks(&left_x, &left_y, &right_x, &right_y);
  if (type == STICK_AS_RAW)
    snprintf(buf, len, "%+6d %+6d", (int)left_x, (int)left_y);
  else
    snprintf(buf, len, "%+4d %+4d", (int)(left_x * 100 / STICK_MAX), (int)(left_y * 100 / STICK_MAX));
}

static void timer_text(int type, char *buf, int len)
{
  uint64_t ticks = timer_total + (timer_running ? sys_tick() - timer_since : 0);
  uint64_t ms = ticks * 1000 / sys_tick_freq();
  if (type == TIMER_AS_SEC)
    snprintf(buf, len, "%u.%03u%s", (unsigned)(ms / 1000), (unsigned)(ms % 1000), timer_running ? "" : " (stopped)");
  else
    snprintf(buf, len, "%u:%02u.%03u%s", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60),
             (unsigned)(ms % 1000), timer_running ? "" : " (stopped)");
}

/* lag: 60 Hz ticks of real time since the counter started, minus the game's frames */
static void lag_text(int type, char *buf, int len)
{
  uint64_t ticks = sys_tick() - lag_start_tick;
  int64_t real_frames = (int64_t)(ticks * LAG_TICK_HZ / sys_tick_freq());
  int64_t game_frames = (int64_t)(frame_counter() - lag_start_frame);
  count_text(real_frames - game_frames, type, buf, len);
}

void watch_text(watch_t watch, char *buf, int len)
{
  vec3f_t pos;
  float facing_cos, facing_sin, velocity[3], cam_pos[3], cam_at[3];
  int type = type_of(watch);
  snprintf(buf, len, "-"); /* unless there's a value */
  switch (watch) {
    case W_X: case W_Y: case W_Z:
      if (link_drawn_pos(&pos))
        float_text(watch == W_X ? pos.x : watch == W_Y ? pos.y : pos.z, type, buf, len);
      break;
    case W_FACING:
      if (link_motion(&facing_cos, &facing_sin, velocity))
        angle_text(direction_degrees(facing_sin, facing_cos), type, buf, len);
      break;
    case W_MOVE_ANGLE:
      if (motion_known && motion_speed > MOTION_MIN_SPEED)
        angle_text(direction_degrees((float)motion_vx, (float)motion_vz), type, buf, len);
      break;
    case W_SPEED:
      if (motion_known)
        float_text((float)motion_speed, type, buf, len);
      break;
    case W_Y_SPEED:
      if (link_motion(&facing_cos, &facing_sin, velocity))
        float_text(velocity[1], type, buf, len);
      break;
    case W_CAMERA_ANGLE:
      if (camera_look(cam_pos, cam_at))
        angle_text(direction_degrees(cam_at[0] - cam_pos[0], cam_at[2] - cam_pos[2]), type, buf, len);
      break;
    case W_CAMERA_PITCH:
      if (camera_look(cam_pos, cam_at)) {
        float rise = cam_at[1] - cam_pos[1];
        float run = hypotf(cam_at[0] - cam_pos[0], cam_at[2] - cam_pos[2]);
        angle_text(atan2f(rise, run) * DEGREES_PER_RADIAN, type, buf, len);
      }
      break;
    case W_CAMERA_DIST:
      if (camera_look(cam_pos, cam_at) && link_drawn_pos(&pos)) {
        float dx = cam_pos[0] - pos.x;
        float dy = cam_pos[1] - pos.y;
        float dz = cam_pos[2] - pos.z;
        float_text(sqrtf(dx * dx + dy * dy + dz * dz), type, buf, len);
      }
      break;
    case W_INPUT:
      input_text(buf, len);
      break;
    case W_STICK:
      stick_text(type, buf, len);
      break;
    case W_LAG:
      if (lag_started)
        lag_text(type, buf, len);
      break;
    case W_TIMER:
      timer_text(type, buf, len);
      break;
    case W_FRAME:
      count_text(frame_counter(), type, buf, len);
      break;
    case W_MAP: {
      const char *map = gd_get_str(GD_STR_MAP);
      if (map)
        snprintf(buf, len, "%s", map);
      break;
    }
    case W_AIRWALL:
      airwall_link_text(buf, len);
      break;
    case W_JS_HEIGHT: {
      float height;
      if (js_height(&height))
        snprintf(buf, len, "%.9g", (double)height); /* full f32 precision, compact even at FLT_MAX */
      else
        snprintf(buf, len, "N/A");
      break;
    }
    default:
      break;
  }
}

/* --- entries (predefined or memory) --------------------------------------------------------------- */

void watch_entry_label(const watch_entry_t *entry, char *buf, int len)
{
  if (entry->label[0])
    snprintf(buf, len, "%s", entry->label);
  else if (entry->id == W_MEMORY)
    snprintf(buf, len, "%010lx", (unsigned long)entry->addr);
  else
    snprintf(buf, len, "%s", T(watch_labels[entry->id]));
}

/* gz's watch: the value at the address, as its type; "-" if it isn't readable */
static void memory_text(const watch_entry_t *entry, char *buf, int len)
{
  int type = entry->type < MW_NTYPES ? entry->type : MW_X8;
  int size = memory_type_size[type];
  if (!sys_mapped(entry->addr, (size_t)size, SYS_PERM_R)) {
    snprintf(buf, len, "-");
    return;
  }
  uint64_t value = 0;
  memcpy(&value, (const void *)entry->addr, (size_t)size); /* little-endian */
  switch (type) {
    case MW_U8: case MW_U16: case MW_U32: snprintf(buf, len, "%llu", (unsigned long long)value); break;
    case MW_S8: snprintf(buf, len, "%d", (int)(int8_t)value); break;
    case MW_S16: snprintf(buf, len, "%d", (int)(int16_t)value); break;
    case MW_S32: snprintf(buf, len, "%d", (int)(int32_t)value); break;
    case MW_X8: snprintf(buf, len, "0x%02x", (unsigned)value); break;
    case MW_X16: snprintf(buf, len, "0x%04x", (unsigned)value); break;
    case MW_X32: snprintf(buf, len, "0x%08x", (unsigned)value); break;
    case MW_F32: {
      float f;
      uint32_t bits = (uint32_t)value;
      memcpy(&f, &bits, 4);
      snprintf(buf, len, "%g", (double)f);
      break;
    }
    default: snprintf(buf, len, "0x%010llx", (unsigned long long)value); break;
  }
}

void watch_entry_text(const watch_entry_t *entry, char *buf, int len)
{
  if (entry->id == W_MEMORY)
    memory_text(entry, buf, len);
  else
    watch_text((watch_t)entry->id, buf, len);
}

const char *const *watch_entry_types(const watch_entry_t *entry, int *n, int *current)
{
  if (entry->id == W_MEMORY) {
    *n = MW_NTYPES;
    *current = entry->type;
    return memory_type_names;
  }
  const char *const *names = watch_type_names((watch_t)entry->id, n);
  *current = *n ? watch_types[entry->id] % *n : 0;
  return names;
}

void watch_entry_set_type(int index, int type)
{
  if (index < 0 || index >= watch_count)
    return;
  watch_entry_t *entry = &watch_list[index];
  if (entry->id == W_MEMORY) {
    entry->type = (uint8_t)((type % MW_NTYPES + MW_NTYPES) % MW_NTYPES); /* wraps both ways */
    watch_version++;
  } else {
    watch_set_type((watch_t)entry->id, type);
  }
}

bool watch_entry_has_address(const watch_entry_t *entry)
{
  switch (entry->id) {
    case W_MEMORY: case W_X: case W_Y: case W_Z: case W_FACING: case W_Y_SPEED: case W_CAMERA_ANGLE:
    case W_CAMERA_PITCH: case W_CAMERA_DIST: case W_FRAME: case W_MAP: case W_JS_HEIGHT:
      return true;
    default:
      return false;
  }
}

bool watch_entry_address(const watch_entry_t *entry, uintptr_t *addr, int *mw_type)
{
  uintptr_t found = 0;
  uintptr_t base;
  int type = MW_F32;
  switch (entry->id) {
    case W_MEMORY:
      found = entry->addr;
      type = entry->type;
      break;
    case W_X: case W_Y: case W_Z: /* the drawn position (link_drawn_pos) */
      base = sys_ptr(MAIN(Z64HD_DISP_PTR));
      base = base ? sys_ptr(base) : 0;
      found = base ? base + DISP_POSITION + (uintptr_t)(entry->id - W_X) * 4 : 0;
      break;
    case W_FACING: /* cos, then sin */
      base = link_actor();
      found = base ? base + LINK_FACING_COS : 0;
      break;
    case W_Y_SPEED:
      base = link_actor();
      found = base ? base + LINK_VELOCITY + 4 : 0;
      break;
    case W_CAMERA_ANGLE: case W_CAMERA_PITCH: case W_CAMERA_DIST: /* position, then look-at */
      base = camera();
      found = base ? base + CAMERA_POS : 0;
      break;
    case W_FRAME:
      found = MAIN(Z64HD_FRAME_COUNTER);
      type = MW_U32;
      break;
    case W_MAP:
      found = (uintptr_t)gd_get_str(GD_STR_MAP);
      type = MW_X8;
      break;
    case W_JS_HEIGHT:
      found = js_height_addr();
      break;
    default:
      return false;
  }
  *addr = found;
  *mw_type = type;
  return found != 0;
}

void watch_move(int index, int dir)
{
  int other = index + dir;
  if (index < 0 || index >= watch_count || other < 0 || other >= watch_count)
    return;
  watch_entry_t swap = watch_list[index];
  watch_list[index] = watch_list[other];
  watch_list[other] = swap;
  watch_version++;
}

void watch_set_hidden(int index, bool hidden)
{
  if (index < 0 || index >= watch_count)
    return;
  watch_list[index].hidden = hidden;
  watch_version++;
}

void watch_set_label(int index, const char *label)
{
  if (index < 0 || index >= watch_count)
    return;
  snprintf(watch_list[index].label, sizeof(watch_list[index].label), "%s", label ? label : "");
  watch_version++;
}

/* --- frame --------------------------------------------------------------------------------------- */

/* gz's commands, while the menu is closed; the three-button ones first */
static void command_input(void)
{
  uint32_t held = input_held();
  if (input_combo(BTN_R | BTN_A | BTN_DLEFT)) {
    timer_toggle();
    gfx_notice("timer started/stopped");
  } else if (input_combo(BTN_R | BTN_B | BTN_DLEFT)) {
    timer_reset();
    gfx_notice("timer reset");
  } else if (input_combo(BTN_R | BTN_B | BTN_DRIGHT)) {
    lag_reset();
    gfx_notice("lag counter reset");
  } else if (input_combo(BTN_R | BTN_A | BTN_DRIGHT)) {
    gfx_notice(js_height_reset() ? "JS Height reset" : "JS Height: Link not found");
  } else if (input_combo(BTN_R | BTN_DRIGHT) && !(held & (BTN_A | BTN_B))) {
    watches_shown = !watches_shown;
    gfx_notice(watches_shown ? "watches shown" : "watches hidden");
  }
}

void watches_frame(bool menu_open)
{
  input_display_frame(menu_open);
  measure_motion();
  if (!lag_started) {
    lag_start_tick = sys_tick();
    lag_start_frame = frame_counter();
    lag_started = true;
  }
  if (!menu_open)
    command_input();
  bool moving = watches_moving; /* set each frame by the menu's move */
  watches_moving = false;
  if ((menu_open && !moving) || (!watches_shown && !moving))
    return;
  if (watches_corner == GFX_CUSTOM)
    gfx_hud_at(watches_x, watches_y);
  char line[64], value[48];
  int row = 0;
  for (int i = 0; i < watch_count; i++) {
    char label[24];
    if (watch_list[i].hidden)
      continue;
    watch_entry_label(&watch_list[i], label, sizeof(label));
    watch_entry_text(&watch_list[i], value, sizeof(value));
    snprintf(line, sizeof(line), "%-12.12s %.40s", label, value);
    gfx_hud((gfx_corner_t)watches_corner, row++, GFX_NORMAL, line);
  }
}
