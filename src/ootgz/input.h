/* Controller input through the game's own hid library (nn::hid, imported from sdk). */
#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* nn::hid::NpadButton bits */
enum {
  BTN_A = 1u << 0, BTN_B = 1u << 1, BTN_X = 1u << 2, BTN_Y = 1u << 3,
  BTN_LSTICK = 1u << 4, BTN_RSTICK = 1u << 5, BTN_L = 1u << 6, BTN_R = 1u << 7,
  BTN_ZL = 1u << 8, BTN_ZR = 1u << 9, BTN_PLUS = 1u << 10, BTN_MINUS = 1u << 11,
  BTN_DLEFT = 1u << 12, BTN_DUP = 1u << 13, BTN_DRIGHT = 1u << 14, BTN_DDOWN = 1u << 15,
  BTN_LS_LEFT = 1u << 16, BTN_LS_UP = 1u << 17, BTN_LS_RIGHT = 1u << 18, BTN_LS_DOWN = 1u << 19,
  /* the right stick as directions: the C buttons of the original */
  BTN_RS_LEFT = 1u << 20, BTN_RS_UP = 1u << 21, BTN_RS_RIGHT = 1u << 22, BTN_RS_DOWN = 1u << 23,
};

void     input_update(void);       /* once per frame, from the frame hook */
uint32_t input_held(void);
uint32_t input_pressed(void);      /* went down this frame */
/* All of `mask` held and at least one of them pressed this frame. */
bool     input_combo(uint32_t mask);
/* Pressed, or held long enough to repeat (menu navigation). */
bool     input_repeat(uint32_t button);
void     input_sticks(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
/* which controller the input came from last: a Pro Controller, two Joy-Cons apart (or in a grip), or
 * Joy-Cons on the console (nn::hid's FullKey, JoyDual and Handheld styles) */
enum { INPUT_PRO, INPUT_JOYCONS, INPUT_HANDHELD };
int      input_controller(void);

/* While true, the game sees no buttons or sticks. */
extern bool input_block_game;
/* Buttons the game never sees (e.g. L while levitate is on). */
extern uint32_t input_hide_from_game;
/* given to the game on top of the controller until cleared (frame advance's buffered input) */
extern bool input_force_active;
extern uint32_t input_force_buttons;
extern bool input_force_sticks[2];       /* left, right: use the values below */
extern int32_t input_force_stick[4];     /* lx, ly, rx, ry */
/* Point main's nn::hid imports at our wrappers (module init); returns 0 or a message. */
const char *input_hook_game(uintptr_t main_base);

#endif
