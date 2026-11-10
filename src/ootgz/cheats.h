/* gz-style cheats, applied every frame (port of ootmenu.pyw apply_cheats). */
#ifndef CHEATS_H
#define CHEATS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  CHEAT_ENERGY, CHEAT_RUPEES, CHEAT_MAGIC, CHEAT_STICKS, CHEAT_NUTS, CHEAT_SEEDS, CHEAT_BOMBS,
  CHEAT_ARROWS, CHEAT_BOMBCHUS, CHEAT_BEANS, CHEAT_SPEED /* levitate.c */,
  CHEAT_WALLS /* levitate.c */, CHEAT_SCRUB_SEEDS /* shown after the beans */,
  CHEAT_COUNT /* append only: settings.ini keeps a bit per cheat */
} cheat_t;

extern const char *const cheat_names[CHEAT_COUNT];
extern bool cheats[CHEAT_COUNT];

void cheats_frame(void);
/* Current magic, for real: the meter object (which owns the value) and the save field. */
void magic_set(int32_t value);

#endif
