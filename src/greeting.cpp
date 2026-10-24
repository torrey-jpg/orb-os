// Kevin's Orb — the startup greeting. See greeting.h.
#include "greeting.h"
#include "orb_time.h"
#include "theme_sd.h"
#include "warp.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#else
#include <cstdlib>
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
static void heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif
#include <PNGdec.h>
#include <new>
#include <stdio.h>
#include <string.h>

namespace {

// ---- look --------------------------------------------------------------------------------
const uint32_t COL_ENAMEL = 0xF4F1E8;
const uint32_t COL_MINT   = 0x5FE3C0;
const uint32_t COL_GREY   = 0x8A9A9A;
const uint32_t COL_HINT   = 0x4A5A58;

// ---- timeline (ms from the first frame) --------------------------------------------------
const uint32_t T_SCAN_END   = 600;    // scan line top -> bottom
const uint32_t T_RING       = 250;    // ring starts drawing
const uint32_t T_RING_END   = 1300;
const uint32_t T_LINE1      = 450;    // "GOOD MORNING" starts typing
const uint32_t LINE1_CHAR   = 45;
const uint32_t T_NAME       = 1050;   // the name starts typing
const uint32_t NAME_CHAR    = 85;
const uint32_t T_DATE       = 2050;   // date + credit fade in
const uint32_t T_WARP       = 3300;   // hand over to the warp
const uint32_t T_PHOTO      = 300;    // the photo fades in
const uint32_t T_PHOTO_END  = 900;
const uint32_t FRAME_MS     = 16;

// Kevin's photo: a PNG on the SD card, already cut to a circle with a transparent outside
// (tools/greeting/make_photo.py). Optional; without it the greeting is text only.
const char  *PHOTO_PATH = "/greeting/photo.png";
const size_t PHOTO_MAX  = 512 * 1024;

lv_obj_t   *s_cont   = nullptr;
lv_obj_t   *s_scan   = nullptr;
lv_obj_t   *s_ring   = nullptr;
lv_obj_t   *s_line1  = nullptr;
lv_obj_t   *s_name   = nullptr;
lv_obj_t   *s_cursor = nullptr;
lv_obj_t   *s_date   = nullptr;
lv_obj_t   *s_credit = nullptr;
lv_obj_t   *s_photo  = nullptr;
lv_obj_t   *s_photoRing = nullptr;
uint8_t    *s_photoBuf = nullptr;     // RGB565 + alpha, 3 bytes a pixel
lv_img_dsc_t s_photoDsc;
lv_timer_t *s_timer  = nullptr;
uint32_t    s_t0     = 0;
bool        s_started = false;
bool        s_warped  = false;
void      (*s_gone)() = nullptr;

char s_hello[24];
char s_dateText[40];

const char *DAYS[]   = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
const char *MONTHS[] = { "January", "February", "March", "April", "May", "June", "July",
                         "August", "September", "October", "November", "December" };

// Read the clock when the greeting actually starts, which on the device is after WiFi and
// the RTC have had their chance to set it.
void compose_text() {
    struct tm ti;
    if (orb_local_time(&ti)) {
        const int h = ti.tm_hour;
        strcpy(s_hello, h < 12 ? "GOOD MORNING" : (h < 17 ? "GOOD AFTERNOON" : "GOOD EVENING"));
        snprintf(s_dateText, sizeof s_dateText, "%s, %s %d",
                 DAYS[ti.tm_wday % 7], MONTHS[ti.tm_mon % 12], ti.tm_mday);
    } else {
        strcpy(s_hello, "WELCOME");
        s_dateText[0] = '\0';
    }
}

// Show the first `n` characters of `full` on `label`.
void type_on(lv_obj_t *label, const char *full, int n) {
    char buf[48];
    const int len = (int)strlen(full);
    if (n > len) n = len;
    if (n < 0) n = 0;
    memcpy(buf, full, (size_t)n);
    buf[n] = '\0';
    lv_label_set_text(label, buf);
}

lv_obj_t *make_label(const lv_font_t *font, uint32_t color, int y) {
    lv_obj_t *l = lv_label_create(s_cont);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

// ---- the photo --------------------------------------------------------------------------
int s_photoW = 0;
int photo_line(PNGDRAW *d) {
    const uint8_t *src = d->pPixels;
    uint8_t *dst = s_photoBuf + (size_t)d->y * s_photoW * 3;
    const bool rgba = d->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA;
    for (int x = 0; x < d->iWidth; ++x, dst += 3) {
        const uint16_t v = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3));
        dst[0] = v & 0xFF; dst[1] = v >> 8; dst[2] = rgba ? src[3] : 0xFF;
        src += rgba ? 4 : 3;
    }
    return 1;
}

bool load_photo() {
    size_t len = 0;
    uint8_t *file = theme_sd::read_whole(PHOTO_PATH, len, PHOTO_MAX);
    if (!file) return false;
    bool ok = false;
    void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem) {
        PNG *png = new (mem) PNG();
        if (png->openRAM(file, (int)len, photo_line) == PNG_SUCCESS) {
            const int w = png->getWidth(), h = png->getHeight();
            if (w > 0 && h > 0 && w <= 240 && h <= 240) {
                s_photoBuf = (uint8_t *)heap_caps_malloc((size_t)w * h * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (s_photoBuf) {
                    s_photoW = w;
                    ok = png->decode(nullptr, 0) == PNG_SUCCESS;
                    if (ok) {
                        memset(&s_photoDsc, 0, sizeof s_photoDsc);
                        s_photoDsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
                        s_photoDsc.header.w = w;
                        s_photoDsc.header.h = h;
                        s_photoDsc.data_size = (uint32_t)w * h * 3;
                        s_photoDsc.data = s_photoBuf;
                    } else {
                        heap_caps_free(s_photoBuf); s_photoBuf = nullptr;
                    }
                }
            }
            png->close();
        }
        png->~PNG();
        heap_caps_free(mem);
    }
    theme_sd::free(file);
    return ok;
}

void remove_greeting(int) {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    if (s_cont)  { lv_obj_del(s_cont); s_cont = nullptr; }
    s_scan = s_ring = s_line1 = s_name = s_cursor = s_date = s_credit = nullptr;
    s_photo = s_photoRing = nullptr;
    if (s_photoBuf) { heap_caps_free(s_photoBuf); s_photoBuf = nullptr; }
    if (s_gone) s_gone();
}

void tick(lv_timer_t *) {
    if (!s_cont) return;
    const uint32_t now = lv_tick_get();
    if (!s_started) { s_started = true; s_t0 = now; compose_text(); }
    const uint32_t t = now - s_t0;

    // Scan line.
    if (t < T_SCAN_END) {
        const int y = (int)(466 * t / T_SCAN_END);
        lv_obj_set_y(s_scan, y - 2);
        lv_obj_clear_flag(s_scan, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_scan, LV_OBJ_FLAG_HIDDEN);
    }

    // Ring round the edge.
    if (t >= T_RING) {
        // A whole circle is set as 0..360: 270..630 would wrap to 270..270, an empty arc.
        if (t >= T_RING_END) lv_arc_set_angles(s_ring, 0, 360);
        else lv_arc_set_angles(s_ring, 270, (uint16_t)(270 + 360 * (t - T_RING) / (T_RING_END - T_RING)));
    }

    // Photo.
    if (s_photo && t >= T_PHOTO) {
        const uint32_t f = t - T_PHOTO, span = T_PHOTO_END - T_PHOTO;
        const lv_opa_t o = f >= span ? (lv_opa_t)LV_OPA_COVER : (lv_opa_t)(255 * f / span);
        lv_obj_set_style_img_opa(s_photo, o, 0);
        lv_obj_set_style_border_opa(s_photoRing, (lv_opa_t)(o * 3 / 4), 0);
    }

    // Typing.
    if (t >= T_LINE1) type_on(s_line1, s_hello, (int)((t - T_LINE1) / LINE1_CHAR) + 1);
    if (t >= T_NAME) {
        const int n = (int)((t - T_NAME) / NAME_CHAR) + 1;
        type_on(s_name, greeting::NAME, n);
        // A block cursor rides the end of the name, then blinks once it is written.
        const bool typing = n <= (int)strlen(greeting::NAME);
        const bool on = typing || ((t / 400) % 2 == 0);
        lv_obj_update_layout(s_name);
        lv_obj_align_to(s_cursor, s_name, LV_ALIGN_OUT_RIGHT_MID, 6, 2);
        if (on) lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    }

    // Date and credit.
    if (t >= T_DATE) {
        const uint32_t f = t - T_DATE;
        const lv_opa_t o = f >= 500 ? (lv_opa_t)LV_OPA_COVER : (lv_opa_t)(255 * f / 500);
        lv_label_set_text(s_date, s_dateText);
        lv_obj_set_style_text_opa(s_date, o, 0);
        lv_obj_set_style_text_opa(s_credit, o, 0);
    }

    if (t >= T_WARP && !s_warped) {
        s_warped = true;
        lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        warp::start(remove_greeting, 0);
    }
}

}  // namespace

lv_obj_t *greeting::show(void (*gone)()) {
    s_gone = gone;
    s_started = s_warped = false;

    s_cont = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_cont);
    lv_obj_set_size(s_cont, 466, 466);
    lv_obj_center(s_cont);
    lv_obj_set_style_bg_color(s_cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_cont, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_cont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // The ring: an arc that draws itself clockwise from 12 o'clock.
    s_ring = lv_arc_create(s_cont);
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_size(s_ring, 420, 420);
    lv_obj_center(s_ring);
    lv_arc_set_bg_angles(s_ring, 0, 0);
    lv_arc_set_angles(s_ring, 270, 270);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(COL_MINT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_ring, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_ring, LV_OPA_80, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_ring, true, LV_PART_INDICATOR);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);

    // With a photo: photo at the top, text below it. Without: the text sits in the middle.
    const bool photo = load_photo();
    const int yLine1 = photo ? 226 : 150;
    if (photo) {
        const int d = s_photoDsc.header.w;
        s_photoRing = lv_obj_create(s_cont);
        lv_obj_remove_style_all(s_photoRing);
        lv_obj_set_size(s_photoRing, d + 10, d + 10);
        lv_obj_align(s_photoRing, LV_ALIGN_TOP_MID, 0, 205 - d - 5);
        lv_obj_set_style_radius(s_photoRing, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(s_photoRing, lv_color_hex(COL_MINT), 0);
        lv_obj_set_style_border_width(s_photoRing, 2, 0);
        lv_obj_set_style_border_opa(s_photoRing, LV_OPA_TRANSP, 0);
        s_photo = lv_img_create(s_cont);
        lv_img_set_src(s_photo, &s_photoDsc);
        lv_obj_align(s_photo, LV_ALIGN_TOP_MID, 0, 205 - d);
        lv_obj_set_style_img_opa(s_photo, LV_OPA_TRANSP, 0);
    }

    s_line1 = make_label(&lv_font_montserrat_18, COL_MINT, yLine1);
    lv_obj_set_style_text_letter_space(s_line1, 4, 0);

    s_name = make_label(&lv_font_montserrat_44, COL_ENAMEL, yLine1 + 30);

    s_cursor = lv_obj_create(s_cont);
    lv_obj_remove_style_all(s_cursor);
    lv_obj_set_size(s_cursor, 4, 40);
    lv_obj_set_style_bg_color(s_cursor, lv_color_hex(COL_MINT), 0);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);

    s_date = make_label(&lv_font_montserrat_16, COL_GREY, yLine1 + 100);
    lv_obj_set_style_text_opa(s_date, LV_OPA_TRANSP, 0);

    // Zion Brock designed the Orb; a quiet credit stays on the boot screen.
    s_credit = make_label(&lv_font_montserrat_12, COL_HINT, 392);
    lv_label_set_text(s_credit, "Orb by Zion Brock");
    lv_obj_set_style_text_opa(s_credit, LV_OPA_TRANSP, 0);

    // The scan line: a bright 2 px line with a soft glow, swept down once.
    s_scan = lv_obj_create(s_cont);
    lv_obj_remove_style_all(s_scan);
    lv_obj_set_size(s_scan, 466, 3);
    lv_obj_set_pos(s_scan, 0, 0);
    lv_obj_set_style_bg_color(s_scan, lv_color_hex(COL_MINT), 0);
    lv_obj_set_style_bg_opa(s_scan, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(s_scan, lv_color_hex(COL_MINT), 0);
    lv_obj_set_style_shadow_width(s_scan, 24, 0);
    lv_obj_set_style_shadow_opa(s_scan, LV_OPA_60, 0);
    lv_obj_add_flag(s_scan, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(tick, FRAME_MS, nullptr);
    return s_cont;
}
