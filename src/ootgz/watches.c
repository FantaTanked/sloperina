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

const char *const watch_labels[W_COUNT] = {
  "x", "y", "z", "facing", "move angle", "speed", "y speed", "camera angle", "camera pitch",
  "camera dist", "input", "stick", "lag", "timer", "frame", "map", "air wall", "jump height",
};
const char *const watches_corner_names[GFX_NPLACES] = { "top-left", "top-right", "bottom-left", "bottom-right",
                                                       "top-centre", "bottom-centre", "custom" };

bool watches_shown = true;
watch_entry_t watch_list[WATCH_MAX] = {
  { .id = W_X }, { .id = W_Y }, { .id = W_Z }, { .id = W_FACING }, { .id = W_SPEED }, { .id = W_CAMERA_ANGLE },
};
int watch_count = 6;
uint32_t watch_version;

const char *const memory_type_names[MW_NTYPES] = { "u8", "s8", "x8", "u16", "s16", "x16", "u32", "s32",
                                                   "x32", "f32", "x64" };
const uint8_t memory_type_size[MW_NTYPES] = { 1, 1, 1, 2, 2, 2, 4, 4, 4, 4, 8 };

bool watch_has(watch_t w)
{
  for (int i = 0; i < watch_count; i++)
    if (watch_list[i].id == (int)w)
      return true;
  return false;
}

bool watch_add(watch_t w)
{
  if (w < 0 || w >= W_COUNT || watch_has(w) || watch_count >= WATCH_MAX)
    return false;
  watch_list[watch_count++] = (watch_entry_t){ .id = (int16_t)w };
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
  watch_version++;
}
int watches_corner = GFX_BOTTOM_LEFT;
int watches_x = 30, watches_y = 600;
bool watches_moving;
bool input_display;
bool input_display_bg = true;
bool input_show_left = true, input_show_right = true, input_show_buttons = true;
int input_style = 1; /* controller */
int input_size;

/* the controller: each button's text where it is on a Switch 2 Pro Controller seen from above
 * (nvn.c draws the body and the sticks around them) */
static const struct { uint32_t bit; int col, row; const char *name; } pad_buttons[] = {
  { BTN_ZL, 1, 0, "ZL" }, { BTN_L, 4, 0, "L" }, { BTN_R, 16, 0, "R" }, { BTN_ZR, 18, 0, "ZR" },
  { BTN_MINUS, 8, 2, "-" }, { BTN_PLUS, 12, 2, "+" },
  { BTN_X, 16, 2, "X" }, { BTN_Y, 14, 3, "Y" }, { BTN_A, 18, 3, "A" }, { BTN_B, 16, 4, "B" },
};

static int8_t percent(int32_t v)
{
  int p = (int)(v * 100 / 32767);
  return (int8_t)(p < -100 ? -100 : p > 100 ? 100 : p);
}

static void controller_frame(void)
{
  int32_t lx, ly, rx, ry;
  input_sticks(&lx, &ly, &rx, &ry);
  uint32_t held = input_held();
  int8_t sticks[4] = { percent(lx), percent(ly), percent(rx), percent(ry) };
  if (!input_show_left)
    sticks[0] = sticks[1] = 0;
  if (!input_show_right)
    sticks[2] = sticks[3] = 0;
  uint8_t clicks = (held & BTN_LSTICK ? 1 : 0) | (held & BTN_RSTICK ? 2 : 0);
  if (input_show_buttons) /* the d-pad is drawn (nvn.c), its arms filled while held */
    clicks |= (held & BTN_DUP ? 4 : 0) | (held & BTN_DDOWN ? 8 : 0) | (held & BTN_DLEFT ? 16 : 0) |
              (held & BTN_DRIGHT ? 32 : 0);
  gfx_input_pad(sticks, clicks);
  if (input_show_buttons)
    for (unsigned i = 0; i < sizeof(pad_buttons) / sizeof(*pad_buttons); i++)
      gfx_input(pad_buttons[i].col, pad_buttons[i].row, held & pad_buttons[i].bit ? GFX_NORMAL : GFX_FAINT,
                pad_buttons[i].name);
  /* the readouts under it */
  char v[16];
  if (input_show_left) {
    snprintf(v, sizeof(v), "L%4d%4d", sticks[0], sticks[1]);
    gfx_input(1, 9, GFX_LOG, v);
  }
  if (input_show_right) {
    snprintf(v, sizeof(v), "R%4d%4d", sticks[2], sticks[3]);
    gfx_input(11, 9, GFX_LOG, v);
  }
  static const char end[2] = { 0x7F, 0 }; /* fillers: the panel keeps its size */
  gfx_input(0, 0, GFX_NORMAL, end);
  gfx_input(19, 9, GFX_NORMAL, end);
}

/* gz's input display: the sticks (-100..100) in gz's grey, then every button in a slot of its own
 * (so nothing shifts), shown while it's held */
static const struct { uint32_t bit; const char *name; } input_slots[] = {
  { BTN_A, "A" }, { BTN_B, "B" }, { BTN_X, "X" }, { BTN_Y, "Y" }, { BTN_L, "L" }, { BTN_R, "R" },
  { BTN_ZL, "ZL" }, { BTN_ZR, "ZR" }, { BTN_PLUS, "+" }, { BTN_MINUS, "-" }, { BTN_LSTICK, "L3" },
  { BTN_RSTICK, "R3" }, { BTN_DUP, "\x07" }, { BTN_DDOWN, "\x08" }, { BTN_DLEFT, "\x0C" }, { BTN_DRIGHT, "\x0B" },
};

static void input_display_frame(bool menu_open)
{
  if (!input_display || menu_open) /* hidden while the menu is open */
    return;
  gfx_input_background(input_display_bg);
  static const uint8_t pct[3] = { 50, 75, 100 };
  gfx_input_scale(pct[input_size >= 0 && input_size < 3 ? input_size : 0]);
  if (input_style == 1) {
    controller_frame();
    return;
  }
  int32_t lx, ly, rx, ry;
  input_sticks(&lx, &ly, &rx, &ry);
  char stick[16];
  int col = 0;
  /* each part only if it's on; each in a slot of fixed width */
  if (input_show_left) {
    snprintf(stick, sizeof(stick), "%4d %4d", (int)(lx * 100 / 32767), (int)(ly * 100 / 32767));
    gfx_input(col, 0, GFX_LOG, stick);
    col += 10;
  }
  if (input_show_right) {
    snprintf(stick, sizeof(stick), "%4d %4d", (int)(rx * 100 / 32767), (int)(ry * 100 / 32767));
    gfx_input(col, 0, GFX_LOG, stick);
    col += 10;
  }
  if (input_show_buttons) {
    uint32_t held = input_held();
    for (unsigned i = 0; i < sizeof(input_slots) / sizeof(*input_slots); i++) {
      if (held & input_slots[i].bit)
        gfx_input(col, 0, GFX_NORMAL, input_slots[i].name);
      col += (int)strlen(input_slots[i].name) + 1;
    }
  }
  if (col) {
    static const char end[2] = { 0x7F, 0 }; /* a filler (drawn as nothing): the panel keeps one width */
    gfx_input(col - 1, 0, GFX_NORMAL, end);
  }
}

/* Link object fields (oot26-tools, game.py): facing cos/sin, velocity (units/s). */
#define LINK_FACING_COS 0x328
#define LINK_FACING_SIN 0x330
#define LINK_VELOCITY 0x388
/* Jump height: the player object is [[link + 0x270] + 0x358]; the float at +0x18E8 is FLT_MAX after a
 * load and each jump sets it to Link's drawn y at takeoff + 0.7 (measured live in v131072). The
 * jumpslash launch is said to depend on it (Link must be below it). */
#define LINK_PLAYER_PTR 0x270
#define PLAYER_PTR 0x358
#define PLAYER_JUMP_HEIGHT 0x18E8

static const float RAD = 57.29577951f;

/* --- timer and lag counter (gz: lag = 60 Hz ticks of real time minus game frames) -------- */

static bool timer_running;
static uint64_t timer_total, timer_since; /* ticks */
static uint64_t lag_t0;
static uint32_t lag_f0;
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
static vec3f_t meas_last;
static bool meas_have;
static uint64_t meas_tick;
static double meas_speed, meas_vx, meas_vz;

static void measure_motion(void)
{
  vec3f_t p;
  uint64_t now = sys_tick();
  if (!link_drawn_pos(&p)) {
    meas_have = false;
    return;
  }
  if (meas_have) {
    double dt = (double)(now - meas_tick) / (double)sys_tick_freq();
    double dx = p.x - meas_last.x, dz = p.z - meas_last.z;
    if (dt > 0 && dt < 0.2 && dx * dx + dz * dz < 25.0) { /* not a warp */
      double vx = dx / dt, vz = dz / dt;
      meas_vx += 0.35 * (vx - meas_vx), meas_vz += 0.35 * (vz - meas_vz);
      meas_speed = hypotf((float)meas_vx, (float)meas_vz);
    }
  }
  meas_last = p, meas_tick = now, meas_have = true;
}

/* --- values ---------------------------------------------------------------------------------- */

static float deg(float y, float x)
{
  float d = atan2f(y, x) * RAD;
  return d < 0 ? d + 360.0f : d;
}

static bool link_motion(float *cos, float *sin, float vel[3])
{
  uintptr_t o = link_actor();
  if (!o || !sys_mapped(o + LINK_FACING_COS, LINK_VELOCITY + 12 - LINK_FACING_COS, SYS_PERM_R))
    return false;
  *cos = *(float *)(o + LINK_FACING_COS);
  *sin = *(float *)(o + LINK_FACING_SIN);
  for (int i = 0; i < 3; i++)
    vel[i] = ((float *)(o + LINK_VELOCITY))[i];
  return true;
}

/* where the jump height float is, or 0 */
static uintptr_t jump_height_addr(void)
{
  uintptr_t o = link_actor();
  uintptr_t q = o ? sys_ptr(o + LINK_PLAYER_PTR) : 0;
  uintptr_t p = q ? sys_ptr(q + PLAYER_PTR) : 0;
  return p && sys_mapped(p + PLAYER_JUMP_HEIGHT, 4, SYS_PERM_R) ? p + PLAYER_JUMP_HEIGHT : 0;
}

bool jump_height_reset(void)
{
  uintptr_t a = jump_height_addr();
  if (!a || !sys_mapped(a, 4, SYS_PERM_W))
    return false;
  *(volatile uint32_t *)a = 0x7F7FFFFFu; /* FLT_MAX */
  return true;
}

static bool camera_look(float pos[3], float at[3])
{
  uintptr_t c = camera();
  if (!c || !sys_mapped(c + 0x38, 24, SYS_PERM_R))
    return false;
  for (int i = 0; i < 3; i++)
    pos[i] = ((float *)(c + 0x38))[i], at[i] = ((float *)(c + 0x44))[i];
  return true;
}

static const struct { uint32_t bit; const char *name; } button_names[] = {
  { BTN_A, "A" }, { BTN_B, "B" }, { BTN_X, "X" }, { BTN_Y, "Y" }, { BTN_L, "L" }, { BTN_R, "R" },
  { BTN_ZL, "ZL" }, { BTN_ZR, "ZR" }, { BTN_PLUS, "+" }, { BTN_MINUS, "-" }, { BTN_LSTICK, "L3" },
  { BTN_RSTICK, "R3" }, { BTN_DUP, "Du" }, { BTN_DDOWN, "Dd" }, { BTN_DLEFT, "Dl" }, { BTN_DRIGHT, "Dr" },
};

/* --- display types (gz's watch types, per watch: what suits its value) ---------------------- */

typedef enum { DK_TEXT, DK_FLOAT, DK_ANGLE, DK_COUNT, DK_TIMER, DK_STICK } dkind_t;

static const dkind_t kind_of[W_COUNT] = {
  [W_X] = DK_FLOAT, [W_Y] = DK_FLOAT, [W_Z] = DK_FLOAT, [W_FACING] = DK_ANGLE, [W_MOVE_ANGLE] = DK_ANGLE,
  [W_SPEED] = DK_FLOAT, [W_Y_SPEED] = DK_FLOAT, [W_CAMERA_ANGLE] = DK_ANGLE, [W_CAMERA_PITCH] = DK_ANGLE,
  [W_CAMERA_DIST] = DK_FLOAT, [W_INPUT] = DK_TEXT, [W_STICK] = DK_STICK, [W_LAG] = DK_COUNT,
  [W_TIMER] = DK_TIMER, [W_FRAME] = DK_COUNT, [W_MAP] = DK_TEXT, [W_AIRWALL] = DK_TEXT,
  [W_JUMP_HEIGHT] = DK_FLOAT,
};

/* f32: decimal; s32: whole number; x32: the float's bits. deg: degrees; hex: the game's 16-bit angle
 * (0x4000 = 90 degrees) as N64 OoT shows it; s16: the same, signed; rad: radians. */
static const char *const names_float[] = { "f32", "s32", "x32" };
static const char *const names_angle[] = { "deg", "hex", "s16", "rad" };
static const char *const names_count[] = { "dec", "hex" };
static const char *const names_timer[] = { "m:s", "sec" };
static const char *const names_stick[] = { "%", "raw" };

uint8_t watch_types[W_COUNT];

const char *const *watch_type_names(watch_t w, int *n)
{
  switch (w >= 0 && w < W_COUNT ? kind_of[w] : DK_TEXT) {
    case DK_FLOAT: *n = 3; return names_float;
    case DK_ANGLE: *n = 4; return names_angle;
    case DK_COUNT: *n = 2; return names_count;
    case DK_TIMER: *n = 2; return names_timer;
    case DK_STICK: *n = 2; return names_stick;
    default: *n = 0; return 0;
  }
}

void watch_set_type(watch_t w, int type)
{
  int n;
  watch_type_names(w, &n);
  if (w >= 0 && w < W_COUNT && n > 0) {
    watch_types[w] = (uint8_t)(type < 0 ? 0 : type % n);
    watch_version++;
  }
}

static int type_of(watch_t w)
{
  int n;
  watch_type_names(w, &n);
  return n && watch_types[w] < n ? watch_types[w] : 0;
}

static void float_text(float f, int t, char *buf, int len)
{
  if (t == 1)
    snprintf(buf, len, "%d", (int)(f < 0 ? f - 0.5f : f + 0.5f));
  else if (t == 2) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    snprintf(buf, len, "0x%08x", (unsigned)bits);
  } else
    snprintf(buf, len, "%.2f", (double)f);
}

static void angle_text(float d, int t, char *buf, int len)
{
  int bin = (int)(d / 360.0f * 65536.0f + (d < 0 ? -0.5f : 0.5f));
  switch (t) {
    case 1: snprintf(buf, len, "0x%04x", (unsigned)(bin & 0xFFFF)); break;
    case 2: snprintf(buf, len, "%d", (int)(int16_t)(bin & 0xFFFF)); break;
    case 3: snprintf(buf, len, "%.3f", (double)(d / RAD)); break;
    default: snprintf(buf, len, "%.1f", (double)d); break;
  }
}

static void count_text(int64_t v, int t, char *buf, int len)
{
  if (t == 1)
    snprintf(buf, len, "0x%x", (unsigned)(uint32_t)v);
  else
    snprintf(buf, len, "%lld", (long long)v);
}

void watch_text(watch_t w, char *buf, int len)
{
  vec3f_t p;
  float cs, sn, v[3], cp[3], ca[3];
  int t = type_of(w);
  snprintf(buf, len, "-");
  switch (w) {
    case W_X: case W_Y: case W_Z:
      if (link_drawn_pos(&p))
        float_text(w == W_X ? p.x : w == W_Y ? p.y : p.z, t, buf, len);
      break;
    case W_FACING:
      if (link_motion(&cs, &sn, v))
        angle_text(deg(sn, cs), t, buf, len);
      break;
    case W_MOVE_ANGLE:
      if (meas_have && meas_speed > 0.05)
        angle_text(deg((float)meas_vx, (float)meas_vz), t, buf, len);
      break;
    case W_SPEED:
      if (meas_have)
        float_text((float)meas_speed, t, buf, len);
      break;
    case W_Y_SPEED:
      if (link_motion(&cs, &sn, v))
        float_text(v[1], t, buf, len);
      break;
    case W_CAMERA_ANGLE:
      if (camera_look(cp, ca))
        angle_text(deg(ca[0] - cp[0], ca[2] - cp[2]), t, buf, len);
      break;
    case W_CAMERA_PITCH:
      if (camera_look(cp, ca))
        angle_text(atan2f(ca[1] - cp[1], hypotf(ca[0] - cp[0], ca[2] - cp[2])) * RAD, t, buf, len);
      break;
    case W_CAMERA_DIST:
      if (camera_look(cp, ca) && link_drawn_pos(&p)) {
        float dx = cp[0] - p.x, dy = cp[1] - p.y, dz = cp[2] - p.z;
        float_text(sqrtf(dx * dx + dy * dy + dz * dz), t, buf, len);
      }
      break;
    case W_INPUT: {
      int n = 0;
      buf[0] = 0;
      for (unsigned i = 0; i < sizeof(button_names) / sizeof(*button_names); i++)
        if (input_held() & button_names[i].bit)
          n += snprintf(buf + n, n < len ? len - n : 0, "%s%s", n ? " " : "", button_names[i].name);
      if (!n)
        snprintf(buf, len, ".");
      break;
    }
    case W_STICK: {
      int32_t lx, ly, rx, ry;
      input_sticks(&lx, &ly, &rx, &ry);
      if (t == 1)
        snprintf(buf, len, "%+6d %+6d", (int)lx, (int)ly);
      else
        snprintf(buf, len, "%+4d %+4d", (int)(lx * 100 / 32767), (int)(ly * 100 / 32767));
      break;
    }
    case W_LAG:
      if (lag_started) {
        uint64_t ticks = sys_tick() - lag_t0;
        count_text((int64_t)(ticks * 60 / sys_tick_freq()) - (int64_t)(frame_counter() - lag_f0), t, buf, len);
      }
      break;
    case W_TIMER: {
      uint64_t tk = timer_total + (timer_running ? sys_tick() - timer_since : 0);
      uint64_t ms = tk * 1000 / sys_tick_freq();
      if (t == 1)
        snprintf(buf, len, "%u.%03u%s", (unsigned)(ms / 1000), (unsigned)(ms % 1000), timer_running ? "" : " (stopped)");
      else
        snprintf(buf, len, "%u:%02u.%03u%s", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60),
                 (unsigned)(ms % 1000), timer_running ? "" : " (stopped)");
      break;
    }
    case W_FRAME:
      count_text(frame_counter(), t, buf, len);
      break;
    case W_MAP: {
      const char *m = gd_get_str(GD_STR_MAP);
      if (m)
        snprintf(buf, len, "%s", m);
      break;
    }
    case W_AIRWALL:
      airwall_link_text(buf, len);
      break;
    case W_JUMP_HEIGHT: {
      uintptr_t a = jump_height_addr();
      if (a) {
        float f = *(volatile float *)a;
        if (f > 1e30f && t != 2) /* FLT_MAX: no jump since the load */
          snprintf(buf, len, "%s", T("none"));
        else
          float_text(f, t, buf, len);
      }
      break;
    }
    default:
      break;
  }
}

/* --- entries (predefined or memory) --------------------------------------------------------------- */

void watch_entry_label(const watch_entry_t *e, char *buf, int len)
{
  if (e->label[0])
    snprintf(buf, len, "%s", e->label);
  else if (e->id == W_MEMORY)
    snprintf(buf, len, "%010lx", (unsigned long)e->addr);
  else
    snprintf(buf, len, "%s", T(watch_labels[e->id]));
}

/* gz's watch: the value at the address, as its type; "-" if it isn't readable */
static void memory_text(const watch_entry_t *e, char *buf, int len)
{
  int t = e->type < MW_NTYPES ? e->type : MW_X8, sz = memory_type_size[t];
  if (!sys_mapped(e->addr, (size_t)sz, SYS_PERM_R)) {
    snprintf(buf, len, "-");
    return;
  }
  uint64_t v = 0;
  memcpy(&v, (const void *)e->addr, (size_t)sz); /* little-endian */
  switch (t) {
    case MW_U8: case MW_U16: case MW_U32: snprintf(buf, len, "%llu", (unsigned long long)v); break;
    case MW_S8: snprintf(buf, len, "%d", (int)(int8_t)v); break;
    case MW_S16: snprintf(buf, len, "%d", (int)(int16_t)v); break;
    case MW_S32: snprintf(buf, len, "%d", (int)(int32_t)v); break;
    case MW_X8: snprintf(buf, len, "0x%02x", (unsigned)v); break;
    case MW_X16: snprintf(buf, len, "0x%04x", (unsigned)v); break;
    case MW_X32: snprintf(buf, len, "0x%08x", (unsigned)v); break;
    case MW_F32: {
      float f;
      uint32_t b = (uint32_t)v;
      memcpy(&f, &b, 4);
      snprintf(buf, len, "%g", (double)f);
      break;
    }
    default: snprintf(buf, len, "0x%010llx", (unsigned long long)v); break;
  }
}

void watch_entry_text(const watch_entry_t *e, char *buf, int len)
{
  if (e->id == W_MEMORY)
    memory_text(e, buf, len);
  else
    watch_text((watch_t)e->id, buf, len);
}

const char *const *watch_entry_types(const watch_entry_t *e, int *n, int *current)
{
  if (e->id == W_MEMORY) {
    *n = MW_NTYPES;
    *current = e->type;
    return memory_type_names;
  }
  const char *const *names = watch_type_names((watch_t)e->id, n);
  *current = *n ? watch_types[e->id] % *n : 0;
  return names;
}

void watch_entry_set_type(int index, int type)
{
  if (index < 0 || index >= watch_count)
    return;
  watch_entry_t *e = &watch_list[index];
  if (e->id == W_MEMORY) {
    e->type = (uint8_t)((type % MW_NTYPES + MW_NTYPES) % MW_NTYPES);
    watch_version++;
  } else {
    watch_set_type((watch_t)e->id, type);
  }
}

bool watch_entry_has_address(const watch_entry_t *e)
{
  switch (e->id) {
    case W_MEMORY: case W_X: case W_Y: case W_Z: case W_FACING: case W_Y_SPEED: case W_CAMERA_ANGLE:
    case W_CAMERA_PITCH: case W_CAMERA_DIST: case W_FRAME: case W_MAP: case W_JUMP_HEIGHT:
      return true;
    default:
      return false;
  }
}

bool watch_entry_address(const watch_entry_t *e, uintptr_t *addr, int *mw_type)
{
  uintptr_t a = 0, q;
  int t = MW_F32;
  switch (e->id) {
    case W_MEMORY: a = e->addr, t = e->type; break;
    case W_X: case W_Y: case W_Z: /* the drawn position (link_drawn_pos) */
      q = sys_ptr(MAIN(Z64HD_DISP_PTR));
      q = q ? sys_ptr(q) : 0;
      a = q ? q + 0x29C + (uintptr_t)(e->id - W_X) * 4 : 0;
      break;
    case W_FACING: q = link_actor(), a = q ? q + LINK_FACING_COS : 0; break;      /* cos, then sin */
    case W_Y_SPEED: q = link_actor(), a = q ? q + LINK_VELOCITY + 4 : 0; break;
    case W_CAMERA_ANGLE: case W_CAMERA_PITCH: case W_CAMERA_DIST: /* position, then look-at */
      q = camera(), a = q ? q + 0x38 : 0;
      break;
    case W_FRAME: a = MAIN(Z64HD_FRAME_COUNTER), t = MW_U32; break;
    case W_JUMP_HEIGHT: a = jump_height_addr(); break;
    case W_MAP: a = (uintptr_t)gd_get_str(GD_STR_MAP), t = MW_X8; break;
    default: return false;
  }
  *addr = a, *mw_type = t;
  return a != 0;
}

void watch_move(int index, int dir)
{
  int j = index + dir;
  if (index < 0 || index >= watch_count || j < 0 || j >= watch_count)
    return;
  watch_entry_t t = watch_list[index];
  watch_list[index] = watch_list[j];
  watch_list[j] = t;
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

void watches_frame(bool menu_open)
{
  input_display_frame(menu_open);
  measure_motion();
  if (!lag_started) {
    lag_t0 = sys_tick();
    lag_f0 = frame_counter();
    lag_started = true;
  }
  if (!menu_open) {
    /* gz commands; the three-button ones first */
    uint32_t held = input_held();
    if (input_combo(BTN_R | BTN_A | BTN_DLEFT))
      timer_toggle(), gfx_notice("timer started/stopped");
    else if (input_combo(BTN_R | BTN_B | BTN_DLEFT))
      timer_reset(), gfx_notice("timer reset");
    else if (input_combo(BTN_R | BTN_B | BTN_DRIGHT))
      lag_reset(), gfx_notice("lag counter reset");
    else if (input_combo(BTN_R | BTN_A | BTN_DRIGHT))
      gfx_notice(jump_height_reset() ? "jump height reset" : "jump height: Link not found");
    else if (input_combo(BTN_R | BTN_DRIGHT) && !(held & (BTN_A | BTN_B)))
      watches_shown = !watches_shown, gfx_notice(watches_shown ? "watches shown" : "watches hidden");
  }
  bool moving = watches_moving; /* set each frame by the menu's move */
  watches_moving = false;
  if ((menu_open && !moving) || (!watches_shown && !moving))
    return;
  if (watches_corner == GFX_CUSTOM)
    gfx_hud_at(watches_x, watches_y);
  char line[64], val[48];
  int row = 0;
  for (int i = 0; i < watch_count; i++) {
    char label[24];
    if (watch_list[i].hidden)
      continue;
    watch_entry_label(&watch_list[i], label, sizeof(label));
    watch_entry_text(&watch_list[i], val, sizeof(val));
    snprintf(line, sizeof(line), "%-12.12s %.40s", label, val);
    gfx_hud((gfx_corner_t)watches_corner, row++, GFX_NORMAL, line);
  }
}
