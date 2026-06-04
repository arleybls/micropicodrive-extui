#include "pico/stdlib.h"
#include "animations.h"
#include <string.h>


void play_animation(ssd1306_t *poled,
                    const uint8_t  *frames,
                    int             count,
                    int             frame_w,
                    int             frame_h,
                    const uint16_t *delays,
                    int             speed_ms,
                    float           scale,
                    const char     *overlay_text,
                    anim_text_pos_t text_pos) {
    if (scale <= 0.0f) scale = 1.0f;

    // Bytes per frame: SSD1306 page format packs 8 vertical pixels per byte,
    // so the buffer is (frame_w * frame_h / 8) bytes laid out as
    // frame_w columns per page, frame_h/8 pages tall.
    int frame_size = (frame_w * frame_h) / 8;

    // Rendered pixel dimensions after scaling.
    int rend_w = (int)(frame_w * scale);
    int rend_h = (int)(frame_h * scale);

    // Pixel offset to centre the scaled frame on the display.
    int off_x = (poled->width  - rend_w) / 2;
    int off_y = (poled->height - rend_h) / 2;

    // Precompute 1/scale so the inner loop multiplies instead of divides
    float inv = 1.0f / scale;

    // Resolve text position once before the frame loop.
    bool draw_text = (overlay_text != NULL && overlay_text[0] != '\0'
                      && text_pos != ANIM_TEXT_NONE);
    int text_x = 0, text_y = 0;
    if (draw_text) {
        // Centre horizontally: each glyph is UIEXT_SCROLL_CHAR_PX pixels wide.
        int text_w = (int)strlen(overlay_text) * UIEXT_SCROLL_CHAR_PX;
        text_x = ((int)poled->width - text_w) / 2;
        if (text_x < 0) text_x = 0;
        switch (text_pos) {
            // TOP: place text so its bottom edge is 2 px above the animation.
            // text bottom = text_y + 8; animation top = off_y.
            case ANIM_TEXT_TOP:
                text_y = off_y - 8 - 2;
                if (text_y < 0) text_y = 0;
                break;
            // Font is 8 px tall; shift up by half that to truly centre.
            case ANIM_TEXT_MIDDLE:
                text_y = ((int)poled->height - 8) / 2;
                break;
            // BOTTOM: place text so its top edge is 2 px below the animation.
            // animation bottom = off_y + rend_h; text top = text_y.
            case ANIM_TEXT_BOTTOM:
                text_y = off_y + rend_h + 2;
                if (text_y + 8 > (int)poled->height) text_y = (int)poled->height - 8;
                break;
            default:
                text_y = 0;
                break;
        }
    }

    for (int i = 0; i < count; i++) {
        // Advance the flat frame array by one frame's worth of bytes.
        const uint8_t *frame = frames + i * frame_size;
        ssd1306_clear(poled);

        for (int ry = 0; ry < rend_h; ry++) {
            // Map rendered row back to source row via nearest-neighbour.
            int sy = (int)(ry * inv);
            for (int rx = 0; rx < rend_w; rx++) {
                int sx = (int)(rx * inv);
                // SSD1306 page-format bit test:
                //   byte index = sx + (sy / 8) * frame_w  (column x, page sy/8)
                //   bit index  = sy % 8                   (pixel row within the page)
                if (frame[sx + (sy / 8) * frame_w] & (1 << (sy % 8)))
                    ssd1306_draw_pixel(poled, off_x + rx, off_y + ry);
            }
        }

        // Overlay text is drawn after frame pixels so it always sits on top.
        if (draw_text)
            ssd1306_draw_string(poled, text_x, text_y, 1, overlay_text);

        ssd1306_show(poled);
        sleep_ms(speed_ms > 0 ? (uint32_t)speed_ms : delays[i]);
    }
}

