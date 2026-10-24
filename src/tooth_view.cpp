// Kevin's Orb — Tooth Explorer. See tooth_view.h.
#include "tooth_view.h"
#include "tooth_data.h"
#include "tooth_frames.h"
#ifdef ARDUINO
#include <esp_heap_caps.h>
#else
#include <cstdlib>
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
static void heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif
#include <math.h>
#include <stdio.h>
#include <string.h>

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

ToothFrames  s_tf;                     // the current tooth's pictures on the card

uint16_t    *s_pix       = nullptr;   // decoded frame shown on screen (RGB565)
lv_img_dsc_t s_dsc;
int          s_shownFrame = -1;
bool         s_shownFlip  = false;

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

void free_frame() {
    if (s_img) lv_img_set_src(s_img, nullptr);
    if (s_pix) { heap_caps_free(s_pix); s_pix = nullptr; }
    s_shownFrame = -1;
}

bool show_frame(int frame, bool flip) {
    if (!s_tf.isOpen() || !s_img) return false;
    if (frame == s_shownFrame && flip == s_shownFlip) return true;
    if (!s_pix) {
        const int w = s_tf.width(), h = s_tf.height();
        s_pix = (uint16_t *)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pix) return false;
        memset(&s_dsc, 0, sizeof s_dsc);
        s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s_dsc.header.w = w;
        s_dsc.header.h = h;
        s_dsc.data_size = (uint32_t)w * h * 2;
        s_dsc.data = (const uint8_t *)s_pix;
    }
    if (!s_tf.decode(frame, flip, s_pix)) return false;
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
    if (!s_tf.isOpen() || s_tf.type() != tooth_data::typeOf(s_num)) return;
    const bool flip = mirrored(s_num);
    show_frame(s_tf.frameFor(s_yaw, s_pitch, flip), flip);
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
    if (s_tf.isOpen() && s_tf.type() == type) {
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
        s_missing = !s_tf.open(type);
        if (s_missing) {
            lv_obj_clear_flag(s_noimg, LV_OBJ_FLAG_HIDDEN);
        } else {
            set_image_visible(true);
            draw_tooth();
        }
    }
    if (!s_dragging && s_tf.isOpen() && s_wantType < 0 &&
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
    s_tf.close();
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
