// Kevin's Orb — the hologram screensaver. See screensaver.h.
#include "screensaver.h"
#include "app_shell.h"
#include "daily_card.h"
#include "orb_time.h"
#include "tooth_data.h"
#include "tooth_frames.h"
#include "warp.h"
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#else
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
static void heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif

namespace {

// Accent: sky blue, chosen for Dr. Hollar's red/green colour blindness. Blue stays blue
// to a red/green colour-blind eye; the mint this replaced looks almost white to one
// (simulated, Machado 2009), so it could not be told apart from the white text.
const uint32_t COL_ACCENT  = 0x5BC0FF;
const uint32_t COL_TIME  = 0xC8CCD2;

const uint32_t CHECK_MS   = 500;     // how often the idle clock is looked at
const uint32_t FRAME_MS   = 40;      // animation tick
const uint32_t SPIN_MS    = 220;     // one 15-degree step of the turn (~5 s a turn)
const uint32_t SWAP_MS    = 30000;   // a different tooth this often
const uint32_t GLITCH_MS  = 160;     // the jump between teeth
const uint32_t SCAN_MS    = 2600;    // one pass of the scan band
const int      IMG_Y      = 92;      // top of the tooth picture
const int      DRIFT_PX   = 8;       // how far the whole picture wanders
const uint32_t DRIFT_MS   = 180000;  // ... and how slowly

bool        s_enabled = true;
uint32_t    s_idleMs  = screensaver::IDLE_MS;
uint32_t    s_lastKnob = 0;
lv_timer_t *s_check   = nullptr;

lv_obj_t   *s_root    = nullptr;   // full-screen, on the top layer
lv_obj_t   *s_body    = nullptr;   // everything that drifts
lv_obj_t   *s_img     = nullptr;
lv_obj_t   *s_emitter = nullptr;
lv_obj_t   *s_scan    = nullptr;
lv_obj_t   *s_label   = nullptr;
lv_obj_t   *s_time    = nullptr;
lv_timer_t *s_anim    = nullptr;

ToothFrames  s_tf;
uint16_t    *s_pix    = nullptr;
lv_img_dsc_t s_dsc;
int          s_num    = 1;
int          s_yaw    = 0;
uint32_t     s_t0 = 0, s_lastSpin = 0, s_lastSwap = 0, s_glitchUntil = 0;

uint32_t idle_ms() {
    // The screen's own clock covers touches (LVGL's pointer input); the knob is counted here
    // because on the device it does not pass through LVGL.
    const uint32_t touch = lv_disp_get_inactive_time(NULL);
    const uint32_t knob = lv_tick_get() - s_lastKnob;
    return touch < knob ? touch : knob;
}

bool show_tooth_frame() {
    if (!s_tf.isOpen() || !s_img) return false;
    const int w = s_tf.width(), h = s_tf.height();
    if (!s_pix) {
        s_pix = (uint16_t *)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pix) return false;
        memset(&s_dsc, 0, sizeof s_dsc);
        s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s_dsc.header.w = w;
        s_dsc.header.h = h;
        s_dsc.data_size = (uint32_t)w * h * 2;
        s_dsc.data = (const uint8_t *)s_pix;
    }
    const bool flip = s_num >= 9 && s_num <= 24;
    if (!s_tf.decode(s_tf.frameFor(s_yaw, 0, flip), flip, s_pix, ToothFrames::HOLOGRAM)) return false;
    lv_img_cache_invalidate_src(&s_dsc);
    lv_img_set_src(s_img, &s_dsc);
    lv_obj_invalidate(s_img);
    return true;
}

void pick_tooth(int num) {
    s_num = num;
    if (!s_tf.open(tooth_data::typeOf(num))) return;
    const tooth_data::ToothType &t = tooth_data::kTypes[tooth_data::typeOf(num)];
    char buf[48];
    snprintf(buf, sizeof buf, "#%d  %s", num, t.name);
    for (char *c = buf; *c; ++c) if (*c >= 'a' && *c <= 'z') *c -= 32;
    lv_label_set_text(s_label, buf);
    show_tooth_frame();
}

void update_time() {
    struct tm ti;
    if (!orb_local_time(&ti)) { lv_obj_add_flag(s_time, LV_OBJ_FLAG_HIDDEN); return; }
    int h = ti.tm_hour % 12;
    if (h == 0) h = 12;
    char buf[16];
    snprintf(buf, sizeof buf, "%d:%02d %s", h, ti.tm_min, ti.tm_hour < 12 ? "AM" : "PM");
    lv_label_set_text(s_time, buf);
    lv_obj_clear_flag(s_time, LV_OBJ_FLAG_HIDDEN);
}

void hide();

void anim(lv_timer_t *) {
    if (!s_root) return;
    const uint32_t now = lv_tick_get();
    const uint32_t t = now - s_t0;

    // A different tooth every SWAP_MS, through a short glitch.
    if (now - s_lastSwap >= SWAP_MS) {
        s_lastSwap = now;
        s_glitchUntil = now + GLITCH_MS;
        int next = 1 + rand() % 32;
        if (next == s_num) next = next % 32 + 1;
        pick_tooth(next);
    }
    const bool glitch = now < s_glitchUntil;
    lv_obj_set_style_translate_x(s_img, glitch ? (int)((rand() % 13) - 6) : 0, 0);

    if (now - s_lastSpin >= SPIN_MS) {
        s_lastSpin = now;
        ++s_yaw;
        show_tooth_frame();
    }

    // Flicker: mostly steady, with the odd dip.
    const int r = rand() % 100;
    lv_opa_t o = glitch ? (lv_opa_t)(120 + rand() % 100) : (r < 4 ? 170 : (r < 10 ? 225 : 255));
    lv_obj_set_style_img_opa(s_img, o, 0);

    // Scan band down through the tooth.
    const int y = IMG_Y - 20 + (int)((t % SCAN_MS) * 260 / SCAN_MS);
    lv_obj_set_y(s_scan, y);

    // Emitter breathing.
    const float b = 0.5f + 0.5f * sinf((float)t * 0.0021f);
    lv_obj_set_style_bg_opa(s_emitter, (lv_opa_t)(55 + 45 * b), 0);
    lv_obj_set_style_shadow_opa(s_emitter, (lv_opa_t)(90 + 80 * b), 0);

    // Slow drift of the whole picture, for the AMOLED's sake.
    const float d = (float)(t % DRIFT_MS) / DRIFT_MS * 6.2831853f;
    lv_obj_set_pos(s_body, (int)(DRIFT_PX * sinf(d)), (int)(DRIFT_PX * sinf(2 * d) / 2));

    if (t % 1000 < FRAME_MS) update_time();
}

void touch_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        hide();
        daily_card::maybeShow();   // a touch that wakes it can be the first touch of the day
    }
}

void show() {
    if (s_root) return;
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, 466, 466);
    lv_obj_center(s_root);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_root, touch_cb, LV_EVENT_PRESSED, nullptr);

    s_body = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, 466, 466);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_time = lv_label_create(s_body);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(COL_TIME), 0);
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 50);

    // The emitter: a flat glowing disc the hologram stands on.
    s_emitter = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_emitter);
    lv_obj_set_size(s_emitter, 170, 22);
    lv_obj_align(s_emitter, LV_ALIGN_TOP_MID, 0, IMG_Y + 214);
    lv_obj_set_style_radius(s_emitter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_emitter, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_shadow_color(s_emitter, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_shadow_width(s_emitter, 40, 0);
    lv_obj_set_style_shadow_spread(s_emitter, 4, 0);
    lv_obj_clear_flag(s_emitter, LV_OBJ_FLAG_CLICKABLE);

    s_img = lv_img_create(s_body);
    lv_obj_align(s_img, LV_ALIGN_TOP_MID, 0, IMG_Y);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);

    s_scan = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_scan);
    lv_obj_set_size(s_scan, 240, 10);
    lv_obj_align(s_scan, LV_ALIGN_TOP_MID, 0, IMG_Y);
    lv_obj_set_style_bg_color(s_scan, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_scan, LV_OPA_20, 0);
    lv_obj_set_style_radius(s_scan, 5, 0);
    lv_obj_clear_flag(s_scan, LV_OBJ_FLAG_CLICKABLE);

    s_label = lv_label_create(s_body);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_letter_space(s_label, 3, 0);
    lv_obj_align(s_label, LV_ALIGN_TOP_MID, 0, IMG_Y + 250);

    const uint32_t now = lv_tick_get();
    s_t0 = s_lastSpin = s_lastSwap = now;
    s_glitchUntil = 0;
    s_yaw = rand() % 24;
    pick_tooth(1 + rand() % 32);
    update_time();
    s_anim = lv_timer_create(anim, FRAME_MS, nullptr);
    printf("[saver] on (tooth #%d)\n", s_num);
}

void hide() {
    if (!s_root) return;
    if (s_anim) { lv_timer_del(s_anim); s_anim = nullptr; }
    lv_obj_del(s_root);
    s_root = s_body = s_img = s_emitter = s_scan = s_label = s_time = nullptr;
    if (s_pix) { heap_caps_free(s_pix); s_pix = nullptr; }
    s_tf.close();
    s_lastKnob = lv_tick_get();
    lv_disp_trig_activity(NULL);
    printf("[saver] off\n");
}

void check(lv_timer_t *) {
    if (!s_enabled || s_root) return;
    if (warp::active() || app_shell::browsing()) return;
    if (idle_ms() >= s_idleMs) show();
}

}  // namespace

void screensaver::begin() {
#ifndef ARDUINO
    if (const char *e = getenv("SIM_SAVER_MS")) s_idleMs = (uint32_t)atol(e);
#endif
    s_lastKnob = lv_tick_get();
    if (!s_check) s_check = lv_timer_create(check, CHECK_MS, nullptr);
}

bool screensaver::showing() { return s_root != nullptr; }

bool screensaver::wake() {
    s_lastKnob = lv_tick_get();
    if (!s_root) return false;
    hide();
    return true;
}

void screensaver::setEnabled(bool on) {
    s_enabled = on;
    if (!on) hide();
}
