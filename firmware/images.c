#include "pico/stdlib.h"
#include "animations.h"
#include <string.h>

// Pull generated bitmap headers into this compilation unit so IntelliSense
// resolves stdint types (uint8_t etc.) correctly within the images/ folder.
#if __has_include("images.h")
#include "images.h"
#endif

void show_image(ssd1306_t      *poled,
                const uint8_t  *bitmap,
                int             img_w,
                int             img_h,
                float           scale,
                const char     *overlay_text,
                anim_text_pos_t text_pos) {
    if (scale <= 0.0f) scale = 1.0f;

    // Rendered pixel dimensions after scaling.
    int rend_w = (int)(img_w * scale);
    int rend_h = (int)(img_h * scale);

    // Precompute 1/scale so the inner loop multiplies instead of divides
    // (software float multiply is ~4x faster than divide on Cortex-M0+).
    float inv = 1.0f / scale;

    bool draw_text = (overlay_text != NULL && overlay_text[0] != '\0'
                      && text_pos != ANIM_TEXT_NONE);

    // For TOP/BOTTOM text, anchor text at the display edge and centre the image
    // in the remaining space — avoids the image overrunning the text when large.
    // For MIDDLE/no-text, centre image on the display as before.
    int off_x = (poled->width  - rend_w) / 2;
    int off_y;
    int text_x = 0, text_y = 0;
    const int GAP = 4;

    if (draw_text) {
        int text_w = (int)strlen(overlay_text) * UIEXT_SCROLL_CHAR_PX;
        text_x = ((int)poled->width - text_w) / 2;
        if (text_x < 0) text_x = 0;

        switch (text_pos) {
            case ANIM_TEXT_TOP:
                text_y = 0;
                // Centre image in the space below text + gap.
                {
                    int space_h = (int)poled->height - 8 - GAP;
                    off_y = 8 + GAP + (space_h - rend_h) / 2;
                    if (off_y < 8 + GAP) off_y = 8 + GAP;
                }
                break;
            case ANIM_TEXT_MIDDLE:
                text_y = ((int)poled->height - 8) / 2;
                off_y  = ((int)poled->height - rend_h) / 2;
                break;
            case ANIM_TEXT_BOTTOM:
                text_y = (int)poled->height - 8;
                // Centre image in the space above text + gap.
                {
                    int space_h = text_y - GAP;
                    off_y = (space_h - rend_h) / 2;
                    if (off_y < 0) off_y = 0;
                }
                break;
            default:
                text_y = 0;
                off_y  = ((int)poled->height - rend_h) / 2;
                break;
        }
    } else {
        off_y = ((int)poled->height - rend_h) / 2;
    }

    ssd1306_clear(poled);

    for (int ry = 0; ry < rend_h; ry++) {
        // Map rendered row back to source row via nearest-neighbour.
        int sy = (int)(ry * inv);
        for (int rx = 0; rx < rend_w; rx++) {
            int sx = (int)(rx * inv);
            // SSD1306 page-format bit test:
            //   byte index = sx + (sy / 8) * img_w  (column x, page sy/8)
            //   bit index  = sy % 8                  (pixel row within the page)
            if (bitmap[sx + (sy / 8) * img_w] & (1 << (sy % 8)))
                ssd1306_draw_pixel(poled, off_x + rx, off_y + ry);
        }
    }

    // Overlay text is drawn after bitmap pixels so it always sits on top.
    if (draw_text)
        ssd1306_draw_string(poled, text_x, text_y, 1, overlay_text);

    ssd1306_show(poled);
}

void show_sdcard(ssd1306_t *poled, const char *text) {
    show_image(poled, sdcard_bitmap, SDCARD_W, SDCARD_H, 0.75f, text, ANIM_TEXT_BOTTOM);
}

void show_error(ssd1306_t *poled, const char *text) {
    show_image(poled, error_bitmap, ERROR_W, ERROR_H, 0.75f, text, ANIM_TEXT_BOTTOM);
}
