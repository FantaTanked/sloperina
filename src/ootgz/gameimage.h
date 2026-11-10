/* The game's own pictures: its controllers (Controller_Pro, _Grip and _Console in its Common UI
 * textures) for the input display in the game's style, parts of its menus, and its items' icons. The
 * first are in a 244 MB archive (65 MB packed), the icons each in a file of their own (about 13 MB in
 * all), so they're loaded a little each frame, from the first frame they're wanted, and handed to
 * gpuui.c as textures. Nothing of them is in this repository or the release: they're the user's own
 * files. */
#ifndef GAMEIMAGE_H
#define GAMEIMAGE_H

#include <stdbool.h>

/* the pictures, as gpuui_image's numbers: the controllers in the order of gfx.h's GFX_PAD_ devices, then
 * the parts of the game's menus (masks: white, their red as alpha), all from the shared archive; then the
 * items' icons (GAMEPIC_*, each from its own file in UI/Tex/Icon), loaded after those */
enum {
  GAMEIMAGE_PRO, GAMEIMAGE_GRIP, GAMEIMAGE_CONSOLE,
  GAMEIMAGE_CURSOR_LINE, GAMEIMAGE_CURSOR_GLOW, /* the options' thin cursor: its outline and its glow */
  GAMEIMAGE_LINE,                               /* the system menus' ornament line: a curl, then the line */
  GAMEIMAGE_SCROLL_BAR, GAMEIMAGE_SCROLL_BASE,  /* the scroll bar: its thumb and its track */
  GAMEPIC_HEART,                                /* a heart container (the collection screen's) */
  /* the HUD's magic meter, for the enemies' health: its track, frame, fill (its green made red: red and
   * green swapped) and shadow; and the number plate of the minigames' counters, its plate and its rim */
  GAMEIMAGE_GAUGE_BASE, GAMEIMAGE_GAUGE_FRAME, GAMEIMAGE_GAUGE_FILL, GAMEIMAGE_GAUGE_SHADOW,
  GAMEIMAGE_PLATE, GAMEIMAGE_PLATE_RIM,
  GAMEIMAGES_SHARED,
  GAMEPIC_RUPEE = GAMEIMAGES_SHARED, GAMEPIC_MAGIC, GAMEPIC_STICK, GAMEPIC_NUT, GAMEPIC_SEED, GAMEPIC_BOMB,
  GAMEPIC_ARROW, GAMEPIC_BOMBCHU, GAMEPIC_BEAN, GAMEPIC_BOOMERANG, GAMEPIC_SLINGSHOT, GAMEPIC_BOW, GAMEPIC_HAMMER,
  GAMEPIC_ARROW_FIRE, GAMEPIC_ARROW_ICE, GAMEPIC_ARROW_LIGHT, GAMEPIC_HOOKSHOT, GAMEPIC_DIN, GAMEPIC_FARORE,
  GAMEPIC_NAYRU, GAMEPIC_OCARINA_FAIRY, GAMEPIC_OCARINA_TIME, GAMEPIC_BOTTLE, GAMEPIC_SWORD_KOKIRI,
  GAMEPIC_SWORD_MASTER, GAMEPIC_SWORD_GIANT, GAMEPIC_SHIELD_DEKU, GAMEPIC_SHIELD_HYLIAN, GAMEPIC_SHIELD_MIRROR,
  GAMEPIC_TUNIC_GORON, GAMEPIC_TUNIC_ZORA, GAMEPIC_BOOTS_IRON, GAMEPIC_BOOTS_HOVER, GAMEPIC_LENS, GAMEPIC_AGONY,
  GAMEPIC_GERUDO_CARD, GAMEPIC_BRACELET, GAMEPIC_GAUNTLETS, GAMEPIC_SCALE, GAMEPIC_SKULLTULA, GAMEPIC_DEFENSE,
  GAMEPIC_WALLET, GAMEPIC_BOMB_BAG, GAMEPIC_QUIVER, GAMEPIC_BULLET_BAG, GAMEPIC_STICK_BAG, GAMEPIC_NUT_BAG,
  GAMEPIC_LONGSHOT, GAMEPIC_SWORD_BROKEN, GAMEPIC_SWORD_BIGGORON, GAMEPIC_GAUNTLETS_GOLD, GAMEPIC_SCALE_GOLD,
  GAMEPIC_MAGIC_DOUBLE, GAMEPIC_POTION_GREEN, GAMEPIC_SCRUB_SEED,
  GAMEIMAGES
};

/* render thread, once gpuui_icon_ready(slot): the part of an icon's slot its picture covers (its alpha), as
 * fractions of the slot: x0, y0, x1, y1 */
void gameimage_icon_box(int slot, float box[4]);

/* game thread, every frame they're wanted: loads some more (a few milliseconds' worth) until they're
 * all there, or it fails (logged once; the input display keeps its glyphs) */
void gameimage_frame(void);

/* true once loading them has failed: none of the pictures will come (the menu then leaves no room for them) */
bool gameimage_failed(void);

#endif
