#include "input.h"

#include "got.h"
#include "sys.h"

/* nn::hid state structs (Switch 1 layout: sampling number, buttons, two sticks, attributes;
 * 0x28 bytes). We only touch the leading fields and read one state into a larger buffer, so
 * a longer Switch 2 struct is harmless. VERIFY the field order on hardware (BUILDING.md). */
typedef struct {
  int64_t  sampling_number;
  uint64_t buttons;
  int32_t  lx, ly, rx, ry;
  uint32_t attributes;
} npad_state_t;

#define NPAD_STATE_SIZE      0x28  /* the Switch 1 state's size: the stride between states */
#define NPAD_STATE_BUF       0x100 /* room for one state, however long the Switch 2 one is */
#define NPAD_ATTR_CONNECTED  1u    /* NpadAttribute::IsConnected */

/* nn::hid::detail::GetNpadStates(int *out_count, NpadXState *out, int count, const NpadIdType &id),
 * exported by sdk. The detail versions are what the public ones call, so they keep working
 * when the public ones are interposed below. */
#define DETAIL(style) "_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_" style "EiRKj"
void hid_detail_fullkey(int *n, void *out, int count, const uint32_t *id)
  __asm__(DETAIL("16NpadFullKeyState"));
void hid_detail_handheld(int *n, void *out, int count, const uint32_t *id)
  __asm__(DETAIL("17NpadHandheldState"));
void hid_detail_joydual(int *n, void *out, int count, const uint32_t *id)
  __asm__(DETAIL("16NpadJoyDualState"));

/* NpadIdType values */
enum { NPAD_NO1 = 0, NPAD_HANDHELD = 0x20 };

/* gz-style repeat for held buttons (menu navigation), in frames */
#define REPEAT_FIRST_DELAY 12 /* before the first repeat */
#define REPEAT_INTERVAL    3  /* then every this many */

static uint32_t buttons_held;
static uint32_t buttons_pressed;  /* went down this frame */
static uint32_t repeat_buttons;   /* held since they went down, so they repeat */
static uint32_t buttons_fired;    /* pressed, or repeating this frame */
static int32_t stick_axes[4];     /* lx, ly, rx, ry */
static int repeat_countdown;
bool input_block_game;
uint32_t input_hide_from_game;

/* Add one controller's state (one style of one NpadIdType) to this frame's input. */
static void read_npad(void (*get_states)(int *, void *, int, const uint32_t *), uint32_t npad_id)
{
  uint64_t buf[NPAD_STATE_BUF / 8] = { 0 };
  int state_count = 0;
  get_states(&state_count, buf, 1, &npad_id);
  if (state_count < 1)
    return;
  npad_state_t *state = (npad_state_t *)buf;
  if (!(state->attributes & NPAD_ATTR_CONNECTED))
    return;
  buttons_held |= (uint32_t)state->buttons;
  if (state->lx || state->ly) {
    stick_axes[0] = state->lx;
    stick_axes[1] = state->ly;
  }
  if (state->rx || state->ry) {
    stick_axes[2] = state->rx;
    stick_axes[3] = state->ry;
  }
}

void input_update(void)
{
  uint32_t previously_held = buttons_held;
  buttons_held = 0;
  stick_axes[0] = stick_axes[1] = stick_axes[2] = stick_axes[3] = 0;
  read_npad(hid_detail_fullkey, NPAD_NO1);
  read_npad(hid_detail_joydual, NPAD_NO1);
  read_npad(hid_detail_handheld, NPAD_HANDHELD);
  buttons_pressed = buttons_held & ~previously_held;

  buttons_fired = buttons_pressed;
  if (buttons_pressed) {
    repeat_buttons = buttons_pressed;
    repeat_countdown = REPEAT_FIRST_DELAY;
  } else if (buttons_held & repeat_buttons) {
    repeat_buttons &= buttons_held;
    if (--repeat_countdown <= 0) {
      repeat_countdown = REPEAT_INTERVAL;
      buttons_fired = repeat_buttons;
    }
  } else {
    repeat_buttons = 0;
  }
}

uint32_t input_held(void) { return buttons_held; }
uint32_t input_pressed(void) { return buttons_pressed; }

bool input_combo(uint32_t mask)
{
  return (buttons_held & mask) == mask && (buttons_pressed & mask);
}

bool input_repeat(uint32_t button)
{
  return (buttons_fired & button) != 0;
}

void input_sticks(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry)
{
  *lx = stick_axes[0];
  *ly = stick_axes[1];
  *rx = stick_axes[2];
  *ry = stick_axes[3];
}

/* --- hiding input from the game -------------------------------------------------------------
 * main's calls to the public nn::hid::GetNpadStates overloads go through its GOT; module init
 * points those slots at the wrappers below (got.c). While the menu is open the game then sees
 * the controller idle. Only the buttons and sticks of each state are cleared (Switch 1 layout,
 * 0x28 bytes per state: a longer Switch 2 state is still only written inside the buffer). */
#define PUBLIC(style) "_ZN2nn3hid13GetNpadStatesEPNS0_" style "EiRKj"
typedef int (*hid_public_t)(void *out, int count, const uint32_t *id);

static const char *const hid_public_names[] = {
  PUBLIC("16NpadFullKeyState"), PUBLIC("17NpadHandheldState"), PUBLIC("16NpadJoyDualState"),
  PUBLIC("16NpadJoyLeftState"), PUBLIC("17NpadJoyRightState"),
};
#define N_HID (sizeof(hid_public_names) / sizeof(*hid_public_names))
/* the sdk functions (imported by this module, so they bypass main's GOT) */
int hid_public_0(void *, int, const uint32_t *) __asm__(PUBLIC("16NpadFullKeyState"));
int hid_public_1(void *, int, const uint32_t *) __asm__(PUBLIC("17NpadHandheldState"));
int hid_public_2(void *, int, const uint32_t *) __asm__(PUBLIC("16NpadJoyDualState"));
int hid_public_3(void *, int, const uint32_t *) __asm__(PUBLIC("16NpadJoyLeftState"));
int hid_public_4(void *, int, const uint32_t *) __asm__(PUBLIC("17NpadJoyRightState"));

bool input_force_active;
uint32_t input_force_buttons;
bool input_force_sticks[2];
int32_t input_force_stick[4];

/* Edit the states the game just read: blank them while the menu is open, otherwise add frame
 * advance's buffered input and take out the buttons the game mustn't see. */
static void filter_game_states(void *states, int count)
{
  if (!input_block_game && !input_hide_from_game && !input_force_active)
    return;
  for (int i = 0; i < count; i++) {
    npad_state_t *state = (npad_state_t *)((char *)states + i * NPAD_STATE_SIZE);
    if (input_block_game) {
      state->buttons = 0;
      state->lx = state->ly = state->rx = state->ry = 0;
      continue;
    }
    if (input_force_active) {
      state->buttons |= input_force_buttons;
      if (input_force_sticks[0]) {
        state->lx = input_force_stick[0];
        state->ly = input_force_stick[1];
      }
      if (input_force_sticks[1]) {
        state->rx = input_force_stick[2];
        state->ry = input_force_stick[3];
      }
    }
    state->buttons &= ~(uint64_t)input_hide_from_game;
  }
}

/* hid_wrap_N: the public GetNpadStates overload N, then filter_game_states */
#define WRAP(n)                                                     \
  static int hid_wrap_##n(void *out, int count, const uint32_t *id) \
  {                                                                 \
    int result = hid_public_##n(out, count, id);                    \
    filter_game_states(out, count);                                 \
    return result;                                                  \
  }
WRAP(0) WRAP(1) WRAP(2) WRAP(3) WRAP(4)

static void *const hid_wraps[N_HID] = { hid_wrap_0, hid_wrap_1, hid_wrap_2, hid_wrap_3, hid_wrap_4 };

const char *input_hook_game(uintptr_t main_base)
{
  const char *last_error = 0;
  for (unsigned i = 0; i < N_HID; i++) {
    const char *error = got_hook(main_base, hid_public_names[i], hid_wraps[i]);
    if (error)
      last_error = error;
  }
  return last_error;
}
