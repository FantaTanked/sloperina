/* A small gz-style menu: a tree of menus whose items are submenus, actions, checkboxes,
 * option cycles, numbers and read-only lines. Navigation as in gz: D-pad up/down move,
 * left/right change a value, L selects (on an option: next value; on a number: its action,
 * e.g. fill), "return" items go back. Long menus scroll. Drawing goes through gfx.h. */
#ifndef MENU_H
#define MENU_H

#include <stdbool.h>
#include <stdint.h>

typedef struct menu menu_t;
typedef struct menu_item menu_item_t;

typedef enum { MI_SUBMENU, MI_ACTION, MI_CHECK, MI_OPTION, MI_NUMBER, MI_TEXT, MI_BACK } menu_item_kind_t;

#define NUM_UNKNOWN INT32_MIN

struct menu_item {
  menu_item_kind_t kind;
  const char *label;
  menu_t *submenu;                                   /* MI_SUBMENU */
  void (*action)(menu_item_t *it);                   /* MI_ACTION; MI_NUMBER: on L */
  bool (*get_check)(menu_item_t *it);                /* MI_CHECK */
  void (*set_check)(menu_item_t *it, bool on);
  int  (*get_option)(menu_item_t *it);               /* MI_OPTION: current choice, -1 unknown */
  void (*set_option)(menu_item_t *it, int choice);
  int  n_choices;
  const char *const *choices;                        /* or NULL: */
  void (*choice_text)(menu_item_t *it, int choice, char *buf, int len);
  int32_t (*get_num)(menu_item_t *it);               /* MI_NUMBER: NUM_UNKNOWN if unknown */
  void (*set_num)(menu_item_t *it, int32_t v);
  int32_t (*num_max)(menu_item_t *it);               /* NULL: no maximum */
  int32_t num_min;
  void (*text)(menu_item_t *it, char *buf, int len); /* MI_TEXT line, or the value shown */
  const char *hint;                                  /* shown under the menu while selected (NULL: default) */
  const char *log_label;                             /* the name in the log when it changes (NULL: label);
                                                        a checkbox's can end in ':' ("air walls: on");
                                                        "": not logged */
  intptr_t data, data2;
  uint8_t indent;                                    /* under a heading: drawn MENU_INDENT columns in */
  char icon;                                         /* a GFX_ICON_* shown before the line, or 0 */
  uint8_t picture;                                   /* the game's style: the game's picture of the item
                                                        (gameimage.h's GAMEPIC_*) before its label, or 0 */
  const uint8_t *choice_pictures;                    /* MI_OPTION: the picture for each choice instead */
  bool icon_select;                                  /* the icon is the button: only it (and a "< ... >"
                                                        switch in the line) shows the cursor */
  void (*on_change)(menu_item_t *it, int dir);       /* MI_ACTION: d-left/right */
  bool (*on_move)(menu_item_t *it, int dir);         /* d-up/down: true if the item used it (scrolling) */
  /* gz's buttons in a row: with n_parts > 1, d-left/right picks `part` and L presses it */
  int  n_parts, part;
  void (*press)(menu_item_t *it, int part);
  bool press_repeat;                                 /* L held presses again (gz's scroll arrows) */
  const char *const *part_hints;                     /* the hint for each part (gz-style tooltips) */
  /* the part of the line that shows the cursor while selected (else the whole line): false = whole */
  bool (*cursor)(menu_item_t *it, const char *line, int *start, int *len);
  /* while editing (menu_edit): called each frame instead of the menu's own controls; false ends it */
  bool (*edit)(menu_item_t *it);
  const char *edit_hint;
  bool edit_alone;                                   /* while editing, the menu shows only its hint */
  bool plain;                                        /* MI_TEXT drawn in the normal colour (not dim) */
  bool grid;                                         /* the game's style keeps its line on gz's grid, a
                                                        character a cell: its columns line up with the rows
                                                        around it, and its cursor doesn't move the text */
};

#define MENU_INDENT 2

struct menu {
  const char *title;
  menu_item_t *items;
  int n_items, cap;
  int selected, scroll;
  menu_t *parent;
  bool grouping; /* items added now go under the last heading */
  bool scroll_text; /* a page of text: d-up/down scroll it, L presses the selected item (return) */
  int pinned;       /* the first this many items always show at the top; the rest scroll under them */
};

/* Items come from one shared pool; `cap` reserves that many for this menu. */
void menu_init(menu_t *m, const char *title, int cap);
menu_item_t *menu_add(menu_t *m, menu_item_kind_t kind, const char *label);
menu_item_t *menu_add_submenu(menu_t *m, const char *label, menu_t *sub);
menu_item_t *menu_add_action(menu_t *m, const char *label, void (*fn)(menu_item_t *), intptr_t data);
menu_item_t *menu_add_check(menu_t *m, const char *label, bool (*get)(menu_item_t *),
                            void (*set)(menu_item_t *, bool), intptr_t data);
menu_item_t *menu_add_option(menu_t *m, const char *label, int n, const char *const *choices,
                             int (*get)(menu_item_t *), void (*set)(menu_item_t *, int), intptr_t data);
menu_item_t *menu_add_number(menu_t *m, const char *label, int32_t (*get)(menu_item_t *),
                             void (*set)(menu_item_t *, int32_t), int32_t (*max)(menu_item_t *),
                             void (*fill)(menu_item_t *), intptr_t data);
menu_item_t *menu_add_text(menu_t *m, void (*text)(menu_item_t *, char *, int), intptr_t data);
menu_item_t *menu_add_return(menu_t *m);
/* A heading: the items after it are indented, up to the next heading, menu_end_group or return. */
menu_item_t *menu_add_heading(menu_t *m, const char *label);
void menu_end_group(menu_t *m);

/* the open menu (NULL = closed) */
extern menu_t *menu_current;
void menu_open(menu_t *root);
void menu_close(void);
void menu_back(void);       /* up one level (or ends an edit); closes at the top level */
void menu_goto(menu_t *m);  /* open menu m, wherever it is (gz's menu_enter) */
void menu_edit(menu_item_t *it); /* start editing it (it->edit gets the input) */
extern menu_item_t *menu_editing;
void menu_input(void);      /* navigation, from the frame hook while open */
void menu_draw(void);       /* through gfx.h */
void menu_item_line(menu_item_t *it, char *buf, int len);
/* after a menu's items were rebuilt: keep the selection on an item that exists and can be chosen */
void menu_refresh(menu_t *m);
/* empty the menu to add its items again (its reserved items stay its own) */
void menu_reset(menu_t *m);

#endif
