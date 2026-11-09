/* debug > memory (gz's mem.c, for the Switch: 64-bit addresses, little-endian values).
 *
 *   address   L: type one in (d-left/right a digit, d-up/down change it, L go)   d-left/right: a page
 *   view      byte / halfword / word / float / pointer
 *   go to     gz's domains, here the places worth looking at: main, ootgz, Link, the camera...
 *   arrows    gz's scroll buttons, over the rows: L scrolls a row up or down (held: keeps going)
 *   rows      8 bytes each. L: edit the row: d-left/right moves along it, d-up/down changes the digit
 *             under the cursor (written at once, if the memory is writable). On the row's address,
 *             L adds a watch there, as gz's row button does; in the pointer view, L on a value goes
 *             where it points. R + d-left stops editing.
 *
 * Nothing is read that isn't mapped (svcQueryMemory, sys_mapped); unreadable bytes show as ??. */
#include "memview.h"

#include "gamedata.h"
#include "gfx.h"
#include "input.h"
#include "link.h"
#include "lang.h"
#include "sys.h"
#include "watches.h"
#include "z64hd.h"

#define ROWS 12
#define ROW_BYTES 8
#define ADDR_DIGITS 10              /* the Switch's 39-bit addresses */
#define CELLS_COL (ADDR_DIGITS + 2) /* the row's address, two spaces, then its cells */
#define FLOAT_CELL_W 13             /* "%13.6g" */
#define POINTER_BYTES 8
#define ADDRESS_DIGITS_COL 9        /* the address line's digits start after "address  " */
#define DIGIT_BITS 4                /* a hex digit */

menu_t memview_menu;
static menu_t *watches_menu; /* opened after a watch is added from a row */

enum { V_BYTE, V_HALF, V_WORD, V_FLOAT, V_PTR, V_N };
static const char *const view_names[V_N] = { "byte", "halfword", "word", "float", "pointer" };
static const int view_size[V_N] = { 1, 2, 4, 4, POINTER_BYTES };
static const uint8_t view_watch_type[V_N] = { MW_X8, MW_X16, MW_X32, MW_F32, MW_X64 };

static uintptr_t view_addr; /* the first row's */
static int view;

/* --- where to go (gz's domains) ----------------------------------------------------------------- */

/* Link's status object: [[link + 8] + 0x6A0] (docs/FINDINGS.md: it holds his health) */
#define ACTOR_STATE_PTR 0x8
#define STATE_PARAMS_PTR 0x6A0

extern char __module_start[];
static uintptr_t at_main(void) { return z64hd_main; }
static uintptr_t at_ootgz(void) { return (uintptr_t)__module_start; }
static uintptr_t at_link(void) { return link_actor(); }
static uintptr_t at_status(void)
{
  uintptr_t link = link_actor();
  uintptr_t state = link ? sys_ptr(link + ACTOR_STATE_PTR) : 0;
  return state ? sys_ptr(state + STATE_PARAMS_PTR) : 0;
}
static uintptr_t at_camera(void) { return camera(); }
static uintptr_t at_gamedata(void) { return (uintptr_t)gd_manager(); }
static uintptr_t at_strings(void) { return (uintptr_t)gd_get_str(0); }

static const struct { const char *name; uintptr_t (*addr)(void); } places[] = {
  { "main", at_main }, { "sloperinaGZ", at_ootgz }, { "link actor", at_link }, { "link status", at_status },
  { "camera", at_camera }, { "game data", at_gamedata }, { "save strings", at_strings },
};
#define N_PLACES ((int)(sizeof(places) / sizeof(*places)))
static const char *place_names[N_PLACES];
static int place = 0;

/* shows the row holding addr first */
static void show(uintptr_t addr)
{
  view_addr = addr & ~(uintptr_t)(ROW_BYTES - 1);
}

/* --- values --------------------------------------------------------------------------------------- */

static bool read_cell(uintptr_t addr, int size, uint64_t *value)
{
  *value = 0;
  if (!sys_mapped(addr, (size_t)size, SYS_PERM_R))
    return false;
  memcpy(value, (const void *)addr, (size_t)size);
  return true;
}

/* a cell's width in columns, and its column in a row */
static int cell_width(void) { return view == V_FLOAT ? FLOAT_CELL_W : 2 * view_size[view]; }
static int cell_col(int cell) { return CELLS_COL + cell * (cell_width() + 1); }

/* value with the hex digit at `shift` stepped by dir (wrapping within the digit) */
static uint64_t step_digit(uint64_t value, int shift, int dir)
{
  uint64_t digit = ((value >> shift & 0xF) + (uint64_t)(dir + 16)) & 0xF; /* +16: never negative */
  return (value & ~((uint64_t)0xF << shift)) | digit << shift;
}

static void row_text(menu_item_t *item, char *buf, int len)
{
  uintptr_t row = view_addr + (uintptr_t)item->data * ROW_BYTES;
  int used = snprintf(buf, len, "%0*lx  ", ADDR_DIGITS, (unsigned long)row);
  int size = view_size[view];
  int width = cell_width();
  for (int cell = 0; cell < ROW_BYTES / size && used < len; cell++) {
    uint64_t value;
    if (!read_cell(row + (uintptr_t)(cell * size), size, &value)) {
      used += snprintf(buf + used, len - used, "%.*s", width, "?????????????????");
    } else if (view == V_FLOAT) {
      float f;
      uint32_t bits = (uint32_t)value;
      memcpy(&f, &bits, 4);
      used += snprintf(buf + used, len - used, "%13.6g", (double)f);
    } else {
      used += snprintf(buf + used, len - used, "%0*llx", width, (unsigned long long)value);
    }
    if (used < len - 1) {
      buf[used++] = ' ';
      buf[used] = 0;
    }
  }
}

/* gz's scroll arrows ("^ v"), then its cell header: the low digit of each cell's address */
static void header_text(menu_item_t *item, char *buf, int len)
{
  int used = snprintf(buf, len, "%c %c%*s", GFX_ICON_UP, GFX_ICON_DOWN, CELLS_COL - 3, "");
  int size = view_size[view];
  for (int cell = 0; cell < ROW_BYTES / size && used < len; cell++) {
    unsigned low_digit = (unsigned)((view_addr + (uintptr_t)(cell * size)) & 0xF);
    used += snprintf(buf + used, len - used, "%-*x", cell_width() + 1, low_digit);
  }
}

/* --- the address line ----------------------------------------------------------------------------- */

static uintptr_t edit_addr;
/* the digit being edited: on the address line, 0 is the leftmost; on a row, 0 is the row's address and
 * 1.. the digits of its cells, left to right */
static int edit_pos;

static void address_text(menu_item_t *item, char *buf, int len)
{
  uintptr_t addr = menu_editing == item ? edit_addr : view_addr;
  snprintf(buf, len, TF("address  %0*lx"), ADDR_DIGITS, (unsigned long)addr);
}

static bool address_cursor(menu_item_t *item, const char *line, int *start, int *len)
{
  if (menu_editing != item)
    return false;
  *start = ADDRESS_DIGITS_COL + edit_pos;
  *len = 1;
  return true;
}

static bool address_edit(menu_item_t *item)
{
  if (input_repeat(BTN_DLEFT)) {
    edit_pos = edit_pos > 0 ? edit_pos - 1 : 0;
  } else if (input_repeat(BTN_DRIGHT)) {
    edit_pos = edit_pos < ADDR_DIGITS - 1 ? edit_pos + 1 : ADDR_DIGITS - 1;
  } else if (input_repeat(BTN_DUP) || input_repeat(BTN_DDOWN)) {
    int shift = (ADDR_DIGITS - 1 - edit_pos) * DIGIT_BITS;
    int dir = input_repeat(BTN_DUP) ? 1 : -1;
    edit_addr = (uintptr_t)step_digit(edit_addr, shift, dir);
  } else if (input_pressed() & BTN_L) {
    show(edit_addr);
    gfx_logf("memory %0*lx", ADDR_DIGITS, (unsigned long)view_addr);
    return false;
  }
  return true;
}

static void act_address(menu_item_t *item)
{
  edit_addr = view_addr;
  edit_pos = ADDR_DIGITS - 1;
  menu_edit(item);
}

/* d-left/right on the address line or a row: a page back or on */
static void page(menu_item_t *item, int dir)
{
  uintptr_t step = ROWS * ROW_BYTES;
  if (dir < 0 && view_addr < step)
    view_addr = 0;
  else
    view_addr += dir < 0 ? (uintptr_t)0 - step : step;
}

/* --- view and places --------------------------------------------------------------------------------- */

static int get_view(menu_item_t *item) { return view; }
static void set_view(menu_item_t *item, int new_view) { view = new_view; }
static int get_place(menu_item_t *item) { return place; }
static void set_place(menu_item_t *item, int new_place)
{
  place = new_place;
  uintptr_t addr = places[new_place].addr();
  if (addr)
    show(addr);
  else
    gfx_logf("%s: not found right now", places[new_place].name);
}

/* --- rows ----------------------------------------------------------------------------------------- */

/* the row's address, then each hex digit (a float row: only the address) */
static int row_positions(void) { return 1 + (view == V_FLOAT ? 0 : 2 * ROW_BYTES); }

static bool row_cursor(menu_item_t *item, const char *line, int *start, int *len)
{
  if (menu_editing != item)
    return false;
  if (edit_pos == 0) {
    *start = 0;
    *len = ADDR_DIGITS;
  } else {
    int digit = edit_pos - 1;
    int cell_digits = 2 * view_size[view];
    *start = cell_col(digit / cell_digits) + digit % cell_digits;
    *len = 1;
  }
  return true;
}

static void change_digit(uintptr_t row, int dir)
{
  int digit = edit_pos - 1;
  int size = view_size[view];
  int cell_digits = 2 * size;
  uintptr_t addr = row + (uintptr_t)(digit / cell_digits * size);
  int shift = (cell_digits - 1 - digit % cell_digits) * DIGIT_BITS;
  uint64_t value;
  if (!read_cell(addr, size, &value))
    return;
  value = step_digit(value, shift, dir);
  if (!sys_mapped(addr, (size_t)size, SYS_PERM_R | SYS_PERM_W)) {
    gfx_notice("that memory is read-only");
    return;
  }
  memcpy((void *)addr, &value, (size_t)size);
}

/* gz's row button: a memory watch on the row's address, as the current view's type */
static void watch_row(uintptr_t row)
{
  if (watch_add_memory(row, view_watch_type[view])) {
    gfx_logf("added watch %0*lx", ADDR_DIGITS, (unsigned long)row);
    if (watches_menu)
      menu_goto(watches_menu);
  } else {
    gfx_notice("that address is already watched");
  }
}

/* the pointer view: go where the pointer under the cursor points */
static void follow_pointer(uintptr_t row)
{
  int cell = (edit_pos - 1) / (2 * POINTER_BYTES);
  uint64_t target;
  if (read_cell(row + (uintptr_t)(cell * POINTER_BYTES), POINTER_BYTES, &target) &&
      sys_mapped((uintptr_t)target, 1, SYS_PERM_R)) {
    show((uintptr_t)target);
    gfx_logf("memory %0*lx", ADDR_DIGITS, (unsigned long)view_addr);
  } else {
    gfx_notice("that doesn't point anywhere readable");
  }
}

static bool row_edit(menu_item_t *item)
{
  uintptr_t row = view_addr + (uintptr_t)item->data * ROW_BYTES;
  int n_positions = row_positions();
  if (edit_pos >= n_positions) /* the view changed to one with fewer */
    edit_pos = n_positions - 1;
  if (input_repeat(BTN_DLEFT)) {
    edit_pos = edit_pos > 0 ? edit_pos - 1 : 0;
  } else if (input_repeat(BTN_DRIGHT)) {
    edit_pos = edit_pos < n_positions - 1 ? edit_pos + 1 : n_positions - 1;
  } else if (edit_pos > 0 && (input_repeat(BTN_DUP) || input_repeat(BTN_DDOWN))) {
    change_digit(row, input_repeat(BTN_DUP) ? 1 : -1);
  } else if (input_pressed() & BTN_L) {
    if (edit_pos == 0)
      watch_row(row);
    else if (view == V_PTR)
      follow_pointer(row);
    return false;
  }
  return true;
}

static const char *const arrow_hints[] = { "L: scroll up a row (held: keeps going)",
                                           "L: scroll down a row (held: keeps going)" };

/* the up arrow is column 0 of the header, the down arrow column 2 */
static bool arrows_cursor(menu_item_t *item, const char *line, int *start, int *len)
{
  *start = item->part ? 2 : 0;
  *len = 1;
  return true;
}

static void arrows_press(menu_item_t *item, int part)
{
  if (part == 0)
    view_addr = view_addr >= ROW_BYTES ? view_addr - ROW_BYTES : 0;
  else
    view_addr += ROW_BYTES;
}

static void act_row(menu_item_t *item)
{
  edit_pos = 0;
  menu_edit(item);
}

/* --- menu ----------------------------------------------------------------------------------------- */

#define MENU_ITEMS (ROWS + 6) /* the rows, and address, view, go to, arrows, return (one spare) */

static int first_row;

void memview_build(menu_t *watches)
{
  watches_menu = watches;
  for (int i = 0; i < N_PLACES; i++)
    place_names[i] = places[i].name;
  show(z64hd_main);
  menu_init(&memview_menu, "memory", MENU_ITEMS);

  menu_item_t *address = menu_add_action(&memview_menu, "", act_address, 0);
  if (address) {
    address->text = address_text;
    address->on_change = page;
    address->cursor = address_cursor;
    address->edit = address_edit;
    address->hint = "L: type an address   d-left/right: a page back/on";
    address->edit_hint = "d-left/right: digit   d-up/down: change   L: go";
  }

  menu_add_option(&memview_menu, "view", V_N, view_names, get_view, set_view, 0);
  menu_item_t *go_to = menu_add_option(&memview_menu, "go to", N_PLACES, place_names, get_place, set_place, 0);
  if (go_to)
    go_to->hint = "d-left/right: jump there";

  menu_item_t *arrows = menu_add_action(&memview_menu, "", 0, 0); /* gz's arrows, on the header line */
  if (arrows) {
    arrows->text = header_text;
    arrows->cursor = arrows_cursor;
    arrows->press = arrows_press;
    arrows->press_repeat = true;
    arrows->n_parts = 2;
    arrows->part_hints = arrow_hints;
  }

  first_row = memview_menu.n_items;
  for (int r = 0; r < ROWS; r++) {
    menu_item_t *row = menu_add_action(&memview_menu, "", act_row, r);
    if (row) {
      row->text = row_text;
      row->on_change = page;
      row->cursor = row_cursor;
      row->edit = row_edit;
      row->hint = "L: edit this row   d-left/right: a page back/on";
      row->edit_hint = "d-l/r: move  d-u/d: change  L on address: watch it";
    }
  }
  menu_add_return(&memview_menu);
}

void memview_open(uintptr_t addr, int mw_type)
{
  int size = mw_type >= 0 && mw_type < MW_NTYPES ? memory_type_size[mw_type] : 1;
  if (size == 1)
    view = V_BYTE;
  else if (size == 2)
    view = V_HALF;
  else if (size == 8)
    view = V_PTR;
  else if (mw_type == MW_F32)
    view = V_FLOAT;
  else
    view = V_WORD;
  show(addr);
  menu_goto(&memview_menu);
  memview_menu.selected = first_row;
}
