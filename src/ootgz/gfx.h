/* Text drawing for the menu and watches, on grids of character cells.
 *
 * The game thread fills three panels each frame and publishes them (gfx_frame):
 *   panel 0, top-left: the menu while it's open
 *   panel 1, any corner: the on-screen watches while the menu is closed
 *   panel 2, bottom-right: gz's log, the last few events, with no background, fading out
 *   panel 3, bottom-left: gz's input display, with no background
 * The render thread reads the published panels when the game presents a frame and copies
 * them into the picture (nvn.c). Until that has worked once, the menu also goes to the log. */
#ifndef GFX_H
#define GFX_H

#include <stdbool.h>
#include <stdint.h>

#define GFX_COLS 64
#define GFX_ROWS 26
#define GFX_PANELS 4
#define GFX_PANEL_HUD 1   /* the watches */
#define GFX_PANEL_LOG 2
#define GFX_PANEL_INPUT 3 /* gz's input display: the stick and the buttons held (bottom-left unless moved) */
#define GFX_LOG_MAX 4 /* lines, as gz's SETTINGS_LOG_MAX */
#define GFX_LOG_ROWS 6 /* the log panel's rows: the log, or the start-up splash */
#define GFX_HINT_LEN 256

/* gz's icons, as characters. The two-cell ones are followed by GFX_WIDE_REST.
 *   GFX_CHECK_OFF / GFX_CHECK_ON: the checkbox (res/gz/checkbox.png), two cells, in the cell's style
 *     colour, with gz's green tick when on
 *   GFX_ICON_REMOVE: the red cross of a watch row (list_icons.png), one cell
 *   GFX_ICON_FOLDER: the yellow folder (file_icons.png), two cells
 *   GFX_ICON_WRENCH: the wrench of a watch row, "edit in memory" (wrench.png), one cell */
#define GFX_CHECK_OFF '\x01'
#define GFX_CHECK_ON '\x02'
#define GFX_WIDE_REST '\x03'
#define GFX_ICON_REMOVE '\x04'
#define GFX_ICON_FOLDER '\x05'
#define GFX_ICON_WRENCH '\x06'
#define GFX_ICON_UP '\x07'   /* arrow_icons.png, one cell each: move a watch up / down */
#define GFX_ICON_DOWN '\x08'
#define GFX_ICON_ADD '\x09'  /* gz's green + (list_icons.png tile 0): a custom watch */
#define GFX_ICON_RIGHT '\x0B' /* arrow_icons.png tiles 2 and 3 (0x0A is the newline, so skipped) */
#define GFX_ICON_LEFT '\x0C'
#define GFX_ICON_LOCK '\x80' /* single-cell padlock; 0x7F is reserved for invisible panel sizing */
/* the game's picture of an item (the game's style only, nativeui.c): two cells, GFX_PICTURE then its
 * number (gameimage.h's GAMEPIC_*) as GFX_PICTURE_ID(n) */
#define GFX_PICTURE '\x0E'
#define GFX_PICTURE_ID(n) ((char)(0xA0 + (n)))
#define GFX_PICTURE_OF(c) ((int)(unsigned char)(c) - 0xA0)

typedef enum {
  GFX_NORMAL,
  GFX_SELECTED,
  GFX_TITLE,
  GFX_DIM,
  GFX_NOTICE,
  GFX_LOG,
  GFX_FAINT, /* the controller's buttons not held */
  GFX_NSTYLES
} gfx_style_t;
/* What a row of the menu panel is: the game's style (nativeui.c) lays the menu out by it, with its own
 * spacing, dividers and symbols; gz's pixel font draws the grid as it is. */
typedef enum {
  GFX_ROW_TEXT,    /* any other row (and every row of the other panels) */
  GFX_ROW_TITLE,   /* the breadcrumb */
  GFX_ROW_GAP,     /* an empty row between parts (under the title, above the hint) */
  GFX_ROW_HEADING, /* a heading over a group of items */
  GFX_ROW_ITEM,    /* an item that can be selected */
  GFX_ROW_LINE,    /* a line of text in the list that can't be */
  GFX_ROW_HINT,    /* the selected item's hint */
  GFX_ROW_WATCH,   /* a watch: its label in GFX_WATCH_LABEL_W columns, a space, its value */
  GFX_ROW_BUTTONS, /* the input display's row: the buttons' names (and arrows) as the game's glyphs */
  GFX_ROW_SKIP,    /* a row the game's style leaves out (a hint's place in a menu without hints) */
  GFX_ROW_CAPTION, /* a small header over a panel's rows (the watches box's), with a line under it */
  GFX_ROW_KINDS
} gfx_row_kind_t;
#define GFX_ROW_KIND 0x0F  /* the kind's bits */
#define GFX_ROW_OPENS 0x10 /* an item that opens a menu: its line ends " >" */
#define GFX_ROW_GRID 0x20  /* a line kept on the grid, a character a cell (menu_item_t.grid) */
#define GFX_ROW_INDENT 0x40 /* an item under a heading, menu.c's MENU_INDENT cells in (the game's style indents it less) */
#define GFX_WATCH_LABEL_W 12

/* the input display's controller drawing (gfx_input_pad): where its sticks and d-pad are, in font pixels
 * from the text's top-left (nvn.c draws it in gz's style, nativeui.c in the game's), and the bits of
 * gfx_panel_t.clicks */
#define GFX_PAD_LSTICK_X 36
#define GFX_PAD_LSTICK_Y 36
#define GFX_PAD_RSTICK_X 96
#define GFX_PAD_RSTICK_Y 56
#define GFX_PAD_STICK_R 10
#define GFX_PAD_STICK_TRAVEL (GFX_PAD_STICK_R - 3) /* how far the stick's dot moves at full tilt */
#define GFX_PAD_DPAD_X 64
#define GFX_PAD_DPAD_Y 56
enum { GFX_CLICK_L3, GFX_CLICK_R3, GFX_CLICK_DPAD_UP, GFX_CLICK_DPAD_DOWN, GFX_CLICK_DPAD_LEFT, GFX_CLICK_DPAD_RIGHT };
typedef enum {
  GFX_TOP_LEFT, GFX_TOP_RIGHT, GFX_BOTTOM_LEFT, GFX_BOTTOM_RIGHT, GFX_TOP_CENTRE, GFX_BOTTOM_CENTRE,
  GFX_CUSTOM, /* at (x, y), thousandths of the picture: the panel's top-left corner */
  GFX_NPLACES
} gfx_corner_t;

typedef struct {
  char     ch[GFX_ROWS][GFX_COLS];    /* 0 = empty */
  uint8_t  style[GFX_ROWS][GFX_COLS];
  int      rows, cols;                /* used area; 0 = nothing to show */
  uint8_t  corner;
  uint32_t hash;                      /* changes when the content does */
  uint8_t  alpha[GFX_ROWS];           /* the log: each row's text opacity, 0..255 */
  int16_t  x, y;                      /* GFX_CUSTOM: where, in thousandths of the picture */
  bool     clear;                     /* no background (the log; the input display unless it has one) */
  uint8_t  pad;                       /* input display: 1 = the controller drawing (nvn.c pad_pixel) */
  int8_t   sticks[4];                 /* ... left x, y, right x, y, -100..100 */
  uint8_t  clicks;                    /* ... bit 0 L3, bit 1 R3, bits 2..5 d-pad up, down, left, right */
  uint8_t  device;                    /* ... the controller in use: GFX_PAD_PRO, _JOYCONS or _HANDHELD */
  int8_t   yoff;                      /* the text moved down this many font pixels (centred: 1) */
  uint8_t  scale_pct;                 /* drawn at this % of the menu's size (0: the same) */
  uint8_t  kind[GFX_ROWS];            /* the menu: what each row is (gfx_row_kind_t, GFX_ROW_OPENS) */
  char     hint[GFX_HINT_LEN];        /* the menu: its hint as one text, which the game's style wraps itself */
  uint8_t  scroll_row, scroll_rows;   /* the menu: the rows its list scrolls in (0 rows: it doesn't) ... */
  uint16_t scroll_items, scroll_top;  /* ... how many items scroll there, and the first one shown */
} gfx_panel_t;

typedef struct { gfx_panel_t panel[GFX_PANELS]; } gfx_screen_t;

/* game thread: the menu (panel 0) */
void gfx_begin(void);
void gfx_text(int col, int row, gfx_style_t style, const char *s);
void gfx_row(int row, int kind); /* what the row is (gfx_row_kind_t, | GFX_ROW_OPENS) */
void gfx_hint(const char *s);    /* the hint shown in the hint's rows, unwrapped */
/* the list scrolls: `items` items in the rows from `row` on, `rows` of them shown from item `top` */
void gfx_scroll(int row, int rows, int items, int top);
void gfx_end(void);
void gfx_clear(void);            /* menu closed */
/* gz_log: a line in the log at the bottom right, shown a second and then faded out */
void gfx_notice(const char *s);
/* gz's start-up splash, in the log's place (bottom right) for `frames` frames, then faded out: lines
 * (NULL: a blank one), each in its style, indented by its count of columns */
void gfx_splash(const char *const *lines, const uint8_t *styles, const uint8_t *indents, int n, int frames);
void gfx_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern volatile uint32_t gfx_log_count; /* lines logged so far */
/* game thread: watches (panel 1), drawn this frame only */
void gfx_hud(gfx_corner_t corner, int row, gfx_style_t style, const char *s);
void gfx_hud_at(int x_permille, int y_permille); /* for GFX_CUSTOM */
void gfx_hud_caption(gfx_corner_t corner, int row, const char *s); /* a small header over the watches */
/* game thread: the input display (panel 3), drawn this frame only */
void gfx_input(int col, int row, gfx_style_t style, const char *s);
/* the controller drawing, this frame, of the controller in use (the game's style draws its picture of it) */
enum { GFX_PAD_PRO, GFX_PAD_JOYCONS, GFX_PAD_HANDHELD };
void gfx_input_pad(const int8_t sticks[4], uint8_t clicks, int device);
void gfx_input_background(bool on); /* the input display on a background, as the watches */
void gfx_input_scale(int percent);  /* ... at this % of the menu's text size */
void gfx_input_place(gfx_corner_t corner, int x_permille, int y_permille); /* ... where (x, y: GFX_CUSTOM) */
/* set by nvn.c: where each GFX_CUSTOM panel was really put (kept whole on screen), thousandths; -1 not yet */
extern volatile int gfx_custom_x[GFX_PANELS], gfx_custom_y[GFX_PANELS];
/* gz's pause display at the top left: 0 none, 1 paused (two bars), 2 advancing a frame (bar + play) */
extern volatile int gfx_pause_icon;
extern volatile uint32_t gfx_presents; /* frames the game has presented (nvn.c) */
void gfx_frame(void);            /* publish; once per frame after everything has drawn */

/* render thread: the latest published screen, or NULL when there's nothing to show */
const gfx_screen_t *gfx_published(void);
void gfx_release(void); /* render thread: done with it */
/* render thread: a notice (a string literal) for the game thread to show on its next frame */
void gfx_notice_later(const char *s);

/* set by nvn.c once a frame has been drawn on screen */
extern volatile bool gfx_on_screen;
/* set by nvn.c: rows of the menu panel that fit on screen under the HUD's D-pad */
extern volatile int gfx_menu_rows;
/* background opacity of the panels, 0..100 (in gz's style, below 100 costs a read-back and a CPU blend) */
extern volatile int gfx_opacity;
extern volatile int gfx_menu_scale; /* text size: 0 = for the screen, else 1..3 (pixels per font pixel) */
/* how the panels are drawn: in the game's own type (nativeui.c, once the game's fonts are loaded) or
 * in gz's pixel font */
#define GFX_STYLE_GAME 0
#define GFX_STYLE_GZ 1
#define GFX_STYLES 2
extern volatile int gfx_style;

#endif
