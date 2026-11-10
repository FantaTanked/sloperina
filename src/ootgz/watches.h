/* On-screen watches, timer and lag counter (port of ootmenu's watches and FrameStats). */
#ifndef WATCHES_H
#define WATCHES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  W_X, W_Y, W_Z, W_FACING, W_MOVE_ANGLE, W_SPEED, W_Y_SPEED, W_CAMERA_ANGLE, W_CAMERA_PITCH,
  W_CAMERA_DIST, W_INPUT, W_STICK, W_LAG, W_TIMER, W_FRAME, W_MAP, W_AIRWALL,
  W_JS_HEIGHT, W_COUNT /* append only: built-in IDs are saved in settings.ini */
} watch_t;

extern const char *const watch_labels[W_COUNT];
extern bool watches_shown;
extern bool watches_header; /* a small "watches" header over them in the box */
/* The watches on screen, in order (gz's watch list): the predefined ones, each at most once, and
 * memory watches (gz's: an address and a type), at most one per address. Memory watches aren't
 * saved: addresses change every time the game starts. */
#define W_MEMORY W_COUNT
enum { MW_U8, MW_S8, MW_X8, MW_U16, MW_S16, MW_X16, MW_U32, MW_S32, MW_X32, MW_F32, MW_X64, MW_NTYPES };
extern const char *const memory_type_names[MW_NTYPES];
extern const uint8_t memory_type_size[MW_NTYPES];
#define WATCH_LABEL_MAX 12
typedef struct {
  int16_t id;                         /* watch_t, or W_MEMORY */
  uint8_t type;                       /* W_MEMORY: MW_* */
  uintptr_t addr;                     /* W_MEMORY */
  char label[WATCH_LABEL_MAX + 1];    /* the user's name for it (gz's keyboard), or "" */
  bool hidden;                        /* kept in the list but not shown on screen */
} watch_entry_t;
#define WATCH_MAX 32
extern watch_entry_t watch_list[WATCH_MAX];
extern int watch_count;
extern uint32_t watch_version;     /* changes whenever the list does */
bool watch_has(watch_t w);
bool watch_add(watch_t w);         /* false if it's already there */
bool watch_add_memory(uintptr_t addr, int type); /* false if that address is already watched */
void watch_remove(int index);
void watch_set_list(const watch_t *list, int n); /* settings; includes JS Height if missing */
/* an entry's label, value, and display types (and its current one) */
void watch_entry_label(const watch_entry_t *e, char *buf, int len);
void watch_entry_display_label(const watch_entry_t *e, char *buf, int len);
void watch_entry_text(const watch_entry_t *e, char *buf, int len);
const char *const *watch_entry_types(const watch_entry_t *e, int *n, int *current);
void watch_entry_set_type(int index, int type);
/* where the value is in memory, for the wrench (debug > memory): the built-in watches that read one
 * value (position, facing, y speed, camera, frame, map, JS Height) and memory watches */
bool watch_entry_has_address(const watch_entry_t *e);
/* JS Height's f32: [[link_actor() + 0x270] + 0x358] + 0x18E8 (false while a pointer is missing) */
bool js_height_address(uintptr_t *addr);
bool watch_entry_address(const watch_entry_t *e, uintptr_t *addr, int *mw_type);
void watch_move(int index, int dir);              /* up (-1) or down (+1) the list */
void watch_set_label(int index, const char *label); /* "" = its own name again */
void watch_set_hidden(int index, bool hidden);
/* display types (gz's watch types), per watch; a watch with none (text) gives *n = 0 */
extern uint8_t watch_types[W_COUNT];
const char *const *watch_type_names(watch_t w, int *n);
void watch_set_type(watch_t w, int type);
extern int watches_corner;         /* gfx_corner_t */
extern int watches_x, watches_y;   /* GFX_CUSTOM: top-left, thousandths of the picture */
extern bool watches_moving;        /* shown while the menu is open, to move them */
extern bool input_display;         /* gz's input display (bottom left unless moved) */
extern int input_corner;           /* ... where: gfx_corner_t, as the watches */
extern int input_x, input_y;       /* ... GFX_CUSTOM: top-left, thousandths of the picture */
extern bool input_moving;          /* ... shown while the menu is open, to move it */
extern bool input_display_bg;      /* ... on a background, as the watches */
extern bool input_show_left, input_show_right, input_show_buttons; /* what it shows */
extern int input_style;            /* 0 a row (gz's), 1 a controller (Switch 2 Pro Controller) */
extern int input_size;             /* 0 small, 1 medium, 2 large: 50, 75, 100% of the menu's size */
extern const char *const watches_corner_names[];

void watch_text(watch_t w, char *buf, int len);
/* Optional gameplay control: off at startup, not saved. Re-resolves the current player.
 * Reset writes FLT_MAX once; lock repeats it each frame, even with the HUD hidden. */
extern bool js_height_lock;
bool js_height_reset(void); /* false if the current threshold is not readable/writable */
void timer_toggle(void);
void timer_reset(void);
void lag_reset(void);
/* frame hook: commands (menu closed) and the on-screen panel */
void watches_frame(bool menu_open);

#endif
