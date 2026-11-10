#include "menu.h"

#include "gameimage.h"
#include "gfx.h"
#include "input.h"
#include "lang.h"
#include "sys.h"

/* --- the item pool --------------------------------------------------------------------------- */

#define POOL_ITEMS 1400

static menu_item_t pool[POOL_ITEMS];
static int pool_used;

menu_t *menu_current;
static menu_t *menu_last; /* where the menu was when it closed; it reopens there */

void menu_init(menu_t *menu, const char *title, int cap)
{
  menu_t *parent = menu->parent; /* may already be linked by menu_add_submenu */
  memset(menu, 0, sizeof(*menu));
  menu->parent = parent;
  menu->title = title;
  if (pool_used + cap > POOL_ITEMS) {
    log_printf("menu pool full (%s)", title);
    cap = POOL_ITEMS - pool_used;
  }
  menu->items = &pool[pool_used];
  menu->cap = cap;
  pool_used += cap;
}

/* --- adding items ---------------------------------------------------------------------------- */

menu_item_t *menu_add(menu_t *menu, menu_item_kind_t kind, const char *label)
{
  if (menu->n_items >= menu->cap) {
    log_printf("menu %s: more than %d items", menu->title, menu->cap);
    return 0;
  }
  menu_item_t *item = &menu->items[menu->n_items++];
  memset(item, 0, sizeof(*item));
  item->kind = kind;
  item->label = label;
  item->indent = menu->grouping && kind != MI_BACK;
  return item;
}

menu_item_t *menu_add_submenu(menu_t *menu, const char *label, menu_t *sub)
{
  menu_item_t *item = menu_add(menu, MI_SUBMENU, label);
  if (item) {
    item->submenu = sub;
    sub->parent = menu;
  }
  return item;
}

menu_item_t *menu_add_action(menu_t *menu, const char *label, void (*fn)(menu_item_t *), intptr_t data)
{
  menu_item_t *item = menu_add(menu, MI_ACTION, label);
  if (item) {
    item->action = fn;
    item->data = data;
  }
  return item;
}

menu_item_t *menu_add_check(menu_t *menu, const char *label, bool (*get)(menu_item_t *),
                            void (*set)(menu_item_t *, bool), intptr_t data)
{
  menu_item_t *item = menu_add(menu, MI_CHECK, label);
  if (item) {
    item->get_check = get;
    item->set_check = set;
    item->data = data;
  }
  return item;
}

menu_item_t *menu_add_option(menu_t *menu, const char *label, int n, const char *const *choices,
                             int (*get)(menu_item_t *), void (*set)(menu_item_t *, int), intptr_t data)
{
  menu_item_t *item = menu_add(menu, MI_OPTION, label);
  if (item) {
    item->n_choices = n;
    item->choices = choices;
    item->get_option = get;
    item->set_option = set;
    item->data = data;
  }
  return item;
}

menu_item_t *menu_add_number(menu_t *menu, const char *label, int32_t (*get)(menu_item_t *),
                             void (*set)(menu_item_t *, int32_t), int32_t (*max)(menu_item_t *),
                             void (*fill)(menu_item_t *), intptr_t data)
{
  menu_item_t *item = menu_add(menu, MI_NUMBER, label);
  if (item) {
    item->get_num = get;
    item->set_num = set;
    item->num_max = max;
    item->action = fill;
    item->data = data;
  }
  return item;
}

menu_item_t *menu_add_text(menu_t *menu, void (*text)(menu_item_t *, char *, int), intptr_t data)
{
  menu_item_t *item = menu_add(menu, MI_TEXT, 0);
  if (item) {
    item->text = text;
    item->data = data;
  }
  return item;
}

static void heading_text(menu_item_t *item, char *buf, int len) { snprintf(buf, len, "%s", T(item->label)); }

menu_item_t *menu_add_heading(menu_t *menu, const char *label)
{
  menu->grouping = false; /* the heading itself isn't indented */
  menu_item_t *item = menu_add(menu, MI_TEXT, label);
  if (item)
    item->text = heading_text;
  menu->grouping = true;
  return item;
}

void menu_end_group(menu_t *menu) { menu->grouping = false; }

menu_item_t *menu_add_return(menu_t *menu)
{
  return menu_add(menu, MI_BACK, "return");
}

/* --- selection and navigation ---------------------------------------------------------------- */

static bool selectable(const menu_item_t *item) { return item->kind != MI_TEXT; }

/* Moves the selection by one (-1 up, +1 down) to the next item that can be chosen, wrapping round. */
static void move_selection(menu_t *menu, int direction)
{
  int column = menu->n_items ? menu->items[menu->selected].part : 0;
  bool from_button_row = menu->n_items && menu->items[menu->selected].n_parts > 1;
  for (int tries = 0; tries < menu->n_items; tries++) {
    menu->selected = (menu->selected + direction + menu->n_items) % menu->n_items;
    menu_item_t *item = &menu->items[menu->selected];
    if (!selectable(item))
      continue;
    /* rows of buttons: the cursor stays in its column, as gz's does */
    if (item->n_parts > 1) {
      if (!from_button_row)
        item->part = 0;
      else
        item->part = column < item->n_parts ? column : item->n_parts - 1;
    }
    return;
  }
}

void menu_refresh(menu_t *menu)
{
  if (menu->selected >= menu->n_items)
    menu->selected = menu->n_items ? menu->n_items - 1 : 0;
  if (menu->n_items && !selectable(&menu->items[menu->selected])) {
    int previous = menu->selected;
    move_selection(menu, 1);
    if (menu->selected < previous) { /* wrapped round: the item above instead */
      menu->selected = previous;
      move_selection(menu, -1);
    }
  }
}

void menu_reset(menu_t *menu)
{
  menu->n_items = 0;
  menu->grouping = false;
}

static void enter(menu_t *menu)
{
  menu_current = menu;
  if (menu->n_items && !selectable(&menu->items[menu->selected]))
    move_selection(menu, 1);
}

menu_item_t *menu_editing;

void menu_goto(menu_t *menu)
{
  menu_editing = 0;
  enter(menu);
}

void menu_edit(menu_item_t *item) { menu_editing = item; }

void menu_open(menu_t *root)
{
  enter(menu_last ? menu_last : root);
  input_block_game = true;
}

void menu_close(void)
{
  menu_editing = 0;
  /* reopen in the same submenu; from the top level, start at the root */
  menu_last = menu_current && menu_current->parent ? menu_current : 0;
  menu_current = 0;
  input_block_game = false;
  gfx_clear();
}

void menu_back(void)
{
  if (menu_editing) {
    menu_editing = 0;
    return;
  }
  if (menu_current->parent)
    menu_current = menu_current->parent;
  else
    menu_close();
}

/* --- changing and activating items ----------------------------------------------------------- */

#define NUMBER_BIG_STEP 10 /* d-left/right with L held */

/* gz's log: what a switch or checkbox is now, or the action taken (unless it logged something itself) */
static void log_item(menu_item_t *item, uint32_t log_count_before)
{
  bool logged_itself = gfx_log_count != log_count_before;
  bool unnamed = !item->label || !item->label[0];
  bool not_logged = item->log_label && !item->log_label[0];
  if (logged_itself || unnamed || not_logged)
    return;
  const char *name = T(item->log_label ? item->log_label : item->label);
  char value[64] = "";
  switch (item->kind) {
    case MI_CHECK:
      gfx_logf("%s %s", name, T(item->get_check(item) ? "on" : "off"));
      break;
    case MI_OPTION: {
      int choice = item->get_option(item);
      if (choice >= 0 && choice < item->n_choices) {
        if (item->choices)
          snprintf(value, sizeof(value), "%s", T(item->choices[choice]));
        else if (item->choice_text)
          item->choice_text(item, choice, value, sizeof(value));
        gfx_logf("%s: %s", name, value);
      }
      break;
    }
    case MI_ACTION:
      if (item->action)
        gfx_logf("%s", name);
      break;
    default:
      break;
  }
}

/* A number moved by `step`. Single steps wrap round like gz's (when there's a maximum); big steps
 * (L held) stop at the ends, and wrap only when already at one. */
static int32_t stepped_number(int32_t value, int step, int32_t min, int32_t max, bool has_max)
{
  if (step < -1) {
    if (value == min && has_max)
      return max;
    return value + step < min ? min : value + step;
  }
  if (step > 1) {
    if (has_max && value == max)
      return min;
    return has_max && value + step > max ? max : value + step;
  }
  value += step;
  if (value < min)
    return has_max ? max : min;
  if (has_max && value > max)
    return min;
  return value;
}

/* d-left/right on an item (direction -1/+1, or +-NUMBER_BIG_STEP on a number) */
static void change_value(menu_item_t *item, int direction)
{
  if (item->kind == MI_ACTION && item->on_change) {
    item->on_change(item, direction);
  } else if (item->kind == MI_OPTION && item->n_choices > 0) {
    int choice = item->get_option(item);
    if (choice < 0) /* unknown: start at the first */
      choice = 0;
    else
      choice = (choice + direction + item->n_choices) % item->n_choices;
    item->set_option(item, choice);
  } else if (item->kind == MI_CHECK) {
    item->set_check(item, direction > 0);
  } else if (item->kind == MI_NUMBER) {
    int32_t value = item->get_num(item);
    int32_t max = item->num_max ? item->num_max(item) : INT32_MAX;
    if (value == NUM_UNKNOWN)
      return;
    bool has_max = max != NUM_UNKNOWN && max != INT32_MAX;
    item->set_num(item, stepped_number(value, direction, item->num_min, max, has_max));
  }
}

/* L on an item */
static void activate_item(menu_item_t *item)
{
  switch (item->kind) {
    case MI_SUBMENU:
      enter(item->submenu);
      break;
    case MI_ACTION:
      if (item->action)
        item->action(item);
      break;
    case MI_CHECK:
      item->set_check(item, !item->get_check(item));
      break;
    case MI_OPTION:
      change_value(item, 1);
      break;
    case MI_NUMBER:
      if (item->action)
        item->action(item);
      break;
    case MI_BACK:
      menu_back();
      break;
    default:
      break;
  }
}

/* numbers aren't logged (scrolling would fill the log); the rest is, as gz logs its commands */
static void change(menu_item_t *item, int direction)
{
  uint32_t log_count_before = gfx_log_count;
  change_value(item, direction);
  if (item->kind != MI_NUMBER)
    log_item(item, log_count_before);
}

static void activate(menu_item_t *item)
{
  uint32_t log_count_before = gfx_log_count;
  activate_item(item);
  log_item(item, log_count_before);
}

/* --- input ----------------------------------------------------------------------------------- */

/* Numbers: L held makes d-left/right go by NUMBER_BIG_STEP, so their own L (fill) happens when L is
 * let go, and only if the d-pad wasn't used meanwhile. */
static bool l_down_on_number, l_used_for_big_steps;

/* L and d-left/right on a number; true if they used this frame's input */
static bool number_input(menu_item_t *item)
{
  bool l_held = (input_held() & BTN_L) != 0;
  if (input_pressed() & BTN_L) {
    l_down_on_number = true;
    l_used_for_big_steps = false;
  }
  if (l_held && (input_repeat(BTN_DLEFT) || input_repeat(BTN_DRIGHT))) {
    l_used_for_big_steps = true;
    change(item, input_repeat(BTN_DLEFT) ? -NUMBER_BIG_STEP : NUMBER_BIG_STEP);
    return true;
  }
  if (!l_held && l_down_on_number) {
    l_down_on_number = false;
    if (!l_used_for_big_steps)
      activate(item);
    return true;
  }
  return (input_pressed() & BTN_L) != 0; /* just pressed: wait for it to be let go */
}

/* a page of text: the page moves, the cursor stays on return (menu_draw keeps the scroll in range) */
static void scroll_text_input(menu_t *menu, menu_item_t *item)
{
  if (input_repeat(BTN_DUP))
    menu->scroll--;
  else if (input_repeat(BTN_DDOWN))
    menu->scroll++;
  else if (input_pressed() & BTN_L)
    activate(item);
}

void menu_input(void)
{
  menu_t *menu = menu_current;
  if (!menu || !menu->n_items)
    return;
  menu_item_t *item = &menu->items[menu->selected];
  if (menu_editing) {
    if (menu_editing != item || !item->edit || !item->edit(item))
      menu_editing = 0;
    return;
  }
  if (menu->scroll_text) {
    scroll_text_input(menu, item);
    return;
  }
  if (item->kind == MI_NUMBER) {
    if (number_input(item))
      return;
  } else {
    l_down_on_number = false;
  }
  bool has_parts = item->n_parts > 1;
  if (input_repeat(BTN_DUP)) {
    if (!item->on_move || !item->on_move(item, -1))
      move_selection(menu, -1);
  } else if (input_repeat(BTN_DDOWN)) {
    if (!item->on_move || !item->on_move(item, 1))
      move_selection(menu, 1);
  } else if (has_parts && input_repeat(BTN_DLEFT)) {
    item->part = item->part > 0 ? item->part - 1 : 0;
  } else if (has_parts && input_repeat(BTN_DRIGHT)) {
    item->part = item->part < item->n_parts - 1 ? item->part + 1 : item->n_parts - 1;
  } else if (input_repeat(BTN_DLEFT)) {
    change(item, -1);
  } else if (input_repeat(BTN_DRIGHT)) {
    change(item, 1);
  } else if (item->press && ((input_pressed() & BTN_L) || (item->press_repeat && input_repeat(BTN_L)))) {
    item->press(item, item->part < item->n_parts ? item->part : 0);
  } else if (input_pressed() & BTN_L) {
    activate(item);
  }
}

/* --- an item's line -------------------------------------------------------------------------- */

static void item_line(menu_item_t *item, char *buf, int len);
static bool picture_shown(const menu_item_t *item);

void menu_item_line(menu_item_t *item, char *buf, int len)
{
  if (!item->icon) {
    item_line(item, buf, len);
    return;
  }
  bool wide_icon = item->icon == GFX_ICON_FOLDER; /* two cells: followed by GFX_WIDE_REST ("\x03") */
  char rest[96];
  item_line(item, rest, sizeof(rest));
  snprintf(buf, len, "%c%s %s", item->icon, wide_icon ? "\x03" : "", rest);
}

static void item_line(menu_item_t *item, char *buf, int len)
{
  char value[96] = "", pictured[96];
  const char *label = T(item->label);
  int picture = item->picture;
  if (picture && item->kind == MI_OPTION && item->choice_pictures) { /* the chosen one's picture */
    int choice = item->get_option(item);
    if (choice >= 0 && choice < item->n_choices && item->choice_pictures[choice])
      picture = item->choice_pictures[choice];
  }
  if (picture && picture_shown(item)) { /* the game's picture of the item before its label */
    snprintf(pictured, sizeof(pictured), "%c%c %s", GFX_PICTURE, GFX_PICTURE_ID(picture), label);
    label = pictured;
  }
  switch (item->kind) {
    case MI_SUBMENU:
      snprintf(buf, len, "%s >", label);
      return;
    case MI_CHECK: {
      char box[3] = { item->get_check(item) ? GFX_CHECK_ON : GFX_CHECK_OFF, GFX_WIDE_REST, 0 };
      if (item->text) {
        item->text(item, value, sizeof(value));
        snprintf(buf, len, "%s%-13s %s", box, label, value);
      } else {
        snprintf(buf, len, "%s%s", box, label);
      }
      return;
    }
    case MI_OPTION: {
      int choice = item->get_option(item);
      if (choice < 0 || choice >= item->n_choices)
        snprintf(value, sizeof(value), "-");
      else if (item->choices)
        snprintf(value, sizeof(value), "%s", T(item->choices[choice]));
      else if (item->choice_text)
        item->choice_text(item, choice, value, sizeof(value));
      snprintf(buf, len, "%s  < %s >", label, value);
      return;
    }
    case MI_NUMBER: {
      int32_t number = item->get_num(item);
      if (number == NUM_UNKNOWN)
        snprintf(value, sizeof(value), "-");
      else if (item->text)
        item->text(item, value, sizeof(value));
      else
        snprintf(value, sizeof(value), "%d", (int)number);
      int32_t max = item->num_max ? item->num_max(item) : NUM_UNKNOWN;
      /* "/ max" only for a plain number whose maximum is known */
      if (item->num_max && max != NUM_UNKNOWN && number != NUM_UNKNOWN && !item->text)
        snprintf(buf, len, "%s  < %s > / %d", label, value, (int)max);
      else
        snprintf(buf, len, "%s  < %s >", label, value);
      return;
    }
    case MI_TEXT:
      item->text(item, buf, len);
      return;
    default:
      if (item->text && !item->label[0]) { /* the text is the whole line */
        item->text(item, buf, len);
      } else if (item->text) {
        item->text(item, value, sizeof(value));
        snprintf(buf, len, "%s  %s", label, value);
      } else {
        snprintf(buf, len, "%s", label);
      }
      return;
  }
}

/* --- hints ----------------------------------------------------------------------------------- */

/* What the selected item's controls do (in English): its own hint, or one for its kind. */
static const char *item_hint_english(const menu_item_t *item)
{
  if (item->part_hints && item->n_parts > 1 && item->part < item->n_parts && item->part_hints[item->part])
    return item->part_hints[item->part];
  if (item->hint)
    return item->hint[0] ? item->hint : 0; /* "": no hint */
  switch (item->kind) {
    case MI_NUMBER: return item->action ? "d-left/right: change (L held: by 10)   L: fill"
                                        : "d-left/right: change (L held: by 10)";
    case MI_OPTION: return "d-left/right or L: change";
    default: return 0;
  }
}

static const char *item_hint(const menu_item_t *item) { return T(item_hint_english(item)); }

/* --- drawing --------------------------------------------------------------------------------- */

/* items (and the hint) start here: column 0 has the ^ / v scroll marks, then a blank column */
#define ITEM_COL 2
#define HINT_MIN_W 36 /* hints wrap at the panel's width, but at least this wide */
#define HINT_MAX_ROWS 4
#define TITLE_MAX_ROWS 3
#define LINE_BUF (GFX_COLS * 2)
#define MIN_SCROLL_ROWS 3    /* the scrolling part of the list always gets at least this many rows */
#define MAX_PART_HINTS 8     /* the panel's size allows for the hints of at most this many parts */
#define WRAP_MIN_W 8         /* a wrapped line is never narrower than this, however deep its indent */
#define MAX_MENU_DEPTH 16
#define CRUMB_SEPARATOR_W 3  /* " > " between two menus */
#define CRUMB_LINE_END_W 2   /* " >" ending a broken breadcrumb line */
#define FILLER_CHAR 0x7F     /* drawn as nothing, but counts as content: it holds the panel's size */

static int max_int(int a, int b) { return a > b ? a : b; }

/* The breadcrumb of the menu in up to TITLE_MAX_ROWS lines of at most `width` columns: menus joined by
 * " > ", a line broken after a " >" with the next indented 2. Too many lines: the start is dropped for
 * "... >". Returns the number of lines. */
static int crumb_lines(const menu_t *menu, int width, char out[][GFX_COLS + 1])
{
  const menu_t *path[MAX_MENU_DEPTH]; /* path[0] is this menu, path[depth - 1] the root */
  int depth = 0;
  for (const menu_t *m = menu; m && depth < MAX_MENU_DEPTH; m = m->parent)
    path[depth++] = m;
  /* try the whole path first; then without its first name, and so on */
  for (int first = depth - 1; first >= 0; first--) {
    int row = 0;
    int len = 0;
    bool fits = true;
    out[0][0] = 0;
    if (first != depth - 1)
      len = snprintf(out[0], GFX_COLS + 1, "...");
    for (int i = first; i >= 0; i--) {
      const char *title = T(path[i]->title);
      int title_len = (int)strlen(title);
      bool line_start = i == first && first == depth - 1; /* the very first name: nothing before it */
      if (!line_start && len + CRUMB_SEPARATOR_W + title_len > width) {
        /* " > name" doesn't fit: end this line with " >" and go on on the next */
        if (len + CRUMB_LINE_END_W > width || row + 1 >= TITLE_MAX_ROWS) {
          fits = false;
          break;
        }
        snprintf(out[row] + len, GFX_COLS + 1 - len, " >");
        row++;
        len = snprintf(out[row], GFX_COLS + 1, "  ");
        line_start = true;
      }
      int separator_w = line_start ? 0 : CRUMB_SEPARATOR_W;
      if (len + separator_w + title_len > width) { /* one name wider than the line: cut it */
        title_len = width - len - separator_w;
        if (title_len < 1) {
          fits = false;
          break;
        }
      }
      len += snprintf(out[row] + len, GFX_COLS + 1 - len, "%s%.*s", line_start ? "" : " > ", title_len, title);
    }
    if (fits)
      return row + 1;
  }
  snprintf(out[0], GFX_COLS + 1, "%.*s", width, T(menu->title)); /* (can't happen: the last try is one name) */
  return 1;
}

/* Word-wraps text (lines split at '\n' too) to `width` columns into out; a wrapped line keeps the
 * indent its line started with. Returns the lines (at least 1); *longest (if wanted): the longest. */
static int wrap_text(const char *text, int width, char out[][GFX_COLS + 1], int max_lines, int *longest)
{
  int n_lines = 0;
  if (longest)
    *longest = 0;
  if (!text)
    text = "";
  while (n_lines < max_lines) {
    const char *line_end = text;
    while (*line_end && *line_end != '\n')
      line_end++;
    int indent = 0;
    while (text + indent < line_end && text[indent] == ' ')
      indent++;
    const char *p = text;
    bool first_piece = true;
    do {
      int pad = first_piece ? 0 : indent; /* the first piece has its own indent in it */
      if (!first_piece)
        while (p < line_end && *p == ' ')
          p++;
      int room = width - pad < WRAP_MIN_W ? WRAP_MIN_W : width - pad;
      const char *piece_end = line_end - p > room ? p + room : line_end;
      if (piece_end < line_end) { /* back to the last space, if there's one (not in the indent) */
        const char *earliest = p + (first_piece ? indent : 0);
        const char *space = piece_end;
        while (space > earliest && *space != ' ')
          space--;
        if (space > earliest)
          piece_end = space;
      }
      int piece_len = (int)(piece_end - p);
      snprintf(out[n_lines], GFX_COLS + 1, "%*s%.*s", pad, "", piece_len, p);
      if (longest && pad + piece_len > *longest)
        *longest = pad + piece_len;
      n_lines++;
      p = piece_end;
      first_piece = false;
    } while (p < line_end && n_lines < max_lines);
    if (!*line_end)
      break;
    text = line_end + 1;
  }
  return n_lines ? n_lines : 1;
}

static char hint_lines[HINT_MAX_ROWS][GFX_COLS + 1];

static const char *current_hint(const menu_t *menu)
{
  const char *hint = menu->n_items ? item_hint(&menu->items[menu->selected]) : 0;
  if (menu_editing && menu_editing->edit_hint)
    hint = T(menu_editing->edit_hint);
  if (menu->scroll_text)
    hint = T("d-up/down: scroll   L: return");
  return hint;
}

/* In a run of items (the items between two headings or other lines of text, at one indent), the labels
 * line up: each item starts as far in as the run's checkboxes and the game's pictures of items push the
 * labels after them, less what's before its own label. Not "return", nor rows laid out by their own text
 * (an icon first, gz's grid, no label). */
#define BOX_CELLS 2     /* a checkbox: GFX_CHECK_* and GFX_WIDE_REST */
#define PICTURE_CELLS 3 /* the game's picture of the item (two cells) and a space (item_line) */
static bool lines_up(const menu_item_t *item)
{
  return item->kind != MI_TEXT && item->kind != MI_BACK && !item->icon && !item->grid && item->label &&
         item->label[0];
}

/* the game's picture of the item is drawn before its label: the game's style, with the pictures there (not
 * when loading them failed: no room left for nothing) */
static bool picture_shown(const menu_item_t *item)
{
  return item->picture && gfx_style == GFX_STYLE_GAME && !gameimage_failed();
}

static int label_lead(const menu_item_t *item)
{
  return (item->kind == MI_CHECK ? BOX_CELLS : 0) + (picture_shown(item) ? PICTURE_CELLS : 0);
}

static int label_pad(const menu_t *menu, int index)
{
  const menu_item_t *item = &menu->items[index];
  if (!lines_up(item))
    return 0;
  int most = label_lead(item);
  for (int dir = -1; dir <= 1; dir += 2)
    for (int i = index + dir; i >= 0 && i < menu->n_items; i += dir) {
      const menu_item_t *other = &menu->items[i];
      if (other->kind == MI_TEXT || other->indent != item->indent)
        break;
      if (lines_up(other) && label_lead(other) > most)
        most = label_lead(other);
    }
  return most - label_lead(item);
}

/* the widest item line, from column 0 */
static int items_width(menu_t *menu)
{
  char line[LINE_BUF];
  int width = 0;
  for (int i = 0; i < menu->n_items; i++) {
    menu_item_line(&menu->items[i], line, sizeof(line));
    width = max_int(width, ITEM_COL + menu->items[i].indent * MENU_INDENT + label_pad(menu, i) + (int)strlen(line));
  }
  return width;
}

static void measure_hint(const char *hint, int wrap_w, int *rows, int *width)
{
  if (!hint)
    return;
  int longest;
  int hint_rows = wrap_text(hint, wrap_w, hint_lines, HINT_MAX_ROWS, &longest);
  *rows = max_int(*rows, hint_rows);
  *width = max_int(*width, longest);
}

/* The rows and width of the biggest hint any item of the menu can show, so the panel keeps one size
 * while in the menu. */
static void measure_hints(const menu_t *menu, const char *current, int wrap_w, int *rows, int *width)
{
  *rows = 1;
  *width = 0;
  for (int i = 0; i < menu->n_items; i++) {
    const menu_item_t *item = &menu->items[i];
    measure_hint(item_hint(item), wrap_w, rows, width);
    measure_hint(T(item->edit_hint), wrap_w, rows, width);
    for (int k = 0; item->part_hints && k < item->n_parts && k < MAX_PART_HINTS; k++)
      measure_hint(T(item->part_hints[k]), wrap_w, rows, width);
  }
  measure_hint(current, wrap_w, rows, width);
}

/* Keeps the selection in view in the scrolling rows (those under the pinned ones). */
static void update_scroll(menu_t *menu, int pinned, int visible)
{
  /* headings and text just above the cursor come into view with it (they can't be selected, so
   * the cursor alone would never scroll back to a heading at the top) */
  int top = menu->selected;
  while (top > pinned && !selectable(&menu->items[top - 1]) && menu->selected - (top - 1) < visible)
    top--;
  /* and the same below it: the read-only lines under a menu's return */
  int bottom = menu->selected;
  while (bottom + 1 < menu->n_items && !selectable(&menu->items[bottom + 1]) && bottom + 1 - top < visible)
    bottom++;
  if (menu->scroll_text) /* scrolled by the d-pad: only kept in range */
    menu->scroll = menu->scroll < 0 ? 0 : menu->scroll;
  else if (top < menu->scroll)
    menu->scroll = top;
  else if (bottom >= menu->scroll + visible)
    menu->scroll = bottom - visible + 1;
  if (menu->scroll > menu->n_items - visible)
    menu->scroll = menu->n_items - visible > 0 ? menu->n_items - visible : 0;
  if (menu->scroll < pinned)
    menu->scroll = pinned;
}

/* gz: a button icon in the highlight colour, and the "< ... >" switch in its line (if any) */
static void draw_icon_button(char *line, int col, int y)
{
  char icon[2] = { line[0], 0 };
  gfx_text(col, y, GFX_NORMAL, line);
  gfx_text(col, y, GFX_SELECTED, icon);
  char *open = line; /* (strchr isn't imported) */
  while (*open && *open != '<')
    open++;
  char *close = open;
  while (*close && *close != '>')
    close++;
  if (*open && *close) {
    close[1] = 0;
    gfx_text(col + (int)(open - line), y, GFX_SELECTED, open);
  }
}

static void draw_item(menu_t *menu, int index, int y)
{
  menu_item_t *item = &menu->items[index];
  char line[LINE_BUF];
  menu_item_line(item, line, sizeof(line));
  bool selected = index == menu->selected;
  int col = ITEM_COL + item->indent * MENU_INDENT + label_pad(menu, index);
  int cursor_start = 0;
  int cursor_len = 0;
  if (selected && item->cursor && item->cursor(item, line, &cursor_start, &cursor_len)) {
    /* only part of the line shows the cursor */
    int line_len = (int)strlen(line);
    gfx_text(col, y, GFX_NORMAL, line);
    if (cursor_start >= 0 && cursor_start < line_len) {
      if (cursor_start + cursor_len < line_len)
        line[cursor_start + cursor_len] = 0;
      gfx_text(col + cursor_start, y, GFX_SELECTED, line + cursor_start);
    }
  } else if (selected && item->icon_select) {
    draw_icon_button(line, col, y);
  } else {
    gfx_style_t style = GFX_NORMAL;
    if (selected)
      style = GFX_SELECTED;
    else if (item->kind == MI_TEXT && !item->plain)
      style = GFX_DIM;
    gfx_text(col, y, style, line);
  }
}

/* what an item's row is, for the game's style (gfx_row) */
static int row_kind(const menu_item_t *item)
{
  int grid = item->grid ? GFX_ROW_GRID : 0;
  if (item->kind == MI_TEXT)
    return (item->text == heading_text ? GFX_ROW_HEADING : GFX_ROW_LINE) | grid;
  int indent = item->indent ? GFX_ROW_INDENT : 0;
  return (item->kind == MI_SUBMENU ? GFX_ROW_ITEM | GFX_ROW_OPENS : GFX_ROW_ITEM) | grid | indent;
}

void menu_draw(void)
{
  menu_t *menu = menu_current;
  if (!menu)
    return;
  const char *hint = current_hint(menu);
  if (menu_editing && menu_editing->edit_alone) { /* out of the way of what's being moved */
    gfx_begin();
    gfx_text(0, 0, GFX_TITLE, T(menu->title));
    gfx_text(ITEM_COL, 2, GFX_DIM, hint ? hint : "");
    gfx_row(0, GFX_ROW_TITLE);
    gfx_row(1, GFX_ROW_GAP);
    gfx_row(2, GFX_ROW_HINT);
    gfx_hint(hint);
    gfx_end();
    return;
  }
  /* the items' width; hints wrap to it (at least HINT_MIN_W), and take as many rows as this menu's
   * longest hint, so the panel keeps one size while in the menu */
  int items_w = items_width(menu);
  int wrap_w = items_w - ITEM_COL < HINT_MIN_W ? HINT_MIN_W : items_w - ITEM_COL;
  if (wrap_w > GFX_COLS - ITEM_COL)
    wrap_w = GFX_COLS - ITEM_COL;
  int hint_rows;
  int hint_w;
  measure_hints(menu, hint, wrap_w, &hint_rows, &hint_w);

  /* breadcrumb: root > ... > this menu, wrapped after a " >" to the panel's width (continuation lines
   * indented); a path longer than TITLE_MAX_ROWS keeps its end */
  int title_w = max_int(items_w, ITEM_COL + hint_w);
  if (title_w < ITEM_COL + HINT_MIN_W)
    title_w = ITEM_COL + HINT_MIN_W;
  else if (title_w > GFX_COLS)
    title_w = GFX_COLS;
  static char title_lines[TITLE_MAX_ROWS][GFX_COLS + 1];
  int title_rows = crumb_lines(menu, title_w, title_lines);
  int title_len = 0;
  for (int k = 0; k < title_rows; k++)
    title_len = max_int(title_len, (int)strlen(title_lines[k]));

  /* item rows that fit on screen: the title and a blank line above them; a blank and the hint below
   * (always room for the hint, so the list doesn't jump) */
  int first_item_y = title_rows + 1;
  int visible = gfx_menu_rows - first_item_y - 1 - hint_rows;
  if (visible < MIN_SCROLL_ROWS)
    visible = MIN_SCROLL_ROWS;
  /* the first `pinned` items stay at the top; the rest scroll under them in `visible` rows */
  int pinned = 0;
  if (menu->pinned > 0 && menu->pinned < menu->n_items && visible - menu->pinned >= MIN_SCROLL_ROWS)
    pinned = menu->pinned;
  visible -= pinned;
  update_scroll(menu, pinned, visible);

  gfx_begin();
  for (int k = 0; k < title_rows; k++) {
    gfx_text(0, k, GFX_TITLE, title_lines[k]);
    gfx_row(k, GFX_ROW_TITLE);
  }
  gfx_row(first_item_y - 1, GFX_ROW_GAP);
  for (int row = 0; row < pinned + visible; row++) {
    int index = row < pinned ? row : menu->scroll + row - pinned;
    if (index >= menu->n_items)
      break;
    /* a heading never ends the list: with nothing of its group under it, its row stays empty */
    if (row == pinned + visible - 1 && index + 1 < menu->n_items && menu->items[index].text == heading_text) {
      gfx_row(first_item_y + row, GFX_ROW_LINE);
      break;
    }
    draw_item(menu, index, first_item_y + row);
    gfx_row(first_item_y + row, row_kind(&menu->items[index]));
  }
  if (menu->scroll > pinned)
    gfx_text(0, first_item_y + pinned, GFX_DIM, "^");
  if (menu->scroll + visible < menu->n_items)
    gfx_text(0, first_item_y + pinned + visible - 1, GFX_DIM, "v");
  if (menu->n_items - pinned > visible) /* the game's style draws a scroll bar */
    gfx_scroll(first_item_y + pinned, visible, menu->n_items - pinned, menu->scroll - pinned);

  /* The panel keeps one size while in this menu (a size change redraws it, which flashes): the hint
   * rows are always there, and the width is the widest line or hint of any item, not just the
   * visible ones. */
  static const char filler[2] = { FILLER_CHAR, 0 };
  int scrolled_shown = menu->n_items - menu->scroll < visible ? menu->n_items - menu->scroll : visible;
  int hint_y = first_item_y + pinned + scrolled_shown + 1;
  int n_hint_lines = wrap_text(hint, wrap_w, hint_lines, HINT_MAX_ROWS, 0);
  /* the game's style wraps the hint itself, and leaves the hint's place out of a menu that has none */
  bool hints = hint_w > 0;
  gfx_row(hint_y - 1, hints ? GFX_ROW_GAP : GFX_ROW_SKIP);
  for (int k = 0; k < hint_rows; k++)
    gfx_row(hint_y + k, hints ? GFX_ROW_HINT : GFX_ROW_SKIP);
  gfx_hint(hint);
  for (int k = 0; k < n_hint_lines; k++)
    gfx_text(ITEM_COL, hint_y + k, GFX_DIM, hint_lines[k][0] ? hint_lines[k] : filler);
  if (n_hint_lines < hint_rows) /* the rows the longest hint takes stay the panel's */
    gfx_text(ITEM_COL, hint_y + hint_rows - 1, GFX_DIM, filler);
  int width = max_int(title_len, max_int(items_w, ITEM_COL + hint_w));
  if (width > GFX_COLS)
    width = GFX_COLS;
  gfx_text(width - 1, first_item_y - 1, GFX_DIM, filler);
  gfx_end();
}
