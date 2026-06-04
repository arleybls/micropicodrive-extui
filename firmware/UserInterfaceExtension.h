#ifndef USERINTERFACEEXTENSION_H
#define USERINTERFACEEXTENSION_H

// -- Display geometry ----------------------------------------------------------
#define UIEXT_DISPLAY_WIDTH   64
#define UIEXT_DISPLAY_HEIGHT  32

// -- Long-press continuous scroll ---------------------------------------------
#define UIEXT_LONG_PRESS_MS        500
#define UIEXT_LONG_PRESS_REPEAT_MS  80

// -- Menu layout ---------------------------------------------------------------
#define UIEXT_MENU_TEXT_X      2
#define UIEXT_MENU_ROW_TOP     2
#define UIEXT_MENU_ROW_PITCH  10
#define UIEXT_SCROLL_CHAR_PX   6
// = floor((UIEXT_DISPLAY_WIDTH - UIEXT_MENU_TEXT_X) / UIEXT_SCROLL_CHAR_PX)
// = floor((64 - 2) / 6) = 10
#define UIEXT_MENU_ITEM_MAX_CHARS  10
#define UIEXT_MENU_CHAR_HEIGHT  8

// -- Horizontal idle-scroll animation -----------------------------------------
#define UIEXT_SCROLL_ANIM_ENABLED   1
#define UIEXT_SCROLL_ANIM_IDLE_MS   2000
#define UIEXT_SCROLL_ANIM_STEP_MS   60
#define UIEXT_SCROLL_MAX_TEXT_CHARS  42
#define UIEXT_SCROLL_SCRATCH_COLS    (UIEXT_SCROLL_MAX_TEXT_CHARS * UIEXT_SCROLL_CHAR_PX)  // 252

// -- Select blink animation ----------------------------------------------------
#define UIEXT_SELECT_BLINK_COUNT  2
#define UIEXT_SELECT_BLINK_MS     80

// -- Long-press SELECT sentinel (returned by uiext_run_menu) -------------------
#define UIEXT_LONG_SELECT  (-2)

// -- Vertical navigation-scroll animation -------------------------------------
#define UIEXT_VSCROLL_ANIM_ENABLED   1
#define UIEXT_VSCROLL_STEP_MS       20
#define UIEXT_VSCROLL_SCRATCH_PAGES  ((UIEXT_DISPLAY_HEIGHT / 8) + 2)   // 6 for 32-px display

// -- Public API ----------------------------------------------------------------
#include "ssd1306/ssd1306.h"

void format_menu_item(const char *src, char *dst);
void uiext_draw_menu(char **menu_items, int menu_size, ssd1306_t *poled, int offset);
int  uiext_run_menu (char **items,      int count,     ssd1306_t *poled, int *offset);

#endif // USERINTERFACEEXTENSION_H
