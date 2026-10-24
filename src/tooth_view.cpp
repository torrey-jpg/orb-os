// Kevin's Orb — Tooth Explorer. See tooth_view.h.
#include "tooth_view.h"
#include "tooth_data.h"
#include "theme_sd.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include "sdcard.h"
#else
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
static struct { void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } } Serial;
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
static void heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif
#include <PNGdec.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <string>

namespace {

// ---- look ------------------------------------------------------------------------------
// Fixed dental palette on true black (AMOLED black pixels are off). Not themed by Orb
// Studio: this screen is ours, and Studio does not know it exists.
const uint32_t COL_ENAMEL = 0xF4F1E8;   // tooth white, a touch warm
const uint32_t COL_MINT   = 0x5FE3C0;   // accent
const uint32_t COL_GREY   = 0x8A9A9A;   // secondary text
const uint32_t COL_DIM    = 0x2E3B39;   // unlit arch dots
const uint32_t COL_HINT   = 0x4A5A58;

// ---- geometry (466 x 466 round panel) --------------------------------------------------
const int   CX = 233, CY = 233;
const int   ARCH_R    = 210;     // radius of the ring of 32 dots
const int   DOT       = 10;      // unlit dot
const int   DOT_LIT   = 20;      // the current tooth
const float ARCH_SPAN = 140.0f;  // degrees each arch covers (centred on straight up / down)
const int   IMG_Y     = 128;     // top of the tooth picture
const int   TEETH     = 32;

// ---- behaviour -------------------------------------------------------------------------
const uint32_t SPIN_MS       = 110;   // one frame of auto-spin
const uint32_t SPIN_RESUME   = 2500;  // auto-spin waits this long after a drag
const uint32_t LOAD_SETTLE   = 220;   // load a tooth's frames once the knob rests this long
const int      DRAG_PX       = 12;    // finger travel per step of rotation (15 degrees)

// ---- the frame file: /teeth/tNN.bin, written by tools/teeth/render_teeth.py ------------
//   "TTH2" u16 yaw_count u16 pitch_count u16 w u16 h,
//   then yaw_count*pitch_count x (u32 offset, u32 length), frame = pitch*yaw_count + yaw,
//   then the PNGs. ~5-7 MB per tooth: too big to hold, so the table is read once when the
//   tooth changes and then only the one picture on screen is read from the card.
const char  *SHEET_DIR    = "/teeth";
const int    MAX_FRAMES   = 48 * 48;
const uint32_t MAX_FRAME_BYTES = 256 * 1024;

// ---- state -----------------------------------------------------------------------------
// s_num, s_details and s_yaw survive destroy(), so coming back lands where Kevin left off.
int  s_num     = 1;
bool s_details = false;
int  s_yaw     = 0;      // steps around the tooth's long axis
int  s_pitch   = 0;      // steps end over end

lv_obj_t *s_screen   = nullptr;
lv_obj_t *s_number   = nullptr;
lv_obj_t *s_name     = nullptr;
lv_obj_t *s_sub      = nullptr;
lv_obj_t *s_img      = nullptr;
lv_obj_t *s_noimg    = nullptr;
lv_obj_t *s_card     = nullptr;
lv_obj_t *s_cardText = nullptr;
lv_obj_t *s_dots[TEETH] = {};
lv_timer_t *s_timer  = nullptr;

#ifdef ARDUINO
File         s_file;
#else
FILE        *s_file      = nullptr;
#endif
bool         s_sheet     = false;     // a tooth's file is open and its table read
int          s_sheetType = -1;
int          s_frames    = 0;
int          s_nyaw = 0, s_npitch = 0;
uint32_t    *s_table     = nullptr;   // (offset, length) per frame
uint8_t     *s_comp      = nullptr;   // one compressed frame
uint32_t     s_compCap   = 0;
int          s_fw = 0, s_fh = 0;

uint16_t    *s_pix       = nullptr;   // decoded frame shown on screen (RGB565)
lv_img_dsc_t s_dsc;
int          s_shownFrame = -1;
bool         s_shownFlip  = false;

PNG         *s_png      = nullptr;
bool         s_flipNow  = false;      // decode target settings for the line callback

int          s_wantType  = -1;        // type the screen is waiting to load
uint32_t     s_lastTurn  = 0;
bool         s_dragging  = false;
int          s_dragAcc   = 0;
int          s_dragAccY  = 0;
uint32_t     s_lastDrag  = 0;
uint32_t     s_lastSpin  = 0;
bool         s_missing   = false;     // no frames on the card for this tooth

// ---- arch ring --------------------------------------------------------------------------
// Universal chart orientation, viewed facing the patient: #1 at the upper LEFT of the
// dial running across the top to #16, then #17 at the lower RIGHT running back to #32.
void dot_pos(int num, int &x, int &y) {
    const float step = ARCH_SPAN / 15.0f;
    float deg;
    if (num <= 16) deg = (90.0f + ARCH_SPAN / 2) - (num - 1) * step;    // 160 -> 20
    else           deg = (-90.0f + ARCH_SPAN / 2) - (num - 17) * step;  // -20 -> -160
    const float rad = deg * 3.14159265f / 180.0f;
    x = CX + (int)lroundf(ARCH_R * cosf(rad));
    y = CY - (int)lroundf(ARCH_R * sinf(rad));
}

void style_dot(int num, bool lit) {
    lv_obj_t *d = s_dots[num - 1];
    if (!d) return;
    const int size = lit ? DOT_LIT : DOT;
    int x, y;
    dot_pos(num, x, y);
    lv_obj_set_size(d, size, size);
    lv_obj_set_pos(d, x - size / 2, y - size / 2);
    lv_obj_set_style_bg_color(d, lv_color_hex(lit ? COL_MINT : COL_DIM), 0);
    lv_obj_set_style_shadow_width(d, lit ? 16 : 0, 0);
    lv_obj_set_style_shadow_color(d, lv_color_hex(COL_MINT), 0);
    lv_obj_set_style_shadow_opa(d, lit ? LV_OPA_70 : LV_OPA_TRANSP, 0);
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int y, int width) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (width > 0) {
        lv_obj_set_width(l, width);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

// ---- frames -----------------------------------------------------------------------------
// The models are drawn as the patient's right-side teeth. Left-side teeth (#9-#24) are the
// mirror image, so they are the same frames flipped left-to-right.
bool mirrored(int num) { return num >= 9 && num <= 24; }

void close_file() {
#ifdef ARDUINO
    if (s_file) { theme_sd::lock(); s_file.close(); theme_sd::unlock(); }
#else
    if (s_file) { fclose(s_file); s_file = nullptr; }
#endif
}

// Read `len` bytes at `off` of the open tooth file.
bool read_at(uint32_t off, uint8_t *dst, uint32_t len) {
#ifdef ARDUINO
    if (!s_file) return false;
    theme_sd::lock();
    const bool ok = s_file.seek(off) && s_file.read(dst, len) == len;
    theme_sd::unlock();
    return ok;
#else
    if (!s_file) return false;
    return fseek(s_file, (long)off, SEEK_SET) == 0 && fread(dst, 1, len, s_file) == len;
#endif
}

void free_sheet() {
    close_file();
    if (s_table) { heap_caps_free(s_table); s_table = nullptr; }
    if (s_comp)  { heap_caps_free(s_comp);  s_comp = nullptr; s_compCap = 0; }
    s_sheet = false; s_sheetType = -1; s_frames = 0; s_nyaw = s_npitch = 0;
}

void free_frame() {
    if (s_img) lv_img_set_src(s_img, nullptr);
    if (s_pix) { heap_caps_free(s_pix); s_pix = nullptr; }
    s_shownFrame = -1;
}

void free_decoder() {
    if (s_png) { s_png->~PNG(); heap_caps_free(s_png); s_png = nullptr; }
}

uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

bool load_sheet(int type) {
    if (type == s_sheetType && s_sheet) return true;
    free_sheet();
    char path[40];
    snprintf(path, sizeof path, "%s/t%02d.bin", SHEET_DIR, type);
#ifdef ARDUINO
    if (!sdcard::mounted()) { Serial.printf("[tooth] no SD card\n"); return false; }
    theme_sd::lock();
    s_file = SD.open(path, "r");
    theme_sd::unlock();
    if (!s_file) { Serial.printf("[tooth] %s: not found\n", path); return false; }
#else
    s_file = fopen((std::string("sim/sdcard") + path).c_str(), "rb");
    if (!s_file) { Serial.printf("[tooth] %s: not found\n", path); return false; }
#endif
    uint8_t head[12];
    bool ok = read_at(0, head, sizeof head) && memcmp(head, "TTH2", 4) == 0;
    const int ny = ok ? rd16(head + 4) : 0, np = ok ? rd16(head + 6) : 0;
    const int frames = ny * np;
    ok = ok && ny > 0 && np > 0 && frames <= MAX_FRAMES;
    if (ok) {
        s_table = (uint32_t *)heap_caps_malloc((size_t)frames * 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ok = s_table && read_at(12, (uint8_t *)s_table, (uint32_t)frames * 8);
    }
    uint32_t biggest = 0;
    for (int i = 0; ok && i < frames; ++i) {
        // The table is little-endian on disk, and so are both targets; rd32 keeps that honest.
        const uint32_t off = rd32((const uint8_t *)&s_table[i * 2]);
        const uint32_t len = rd32((const uint8_t *)&s_table[i * 2 + 1]);
        s_table[i * 2] = off; s_table[i * 2 + 1] = len;
        ok = len > 0 && len <= MAX_FRAME_BYTES;
        if (len > biggest) biggest = len;
    }
    if (ok) {
        s_comp = (uint8_t *)heap_caps_malloc(biggest, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_compCap = biggest;
        ok = s_comp != nullptr;
    }
    if (!ok) { Serial.printf("[tooth] %s: bad file\n", path); free_sheet(); return false; }
    s_sheet = true; s_sheetType = type; s_frames = frames; s_nyaw = ny; s_npitch = np;
    s_fw = rd16(head + 8); s_fh = rd16(head + 10);
    Serial.printf("[tooth] %s: %d x %d angles, %dx%d\n", path, ny, np, s_fw, s_fh);
    return true;
}

int line_cb(PNGDRAW *draw) {
    const uint8_t *src = draw->pPixels;
    const int w = draw->iWidth;
    const int bpp = (draw->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA) ? 4 : 3;
    uint16_t *row = s_pix + (size_t)draw->y * s_fw;
    for (int x = 0; x < w; ++x, src += bpp) {
        const uint16_t v = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3));
        row[s_flipNow ? (w - 1 - x) : x] = v;
    }
    return 1;
}

bool show_frame(int frame, bool flip) {
    if (!s_sheet || !s_img) return false;
    if (frame == s_shownFrame && flip == s_shownFlip) return true;
    if (!s_png) {
        void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!mem) return false;
        s_png = new (mem) PNG();
    }
    if (!s_pix) {
        s_pix = (uint16_t *)heap_caps_malloc((size_t)s_fw * s_fh * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pix) return false;
        memset(&s_dsc, 0, sizeof s_dsc);
        s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s_dsc.header.w = s_fw;
        s_dsc.header.h = s_fh;
        s_dsc.data_size = (uint32_t)s_fw * s_fh * 2;
        s_dsc.data = (const uint8_t *)s_pix;
    }
    const uint32_t off = s_table[frame * 2], len = s_table[frame * 2 + 1];
    if (len > s_compCap || !read_at(off, s_comp, len)) return false;
    if (s_png->openRAM(s_comp, (int)len, line_cb) != PNG_SUCCESS) return false;
    if (s_png->getWidth() != s_fw || s_png->getHeight() != s_fh) { s_png->close(); return false; }
    s_flipNow = flip;
    const int r = s_png->decode(nullptr, 0);
    s_png->close();
    if (r != PNG_SUCCESS) return false;
    s_shownFrame = frame; s_shownFlip = flip;
    lv_img_cache_invalidate_src(&s_dsc);
    lv_img_set_src(s_img, &s_dsc);
    lv_obj_invalidate(s_img);
    return true;
}

// The frame for the current tooth and angle. Flipping a picture left-to-right also reverses
// which way it appears to spin about its long axis (end-over-end is unaffected), so a
// mirrored tooth counts its yaw frames backwards to turn the same way under the finger.
void draw_tooth() {
    if (!s_screen) return;
    const int type = tooth_data::typeOf(s_num);
    if (type != s_sheetType || !s_sheet || s_frames == 0) return;
    const bool flip = mirrored(s_num);
    const int y = ((s_yaw % s_nyaw) + s_nyaw) % s_nyaw;
    const int p = ((s_pitch % s_npitch) + s_npitch) % s_npitch;
    show_frame(p * s_nyaw + (flip ? (s_nyaw - y) % s_nyaw : y), flip);
}

void set_image_visible(bool on) {
    if (!s_img) return;
    if (on) lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
}

void render_text() {
    if (!s_screen) return;
    const tooth_data::ToothType &t = tooth_data::kTypes[tooth_data::typeOf(s_num)];
    char buf[64];
    snprintf(buf, sizeof buf, "#%d", s_num);
    lv_label_set_text(s_number, buf);
    lv_label_set_text(s_name, t.name);
    snprintf(buf, sizeof buf, "%s  -  erupts %s", tooth_data::quadrantOf(s_num), t.erupts);
    lv_label_set_text(s_sub, buf);
    lv_label_set_text(s_cardText, t.fact);
    if (s_details) lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    for (int n = 1; n <= TEETH; ++n) style_dot(n, n == s_num);
}

// A different tooth TYPE needs its frames read from the card. That waits until the knob
// rests, so spinning through several teeth does not read every one of them on the way.
void request_tooth() {
    const int type = tooth_data::typeOf(s_num);
    if (type == s_sheetType && s_sheet) {
        set_image_visible(true);
        draw_tooth();
        return;
    }
    s_wantType = type;
    s_lastTurn = lv_tick_get();
    set_image_visible(false);
    lv_obj_add_flag(s_noimg, LV_OBJ_FLAG_HIDDEN);
}

void tick(lv_timer_t *) {
    if (!s_screen) return;
    const uint32_t now = lv_tick_get();
    if (s_wantType >= 0 && now - s_lastTurn >= LOAD_SETTLE) {
        const int type = s_wantType;
        s_wantType = -1;
        free_frame();
        s_missing = !load_sheet(type);
        if (s_missing) {
            lv_obj_clear_flag(s_noimg, LV_OBJ_FLAG_HIDDEN);
        } else {
            set_image_visible(true);
            draw_tooth();
        }
    }
    if (!s_dragging && s_sheet && s_wantType < 0 &&
        now - s_lastDrag >= SPIN_RESUME && now - s_lastSpin >= SPIN_MS) {
        s_lastSpin = now;
        ++s_yaw;
        draw_tooth();
    }
}

// Drag to turn: a finger on the Orb, the mouse in the simulator. Sideways travel spins the
// tooth about its long axis; up and down tumbles it end over end. Both wrap all the way
// round. One 15-degree step per DRAG_PX of travel.
void drag_cb(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_dragging = true;
        s_dragAcc = 0;
        s_dragAccY = 0;
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;
        lv_point_t v;
        lv_indev_get_vect(indev, &v);
        s_dragAcc += v.x;
        s_dragAccY += v.y;
        bool moved = false;
        while (s_dragAcc >= DRAG_PX)   { ++s_yaw;   s_dragAcc -= DRAG_PX;  moved = true; }
        while (s_dragAcc <= -DRAG_PX)  { --s_yaw;   s_dragAcc += DRAG_PX;  moved = true; }
        while (s_dragAccY >= DRAG_PX)  { ++s_pitch; s_dragAccY -= DRAG_PX; moved = true; }
        while (s_dragAccY <= -DRAG_PX) { --s_pitch; s_dragAccY += DRAG_PX; moved = true; }
        if (moved) draw_tooth();
        s_lastDrag = lv_tick_get();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_dragging = false;
        s_lastDrag = lv_tick_get();
    }
}

void release_art() {
    free_frame();
    free_sheet();
    free_decoder();
}

}  // namespace

lv_obj_t *toothview::build() {
    if (s_screen) return s_screen;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_screen, drag_cb, LV_EVENT_ALL, nullptr);

    // The arch: 32 dots around the edge of the dial.
    for (int n = 1; n <= TEETH; ++n) {
        lv_obj_t *d = lv_obj_create(s_screen);
        lv_obj_remove_style_all(d);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s_dots[n - 1] = d;
    }

    // The tooth picture. Not clickable, so a drag on it reaches the screen's handler.
    s_img = lv_img_create(s_screen);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_img, LV_ALIGN_TOP_MID, 0, IMG_Y);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);

    s_noimg = make_label(s_screen, &lv_font_montserrat_14, COL_HINT, 220, 260);
    lv_label_set_text(s_noimg, "3D teeth not installed\nSD card: /teeth");
    lv_obj_add_flag(s_noimg, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label(s_screen, &lv_font_montserrat_12, COL_MINT, 54, 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_label_set_text(title, "TOOTH EXPLORER");

    s_number = make_label(s_screen, &lv_font_montserrat_36, COL_ENAMEL, 72, 0);
    s_name   = make_label(s_screen, &lv_font_montserrat_26, COL_ENAMEL, 352, 340);
    s_sub    = make_label(s_screen, &lv_font_montserrat_14, COL_MINT, 386, 340);

    // The fact card: over the picture, shown by a push.
    s_card = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, 300, 150);
    lv_obj_align(s_card, LV_ALIGN_TOP_MID, 0, 165);
    lv_obj_set_style_bg_color(s_card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_card, 18, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(COL_MINT), 0);
    lv_obj_set_style_border_width(s_card, 2, 0);
    lv_obj_set_style_border_opa(s_card, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(s_card, 16, 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_cardText = lv_label_create(s_card);
    lv_obj_set_width(s_cardText, 268);
    lv_label_set_long_mode(s_cardText, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_cardText, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_cardText, lv_color_hex(COL_ENAMEL), 0);
    lv_obj_set_style_text_align(s_cardText, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_cardText, 4, 0);
    lv_obj_center(s_cardText);

    render_text();
    return s_screen;
}

void toothview::destroy() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    release_art();
    if (s_screen) { lv_obj_del(s_screen); s_screen = nullptr; }
    s_number = s_name = s_sub = s_img = s_noimg = s_card = s_cardText = nullptr;
    for (int i = 0; i < TEETH; ++i) s_dots[i] = nullptr;
}

void toothview::onTurn(int delta) {
    s_num = ((s_num - 1 + delta) % TEETH + TEETH) % TEETH + 1;   // wrap both ways
    render_text();
    request_tooth();
}

void toothview::onPress() {
    s_details = !s_details;
    render_text();
}

// Take the art when shown, give it all back when left (docs/memory.md).
void toothview::onEnter() {
    if (!s_screen) return;
    render_text();
    if (!s_timer) s_timer = lv_timer_create(tick, 30, nullptr);
    s_lastDrag = 0;
    request_tooth();
    s_lastTurn = lv_tick_get() - LOAD_SETTLE;   // nothing to wait for on arrival
}

void toothview::onExit() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    s_dragging = false;
    s_wantType = -1;
    release_art();
    set_image_visible(false);
}

int toothview::current() { return s_num; }
int toothview::frame()   { return s_shownFrame; }
