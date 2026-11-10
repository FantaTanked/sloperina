/* gz's on-screen keyboard (src/gz/osk.c), in our menu: the text line, gz's four rows of keys, and a
 * row of what gz does with Z + the d-pad (shift, space, backspace, delete) plus "ok".
 *
 *   text line   d-left/right: move the cursor   L: done
 *   key rows    d-left/right: pick a key (the column is kept between rows)   L: type it */
#include "osk.h"

#include "gfx.h"
#include "sys.h"

static menu_t osk_menu;
static char text[OSK_MAX + 1];
static int cursor;     /* where the next character goes, 0..strlen(text) */
static int key_column; /* the key picked, kept between rows */
static bool shift;
static void (*on_done)(const char *, intptr_t);
static intptr_t done_ctx;

static const char *const key_rows[] = { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm,." };
#define N_KEY_ROWS 4
/* the last row: commands instead of characters */
enum { CMD_SHIFT, CMD_SPACE, CMD_BACK, CMD_DELETE, CMD_OK, N_COMMANDS };
static const char *const command_names[N_COMMANDS] = { "shift", "space", "back", "del", "ok" };

/* room for the text line, the key rows, the command row and return */
#define OSK_MENU_ITEMS (N_KEY_ROWS + 4)

static void insert(char c)
{
  int len = (int)strlen(text);
  if (len >= OSK_MAX)
    return;
  for (int i = len; i > cursor; i--)
    text[i] = text[i - 1];
  text[cursor++] = c;
  text[len + 1] = 0;
}

static void erase(int at)
{
  int len = (int)strlen(text);
  if (at < 0 || at >= len)
    return;
  for (int i = at; i < len; i++)
    text[i] = text[i + 1];
}

static void finish(void)
{
  void (*callback)(const char *, intptr_t) = on_done;
  on_done = 0;
  menu_back();
  if (callback)
    callback(text, done_ctx);
}

/* --- the text line ------------------------------------------------------------------------------ */

static void text_line(menu_item_t *it, char *buf, int len)
{
  snprintf(buf, len, "%s_", text); /* the _ shows the cursor at the end */
}

static bool text_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  *start = cursor;
  *len = 1;
  return true;
}

static void text_move(menu_item_t *it, int dir)
{
  cursor += dir;
  int len = (int)strlen(text);
  if (cursor < 0)
    cursor = 0;
  else if (cursor > len)
    cursor = len;
}

static void text_done(menu_item_t *it) { finish(); }

/* --- keys ------------------------------------------------------------------------------------------ */

static char key_char(int row, int column)
{
  char key = key_rows[row][column];
  if (shift && key >= 'a' && key <= 'z')
    key = (char)(key - 'a' + 'A');
  else if (shift && key == '-')
    key = '_';
  return key;
}

static int row_len(int row) { return row < N_KEY_ROWS ? (int)strlen(key_rows[row]) : N_COMMANDS; }

/* A key row's line: its keys one space apart, or the command words. */
static void key_line(menu_item_t *it, char *buf, int len)
{
  int row = (int)it->data;
  int n = 0;
  if (row < N_KEY_ROWS) {
    for (int column = 0; column < row_len(row) && n < len - 2; column++) {
      buf[n++] = key_char(row, column);
      buf[n++] = ' ';
    }
    buf[n ? n - 1 : 0] = 0; /* drop the last space */
  } else {
    for (int cmd = 0; cmd < N_COMMANDS && n < len; cmd++) {
      const char *word = cmd == CMD_SHIFT && shift ? "SHIFT" : command_names[cmd];
      n += snprintf(buf + n, len - n, "%s%s", cmd ? " " : "", word);
    }
  }
}

/* The picked column in this row (a shorter row takes its last key). */
static int key_col(const menu_item_t *it)
{
  int n = row_len((int)it->data);
  return key_column < n ? key_column : n - 1;
}

static bool key_cursor(menu_item_t *it, const char *line, int *start, int *len)
{
  int row = (int)it->data;
  int column = key_col(it);
  if (row < N_KEY_ROWS) {
    *start = 2 * column;
    *len = 1;
    return true;
  }
  int at = 0; /* the command words, one space apart */
  for (int i = 0; i < column; i++)
    at += (int)strlen(command_names[i]) + 1;
  *start = at;
  *len = (int)strlen(command_names[column]);
  return true;
}

static void key_move(menu_item_t *it, int dir)
{
  int n = row_len((int)it->data);
  key_column = (key_col(it) + dir + n) % n;
}

static void key_press(menu_item_t *it)
{
  int row = (int)it->data;
  int column = key_col(it);
  if (row < N_KEY_ROWS) {
    insert(key_char(row, column));
    return;
  }
  switch (column) {
    case CMD_SHIFT: shift = !shift; break;
    case CMD_SPACE: insert(' '); break;
    case CMD_BACK:
      if (cursor > 0)
        erase(--cursor);
      break;
    case CMD_DELETE: erase(cursor); break;
    default: finish(); break;
  }
}

void osk_build(void)
{
  menu_init(&osk_menu, "name", OSK_MENU_ITEMS);
  menu_item_t *text_item = menu_add_action(&osk_menu, "", text_done, 0);
  if (text_item) {
    text_item->text = text_line;
    text_item->cursor = text_cursor;
    text_item->grid = true;
    text_item->on_change = text_move;
    text_item->hint = "d-left/right: cursor   L: done";
  }
  for (int row = 0; row <= N_KEY_ROWS; row++) { /* the key rows, then the command row */
    menu_item_t *row_item = menu_add_action(&osk_menu, "", key_press, row);
    if (row_item) {
      row_item->text = key_line;
      row_item->cursor = key_cursor;
      row_item->grid = true; /* the keys in columns, as the cursor moves between rows */
      row_item->on_change = key_move;
      row_item->hint = row < N_KEY_ROWS ? "d-left/right: key   L: type it"
                                        : "L: shift / space / backspace / delete / done";
    }
  }
  menu_add_return(&osk_menu);
}

void osk_open(const char *title, const char *initial, void (*done)(const char *, intptr_t), intptr_t ctx)
{
  snprintf(text, sizeof(text), "%s", initial ? initial : "");
  cursor = (int)strlen(text);
  shift = false;
  on_done = done;
  done_ctx = ctx;
  osk_menu.title = title;
  osk_menu.parent = menu_current; /* return (and R + d-left) go back there without changing anything */
  menu_goto(&osk_menu);
  osk_menu.selected = 0;
}
