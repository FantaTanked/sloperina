/* The menu tree (port of ootmenu.pyw's build_menus). */
#ifndef MENUS_H
#define MENUS_H

#include "menu.h"

extern menu_t menu_root;
void menus_build(void);
void menus_splash(void); /* gz's start-up splash */

/* defined in ootgz.c */
extern bool frame_advance, pause_display;
extern int frame_advance_button; /* 0 L, 1 R */

void watches_sync(void);         /* rebuild the watches menu if the list changed */
void menus_research_frame(void); /* debug > pause research: logging, every frame */

#endif
