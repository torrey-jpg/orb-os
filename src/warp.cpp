// Kevin's Orb — the warp transition. See warp.h.
#include "warp.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef ARDUINO
#include "audio.h"
#else
#include <SDL.h>
#include <stdio.h>
#endif

namespace {

// ---- timing (ms) -------------------------------------------------------------------------
const uint32_t T_DARK   = 420;   // streaks accelerate, the rim closes in to full dark
const uint32_t T_SWAP   = 440;   // the new screen is loaded here, unseen
const uint32_t T_OPEN   = 520;   // the dark starts opening back out
const uint32_t T_END    = 950;   // fully revealed; everything is deleted
const uint32_t FRAME_MS = 16;

// ---- look --------------------------------------------------------------------------------
const int      CX = 233, CY = 233;
const int      RIM = 233;                // the dial's radius
// Border width when fully dark. One pixel short of the ring's radius (it is 2*RIM+4 across):
// LVGL draws nothing at all for a border wider than the radius, so "wider to be safe" would
// make the dark vanish at exactly the moment it should be total.
const int      DARK_MAX = RIM + 1;
const int      STREAKS = 44;
const uint32_t COL_WHITE = 0xF0F6FF;
// Accent: sky blue, chosen for Dr. Hollar's red/green colour blindness. Blue stays blue
// to a red/green colour-blind eye; the mint this replaced looks almost white to one
// (simulated, Machado 2009), so it could not be told apart from the white text.
const uint32_t COL_ACCENT  = 0x5BC0FF;

const char *SOUND_PATH = "/sounds/warp.pcm";

struct Streak {
    lv_obj_t  *line;
    lv_point_t pts[2];
    float      angle, speed, phase;
};

bool        s_enabled = true;
bool        s_active  = false;
lv_obj_t   *s_root    = nullptr;   // full-screen container on the top layer
lv_obj_t   *s_iris    = nullptr;   // a ring whose border is the closing / opening dark
lv_obj_t   *s_flash   = nullptr;
Streak      s_streaks[STREAKS];
lv_timer_t *s_timer   = nullptr;
uint32_t    s_t0      = 0;
uint32_t    s_last    = 0;
float       s_travel  = 0;
bool        s_swapped = false;
void      (*s_swap)(int) = nullptr;
int         s_arg     = 0;

float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
float ease_in(float t)  { return t * t * t; }
float ease_out(float t) { t = 1 - t; return 1 - t * t * t; }

// How fast the streaks fly, 0..1, over the whole warp.
float warp_speed(uint32_t t) {
    if (t < T_DARK) return 0.15f + 0.85f * ease_in((float)t / T_DARK);
    if (t < T_OPEN) return 1.0f;
    return 1.0f - 0.7f * ease_out(clamp01((float)(t - T_OPEN) / (T_END - T_OPEN)));
}

// How much of the dial is dark, as a border width from the rim inwards.
int iris_width(uint32_t t) {
    if (t < T_DARK) return (int)(DARK_MAX * ease_in((float)t / T_DARK) + 0.5f);
    if (t < T_OPEN) return DARK_MAX;
    return (int)(DARK_MAX * (1 - ease_out(clamp01((float)(t - T_OPEN) / (T_END - T_OPEN)))) + 0.5f);
}

// How visible the streaks are.
lv_opa_t streak_opa(uint32_t t) {
    if (t < 80) return (lv_opa_t)(255 * t / 80);
    if (t < T_OPEN) return 255;
    return (lv_opa_t)(255 * (1 - clamp01((float)(t - T_OPEN) / (T_END - T_OPEN - 80))));
}

void play_sound() {
#ifdef ARDUINO
    audio_play_file(SOUND_PATH);
#else
    // The simulator has no audio.cpp, so it plays the same file through SDL.
    static SDL_AudioDeviceID dev = 0;
    if (!dev) {
        if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
        SDL_AudioSpec want = {};
        want.freq = 16000; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 1024;
        dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        if (!dev) return;
        SDL_PauseAudioDevice(dev, 0);
    }
    char path[96];
    snprintf(path, sizeof path, "sim/sdcard%s", SOUND_PATH);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static uint8_t buf[64 * 1024];
    SDL_ClearQueuedAudio(dev);
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) SDL_QueueAudio(dev, buf, (Uint32)n);
    fclose(f);
#endif
}

void place_streak(Streak &s, float speed, lv_opa_t opa) {
    // Each streak's head runs from the centre to past the rim and starts again, faster
    // further out (a perspective tunnel). Longer as the warp speeds up.
    float u = s.phase + s_travel * s.speed;
    u -= floorf(u);
    const float head = 12.0f + 270.0f * u * u;
    const float len  = (8.0f + 150.0f * speed) * (0.25f + u);
    float tail = head - len;
    if (tail < 6.0f) tail = 6.0f;
    const float c = cosf(s.angle), sn = sinf(s.angle);
    s.pts[0].x = (lv_coord_t)(CX + c * tail);
    s.pts[0].y = (lv_coord_t)(CY + sn * tail);
    s.pts[1].x = (lv_coord_t)(CX + c * head);
    s.pts[1].y = (lv_coord_t)(CY + sn * head);
    lv_line_set_points(s.line, s.pts, 2);
    // Fade in near the centre so streaks appear out of the distance rather than popping.
    lv_obj_set_style_line_opa(s.line, (lv_opa_t)(opa * clamp01(u * 4.0f)), 0);
}

void finish() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    if (s_root)  { lv_obj_del(s_root); s_root = nullptr; }
    s_iris = s_flash = nullptr;
    for (auto &s : s_streaks) s.line = nullptr;
    s_active = false;
}

void do_swap() {
    if (s_swapped) return;
    s_swapped = true;
    if (s_swap) s_swap(s_arg);
    // Keep the warp above the screen that was just loaded.
    if (s_root) lv_obj_move_foreground(s_root);
}

void tick(lv_timer_t *) {
    const uint32_t now = lv_tick_get();
    const uint32_t t = now - s_t0;
    const float dt = (float)(now - s_last) / 1000.0f;
    s_last = now;

    const float speed = warp_speed(t);
    s_travel += dt * (0.4f + 2.6f * speed);

    lv_obj_set_style_border_width(s_iris, iris_width(t), 0);

    const lv_opa_t opa = streak_opa(t);
    for (auto &s : s_streaks) place_streak(s, speed, opa);

    // The arrival flash: an accent-coloured disc that blooms and fades as the dark opens.
    if (t >= T_SWAP && t < T_END) {
        const float f = clamp01((float)(t - T_SWAP) / 320.0f);
        const int d = (int)(20 + 300 * ease_out(f));
        lv_obj_set_size(s_flash, d, d);
        lv_obj_align(s_flash, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_opa(s_flash, (lv_opa_t)(200 * (1 - f)), 0);
        lv_obj_clear_flag(s_flash, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_flash, LV_OBJ_FLAG_HIDDEN);
    }

    if (t >= T_SWAP) do_swap();
    if (t >= T_END) finish();
}

}  // namespace

void warp::start(void (*swap)(int arg), int arg) {
    if (!s_enabled || s_active) {
        if (s_active) { do_swap(); }
        if (swap) swap(arg);
        return;
    }
    s_active  = true;
    s_swapped = false;
    s_swap    = swap;
    s_arg     = arg;
    s_t0 = s_last = lv_tick_get();
    s_travel = 0;

    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, 2 * RIM, 2 * RIM);
    lv_obj_align(s_root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // The dark: a transparent circle whose border thickens from the rim to the centre.
    s_iris = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_iris);
    lv_obj_set_size(s_iris, 2 * RIM + 4, 2 * RIM + 4);
    lv_obj_align(s_iris, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_iris, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_iris, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_iris, lv_color_black(), 0);
    lv_obj_set_style_border_opa(s_iris, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_iris, 0, 0);
    lv_obj_clear_flag(s_iris, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < STREAKS; ++i) {
        Streak &s = s_streaks[i];
        s.angle = (6.2831853f * i) / STREAKS + ((rand() % 100) - 50) * 0.0012f;
        s.speed = 0.6f + (rand() % 100) * 0.008f;
        s.phase = (rand() % 1000) / 1000.0f;
        s.line  = lv_line_create(s_root);
        const bool accent = (i % 3) == 0;
        lv_obj_set_style_line_color(s.line, lv_color_hex(accent ? COL_ACCENT : COL_WHITE), 0);
        lv_obj_set_style_line_width(s.line, accent ? 3 : 2, 0);
        lv_obj_set_style_line_rounded(s.line, true, 0);
        lv_obj_set_style_line_opa(s.line, LV_OPA_TRANSP, 0);
        s.pts[0] = s.pts[1] = { (lv_coord_t)CX, (lv_coord_t)CY };
        lv_line_set_points(s.line, s.pts, 2);
        lv_obj_clear_flag(s.line, LV_OBJ_FLAG_CLICKABLE);
    }

    s_flash = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_flash);
    lv_obj_set_style_radius(s_flash, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_flash, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_flash, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_flash, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_flash, LV_OBJ_FLAG_CLICKABLE);

    play_sound();
    s_timer = lv_timer_create(tick, FRAME_MS, nullptr);
    tick(s_timer);
}

bool warp::active() { return s_active; }

void warp::setEnabled(bool on) { s_enabled = on; }
