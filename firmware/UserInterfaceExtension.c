#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "ssd1306/ssd1306.h"
#include <stdlib.h>
#include <string.h>
#include "UserInterfaceExtension.h"
#include "animations.h"
#include "UserInterface.h"

// Buttons in the current project are active-LOW with pull-ups.
// BUTTON_PRESSED(pin) is defined in UserInterface.c as (!gpio_get(pin)).
// This file uses the same macro via UserInterface.h.

int cmp_menu_item(const void *a, const void *b) {
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    int a_bracket = (sa[0] == '[');
    int b_bracket = (sb[0] == '[');
    if (a_bracket != b_bracket) return b_bracket - a_bracket;
    if (a_bracket && b_bracket) return strcmp(sa, sb);
    int a_is_file = (strchr(sa, '.') != NULL);
    int b_is_file = (strchr(sb, '.') != NULL);
    if (a_is_file != b_is_file) return a_is_file - b_is_file;
    return strcmp(sa, sb);
}

// Formats a raw menu string into a display label:
//   [bracket]  → copied unchanged
//   no-dot     → wrapped in < >, clipped to ".." if too long
//   .ext, fits → copied unchanged
//   .ext, long → base truncated, ".." + original extension appended
// dst must be at least UIEXT_MENU_ITEM_MAX_CHARS + 3 bytes.
void format_menu_item(const char *src, char *dst) {
    if (src[0] == '[') { strcpy(dst, src); return; }
    if (strchr(src, '.') == NULL) {
        int max_name = UIEXT_MENU_ITEM_MAX_CHARS - 2;
        dst[0] = '<';
        if ((int)strlen(src) <= max_name) {
            strcpy(dst + 1, src);
            strcat(dst, ">");
        } else {
            int clip_len = UIEXT_MENU_ITEM_MAX_CHARS - 4;
            strncpy(dst + 1, src, clip_len);
            dst[1 + clip_len] = '\0';
            strcat(dst, "..>");
        }
        return;
    }
    if ((int)strlen(src) <= UIEXT_MENU_ITEM_MAX_CHARS) { strcpy(dst, src); return; }
    const char *ext = strrchr(src, '.') + 1;
    int suffix_len = 2 + (int)strlen(ext);
    int clip_len   = UIEXT_MENU_ITEM_MAX_CHARS - suffix_len;
    if (clip_len < 0) clip_len = 0;
    strncpy(dst, src, (size_t)clip_len);
    dst[clip_len] = '\0';
    strcat(dst, "..");
    strcat(dst, ext);
}

#if UIEXT_SCROLL_ANIM_ENABLED
static bool is_item_clipped(const char *src) {
    if (src[0] == '[') return false;
    if (strchr(src, '.') != NULL)
        return (int)strlen(src) > UIEXT_MENU_ITEM_MAX_CHARS;
    return (int)strlen(src) > (UIEXT_MENU_ITEM_MAX_CHARS - 2);
}

static void full_display_text(const char *src, char *dst, int dst_size) {
    if (src[0] != '[' && strchr(src, '.') == NULL) {
        dst[0] = '<';
        int name_len = (int)strlen(src);
        int max_copy = dst_size - 3;
        if (name_len > max_copy) name_len = max_copy;
        memcpy(dst + 1, src, (size_t)name_len);
        dst[1 + name_len] = '>';
        dst[2 + name_len] = '\0';
    } else {
        strncpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

static uint8_t scroll_scratch[UIEXT_SCROLL_SCRATCH_COLS];


// Copies a column of pixels from the scroll scratch buffer to the display buffer, applying the necessary vertical offset.
static void copy_scratch_col(ssd1306_t *p, int dx, uint8_t col_byte, int row_y) {
    for (int r = 0; r < 8; r++) {
        int disp_y = row_y + r;
        if ((uint32_t)disp_y >= p->height) break;
        uint8_t *cell = &p->buffer[dx + p->width * (disp_y >> 3)];
        uint8_t  mask = (uint8_t)(1u << (disp_y & 7));
        if ((col_byte >> r) & 1u) *cell |=  mask;
        else                       *cell &= ~mask;
    }
}
#endif

static void invert_rect(ssd1306_t *p, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    for (uint32_t py = y; py < y + h; py++)
        for (uint32_t px = x; px < x + w; px++)
            p->buffer[(py / 8) * p->width + px] ^= (1 << (py % 8));
}

#if UIEXT_SCROLL_ANIM_ENABLED
static void draw_hscroll_frame(char **items, int menu_size, ssd1306_t *poled,
                                const char *original_text, int scroll_px, int offset) {
    ssd1306_clear(poled);
    int items_left = menu_size - offset;
    for (int i = 0; i < 3 && i < items_left; i++) {
        if (i == 1) continue;
        char label[UIEXT_MENU_ITEM_MAX_CHARS + 3];
        format_menu_item(items[i + offset], label);
        ssd1306_draw_string(poled, UIEXT_MENU_TEXT_X, UIEXT_MENU_ROW_TOP + i * UIEXT_MENU_ROW_PITCH, 1, label);
    }

    memset(scroll_scratch, 0, UIEXT_SCROLL_SCRATCH_COLS);
    ssd1306_t scratch;
    scratch.width   = (uint8_t)UIEXT_SCROLL_SCRATCH_COLS;
    scratch.height  = 8;
    scratch.pages   = 1;
    scratch.buffer  = scroll_scratch;
    scratch.bufsize = UIEXT_SCROLL_SCRATCH_COLS;
    ssd1306_draw_string(&scratch, 0, 0, 1, original_text);

    const int row_y    = UIEXT_MENU_ROW_TOP + UIEXT_MENU_ROW_PITCH;
    const int visible_w = (int)poled->width - UIEXT_MENU_TEXT_X;
    for (int i = 0; i < visible_w; i++) {
        int sx = scroll_px + i;
        uint8_t col = (sx < UIEXT_SCROLL_SCRATCH_COLS) ? scroll_scratch[sx] : 0u;
        copy_scratch_col(poled, UIEXT_MENU_TEXT_X + i, col, row_y);
    }

    if (items_left > 1)
        invert_rect(poled, 0, (uint32_t)row_y, poled->width, UIEXT_MENU_CHAR_HEIGHT);

    ssd1306_show(poled);
}
#endif

#if UIEXT_VSCROLL_ANIM_ENABLED
static uint8_t vscroll_scratch[UIEXT_DISPLAY_WIDTH * UIEXT_VSCROLL_SCRATCH_PAGES];

static void apply_vscroll_window(ssd1306_t *dst, int read_y, int invert_y) {
    const int W    = (int)dst->width;
    const int SPGS = (int)UIEXT_VSCROLL_SCRATCH_PAGES;
    const int DPGS = (int)dst->pages;

    for (int x = 0; x < W; x++) {
        uint64_t col = 0;
        for (int pg = 0; pg < SPGS; pg++)
            col |= (uint64_t)vscroll_scratch[x + W * pg] << (pg * 8);

        uint32_t dcol = (uint32_t)(col >> (unsigned)read_y);
        for (int pg = 0; pg < DPGS; pg++)
            dst->buffer[x + W * pg] = (uint8_t)(dcol >> (pg * 8));
    }

    invert_rect(dst, 0, (uint32_t)invert_y, dst->width, UIEXT_MENU_CHAR_HEIGHT);
}

static void do_vscroll(char **items, int item_count, ssd1306_t *poled,
                        int new_offset, int dir) {
    int scratch_start      = (dir > 0) ? (new_offset - 1) : new_offset;
    int selected_scratch_y = (dir > 0)
        ? (UIEXT_MENU_ROW_TOP + 2 * UIEXT_MENU_ROW_PITCH)
        : (UIEXT_MENU_ROW_TOP + 1 * UIEXT_MENU_ROW_PITCH);

    memset(vscroll_scratch, 0, sizeof(vscroll_scratch));
    ssd1306_t scratch;
    scratch.width   = (uint8_t)UIEXT_DISPLAY_WIDTH;
    scratch.height  = (uint8_t)(UIEXT_VSCROLL_SCRATCH_PAGES * 8);
    scratch.pages   = (uint8_t)UIEXT_VSCROLL_SCRATCH_PAGES;
    scratch.buffer  = vscroll_scratch;
    scratch.bufsize = (size_t)(UIEXT_DISPLAY_WIDTH * UIEXT_VSCROLL_SCRATCH_PAGES);

    for (int r = 0; r < 4; r++) {
        int idx = scratch_start + r;
        if (idx < 0 || idx >= item_count) continue;
        char label[UIEXT_MENU_ITEM_MAX_CHARS + 3];
        format_menu_item(items[idx], label);
        ssd1306_draw_string(&scratch, UIEXT_MENU_TEXT_X,
                            UIEXT_MENU_ROW_TOP + r * UIEXT_MENU_ROW_PITCH, 1, label);
    }

    for (int anim_px = 1; anim_px <= UIEXT_MENU_ROW_PITCH; anim_px++) {
        int read_y   = (dir > 0) ? anim_px : (UIEXT_MENU_ROW_PITCH - anim_px);
        int invert_y = selected_scratch_y - read_y;

        apply_vscroll_window(poled, read_y, invert_y);
        ssd1306_show(poled);
        sleep_ms(UIEXT_VSCROLL_STEP_MS);
    }
}
#endif

static void do_select_blink(ssd1306_t *poled) {
    for (int i = 0; i < UIEXT_SELECT_BLINK_COUNT; i++) {
        invert_rect(poled, 0, UIEXT_MENU_ROW_TOP + UIEXT_MENU_ROW_PITCH,
                    poled->width, UIEXT_MENU_CHAR_HEIGHT);
        ssd1306_show(poled);
        sleep_ms(UIEXT_SELECT_BLINK_MS);
        invert_rect(poled, 0, UIEXT_MENU_ROW_TOP + UIEXT_MENU_ROW_PITCH,
                    poled->width, UIEXT_MENU_CHAR_HEIGHT);
        ssd1306_show(poled);
        sleep_ms(UIEXT_SELECT_BLINK_MS);
    }
}

void uiext_draw_menu(char **menu_items, int menu_size, ssd1306_t *poled, int offset) {
    ssd1306_clear(poled);
    int items_left = menu_size - offset;
    for (int i = 0; i < 3 && i < items_left; i++) {
        char label[UIEXT_MENU_ITEM_MAX_CHARS + 3];
        format_menu_item(menu_items[i + offset], label);
        ssd1306_draw_string(poled, UIEXT_MENU_TEXT_X, UIEXT_MENU_ROW_TOP + i * UIEXT_MENU_ROW_PITCH, 1, label);
    }
    if (items_left > 1)
        invert_rect(poled, 0, UIEXT_MENU_ROW_TOP + UIEXT_MENU_ROW_PITCH, poled->width, UIEXT_MENU_CHAR_HEIGHT);
    ssd1306_show(poled);
}

// Waits to classify a button press as long or short.
// Returns true (long) if still held after UIEXT_LONG_PRESS_MS.
// Returns false (short) after button is released; waits 200 ms settling.
// Buttons are active-LOW: pressed = gpio_get(pin) == 0.
static bool wait_press_type(uint btn) {
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (BUTTON_PRESSED(btn)) {
        if ((to_ms_since_boot(get_absolute_time()) - t0) >= (uint32_t)UIEXT_LONG_PRESS_MS)
            return true;
        sleep_ms(10);
    }
    sleep_ms(200);
    return false;
}

// Returns the 1-based index of the selected item (offset + 1) when SELECT is
// pressed, or 0 when BACK is short-pressed at the top (offset == 0), which
// signals "select the nav/escape item at index 0".
int uiext_run_menu(char **items, int count, ssd1306_t *poled, int *offset) {
#if UIEXT_SCROLL_ANIM_ENABLED
    uint32_t last_input_ms = to_ms_since_boot(get_absolute_time());
    int scroll_px  = 0;
    int scroll_dir = 1;
#endif

    while (true) {
        if (BUTTON_PRESSED(PIN_BTN_BACK)) {
            bool lp = wait_press_type(PIN_BTN_BACK);
#if UIEXT_SCROLL_ANIM_ENABLED
            last_input_ms = to_ms_since_boot(get_absolute_time());
            scroll_px = 0; scroll_dir = 1;
#endif
            if (lp) {
                while (BUTTON_PRESSED(PIN_BTN_BACK)) {
                    if (*offset > 0) { (*offset)--; uiext_draw_menu(items, count, poled, *offset); }
                    sleep_ms(UIEXT_LONG_PRESS_REPEAT_MS);
                }
#if UIEXT_SCROLL_ANIM_ENABLED
                last_input_ms = to_ms_since_boot(get_absolute_time());
#endif
            } else {
                if (*offset > 0) {
                    (*offset)--;
#if UIEXT_VSCROLL_ANIM_ENABLED
                    do_vscroll(items, count, poled, *offset, -1);
#else
                    uiext_draw_menu(items, count, poled, *offset);
#endif
                } else {
                    return 0;
                }
            }
        }

        if (BUTTON_PRESSED(PIN_BTN_NEXT)) {
            bool lp = wait_press_type(PIN_BTN_NEXT);
#if UIEXT_SCROLL_ANIM_ENABLED
            last_input_ms = to_ms_since_boot(get_absolute_time());
            scroll_px = 0; scroll_dir = 1;
#endif
            if (lp) {
                while (BUTTON_PRESSED(PIN_BTN_NEXT)) {
                    if (*offset < count - 2) { (*offset)++; uiext_draw_menu(items, count, poled, *offset); }
                    sleep_ms(UIEXT_LONG_PRESS_REPEAT_MS);
                }
#if UIEXT_SCROLL_ANIM_ENABLED
                last_input_ms = to_ms_since_boot(get_absolute_time());
#endif
            } else {
                if (*offset < count - 2) {
                    (*offset)++;
#if UIEXT_VSCROLL_ANIM_ENABLED
                    do_vscroll(items, count, poled, *offset, +1);
#else
                    uiext_draw_menu(items, count, poled, *offset);
#endif
                }
            }
        }

        if (BUTTON_PRESSED(PIN_BTN_SELECT)) {
            uint32_t t0 = to_ms_since_boot(get_absolute_time());
            bool long_detected = false;
            while (BUTTON_PRESSED(PIN_BTN_SELECT)) {
                if (!long_detected &&
                    (to_ms_since_boot(get_absolute_time()) - t0) >= (uint32_t)UIEXT_LONG_PRESS_MS) {
                    long_detected = true;
                    // Preview at threshold: add * for untagged files, remove * for already-tagged
                    char *item = items[*offset + 1];
                    if (item[0] != '[' && strrchr(item, '.') != NULL) {
                        if (item[0] == '*') {
                            memmove(item, item + 1, strlen(item));  // remove *
                        } else {
                            memmove(item + 1, item, strlen(item) + 1);  // prepend *
                            item[0] = '*';
                        }
                    }
                    uiext_draw_menu(items, count, poled, *offset);
                }
                sleep_ms(10);
            }
            sleep_ms(200);
#if UIEXT_SCROLL_ANIM_ENABLED
            last_input_ms = to_ms_since_boot(get_absolute_time());
            scroll_px = 0; scroll_dir = 1;
#endif
            if (long_detected)
                return UIEXT_LONG_SELECT;
            do_select_blink(poled);
            return *offset + 1;
        }

#if UIEXT_SCROLL_ANIM_ENABLED
        {
            uint32_t now_ms     = to_ms_since_boot(get_absolute_time());
            int      items_left = count - *offset;

            if (items_left > 1 &&
                (now_ms - last_input_ms) >= (uint32_t)UIEXT_SCROLL_ANIM_IDLE_MS) {

                const char *sel = items[*offset + 1];
                if (is_item_clipped(sel)) {
                    char full_text[UIEXT_SCROLL_MAX_TEXT_CHARS + 3];
                    full_display_text(sel, full_text, (int)sizeof(full_text));

                    int text_px_width = (int)strlen(full_text) * UIEXT_SCROLL_CHAR_PX;
                    int visible_px    = (int)poled->width - UIEXT_MENU_TEXT_X;
                    int max_scroll_px = text_px_width - visible_px;

                    draw_hscroll_frame(items, count, poled, full_text, scroll_px, *offset);

                    scroll_px += scroll_dir;
                    if (scroll_px >= max_scroll_px) { scroll_px = max_scroll_px; scroll_dir = -1; }
                    else if (scroll_px <= 0)         { scroll_px = 0;            scroll_dir =  1; }

                    sleep_ms(UIEXT_SCROLL_ANIM_STEP_MS);
                }
            }
        }
#endif
    }
}
