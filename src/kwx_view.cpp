// Kevin's Orb — Weather. See kwx_view.h.
#include "kwx_view.h"
#include "kwx.h"
#include "config.h"
#include "orb_time.h"
#include <atomic>
#include <initializer_list>
#include <stdio.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(font_kevin_temp_84);
extern "C" {
extern const lv_img_dsc_t *const kwx_icons_big[];
extern const lv_img_dsc_t *const kwx_icons_small[];
}

namespace {

// Same palette as the other dental screens. Accent: sky blue, chosen for Dr. Hollar's
// red/green colour blindness. Temperatures are white, never coloured hot/cold.
const uint32_t COL_WHITE  = 0xF4F1E8;
const uint32_t COL_ACCENT = 0x5BC0FF;
const uint32_t COL_GREY   = 0x9A9A9A;
const uint32_t COL_HINT   = 0x5C5C5C;

const uint32_t EVERY_MS = 15UL * 60 * 1000;   // while the screen is up
const uint32_t RETRY_MS = 60UL * 1000;        // after a failure
const uint32_t STALE_MS = 10UL * 60 * 1000;   // entering with older weather than this refreshes

const int STRIP = 5;                          // hours shown at once
const int COL_W = 58;

std::atomic<bool> s_active{false};
std::atomic<bool> s_force{false};
uint32_t          s_lastTry = 0;              // network task only

struct Col { lv_obj_t *hour, *icon, *temp; };

lv_obj_t   *s_screen = nullptr;
lv_obj_t   *s_place = nullptr, *s_date = nullptr;
lv_obj_t   *s_icon = nullptr, *s_temp = nullptr;
lv_obj_t   *s_cond = nullptr, *s_feels = nullptr, *s_hilo = nullptr, *s_stats = nullptr;
lv_obj_t   *s_age = nullptr, *s_big = nullptr, *s_small = nullptr;
Col         s_col[STRIP] = {};
lv_timer_t *s_timer = nullptr;
int         s_off = 0;
bool        s_waiting = false;

lv_obj_t *make_label(const lv_font_t *font, uint32_t color, lv_align_t align, int x, int y) {
    lv_obj_t *l = lv_label_create(s_screen);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, align, x, y);
    lv_label_set_text(l, "");
    return l;
}

const char *compass(int deg) {
    static const char *P[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return P[((deg % 360 + 360) % 360 + 22) / 45 % 8];
}

// "18:41" -> "6:41 PM"
void ampm(char *dst, size_t cap, const char *hm) {
    int h = 0, m = 0;
    if (sscanf(hm, "%d:%d", &h, &m) != 2) { snprintf(dst, cap, "--"); return; }
    snprintf(dst, cap, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
}

void hour_label(char *dst, size_t cap, int h) {
    snprintf(dst, cap, "%d %s", h % 12 == 0 ? 12 : h % 12, h < 12 ? "AM" : "PM");
}

void show_all(bool on) {
    lv_obj_t *objs[] = { s_icon, s_temp, s_cond, s_feels, s_hilo, s_stats };
    for (lv_obj_t *o : objs) on ? lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    for (Col &c : s_col)
        for (lv_obj_t *o : { c.hour, c.icon, c.temp })
            on ? lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    on ? lv_obj_add_flag(s_big, LV_OBJ_FLAG_HIDDEN) : lv_obj_clear_flag(s_big, LV_OBJ_FLAG_HIDDEN);
    on ? lv_obj_add_flag(s_small, LV_OBJ_FLAG_HIDDEN) : lv_obj_clear_flag(s_small, LV_OBJ_FLAG_HIDDEN);
}

void render() {
    if (!s_screen) return;
    KwxSnapshot w; KwxStatus st;
    kwx_get(w, st);

    struct tm now;
    if (orb_local_time(&now)) {
        static const char *DOW[] = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY" };
        static const char *MON[] = { "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY",
                                     "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER" };
        char d[48];
        snprintf(d, sizeof d, "%s, %s %d", DOW[now.tm_wday % 7], MON[now.tm_mon % 12], now.tm_mday);
        lv_label_set_text(s_date, d);
    } else {
        lv_label_set_text(s_date, "");
    }

    char buf[96], a[24];
    // How old, and whether the last try worked.
    if (s_waiting) lv_label_set_text(s_age, "updating...");
    else if (w.valid) {
        const uint32_t sec = (kwx_now_ms() - w.fetchedMs) / 1000;
        if (sec < 90) snprintf(a, sizeof a, "just now");
        else if (sec < 3600) snprintf(a, sizeof a, "%lu min ago", (unsigned long)(sec / 60));
        else snprintf(a, sizeof a, "%lu hr ago", (unsigned long)(sec / 3600));
        snprintf(buf, sizeof buf, st == KWX_OK ? "updated %s" : "couldn't update | last %s", a);
        lv_label_set_text(s_age, buf);
    } else lv_label_set_text(s_age, "");

    if (!w.valid) {
        show_all(false);
        if (st == KWX_NO_WIFI)         { lv_label_set_text(s_big, "No Wi-Fi"); lv_label_set_text(s_small, "The Orb isn't connected to Wi-Fi."); }
        else if (st == KWX_FAILED)     { lv_label_set_text(s_big, "Can't reach the forecast"); lv_label_set_text(s_small, "Will try again in a minute."); }
        else if (st == KWX_UNREADABLE) { lv_label_set_text(s_big, "Can't read the forecast"); lv_label_set_text(s_small, "Open-Meteo sent something unexpected."); }
        else                           { lv_label_set_text(s_big, "Getting the weather..."); lv_label_set_text(s_small, "from Open-Meteo"); }
        return;
    }
    show_all(true);

    lv_img_set_src(s_icon, kwx_icons_big[kwx_icon(w.code, w.isDay)]);
    snprintf(buf, sizeof buf, "%d\xC2\xB0", w.tempF);
    lv_label_set_text(s_temp, buf);
    lv_label_set_text(s_cond, kwx_condition(w.code));
    snprintf(buf, sizeof buf, "Feels like %d\xC2\xB0", w.feelsF);   // degree sign: in Montserrat's built-in set
    lv_label_set_text(s_feels, buf);
    snprintf(buf, sizeof buf, "High %d\xC2\xB0  |  Low %d\xC2\xB0", w.hiF, w.loF);
    lv_label_set_text(s_hilo, buf);

    // In the dark the next thing worth knowing is sunrise: tomorrow's in the evening, today's
    // in the small hours. In daylight, sunset.
    char sun[16];
    const bool dark = !w.isDay;
    const bool afterSunset = dark;
    ampm(sun, sizeof sun, !dark ? w.sunset : (now.tm_hour >= 12 ? w.sunriseNext : w.sunrise));
    if (w.windMph > 0)
        snprintf(buf, sizeof buf, "Rain %d%%  |  Wind %s %d mph  |  %s %s", w.rainMax, compass(w.windDeg), w.windMph,
                 afterSunset ? "Sunrise" : "Sunset", sun);
    else
        snprintf(buf, sizeof buf, "Rain %d%%  |  Calm  |  %s %s", w.rainMax, afterSunset ? "Sunrise" : "Sunset", sun);
    lv_label_set_text(s_stats, buf);

    // The hours.
    const int maxOff = w.nHours > STRIP ? w.nHours - STRIP : 0;
    if (s_off > maxOff) s_off = maxOff;
    if (s_off < 0) s_off = 0;
    for (int i = 0; i < STRIP; ++i) {
        Col &c = s_col[i];
        const int k = s_off + i;
        if (k >= w.nHours) {
            lv_obj_add_flag(c.hour, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(c.icon, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(c.temp, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const KwxHour &h = w.hours[k];
        if (k == 0) snprintf(a, sizeof a, "NOW");
        else hour_label(a, sizeof a, h.hour);
        lv_label_set_text(c.hour, a);
        lv_obj_set_style_text_color(c.hour, lv_color_hex(k == 0 ? COL_ACCENT : COL_GREY), 0);
        lv_img_set_src(c.icon, kwx_icons_small[kwx_icon(h.code, h.day)]);
        // A chance of rain worth mentioning goes under the temperature, in words-and-number.
        // NOW shows the temperature now, not the forecast for the top of the hour.
        const int t = k == 0 ? w.tempF : h.tempF;
        if (h.rain >= 30) snprintf(buf, sizeof buf, "%d\xC2\xB0\n#5BC0FF %d%%#", t, h.rain);
        else              snprintf(buf, sizeof buf, "%d\xC2\xB0", t);
        lv_label_set_text(c.temp, buf);
    }
}

void tick(lv_timer_t *) { render(); }

}  // namespace

lv_obj_t *kwxview::build() {
    if (s_screen) return s_screen;
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ring = lv_arc_create(s_screen);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 430, 430);
    lv_obj_center(ring);
    lv_arc_set_bg_angles(ring, 0, 0);
    lv_arc_set_angles(ring, 0, 360);
    lv_obj_set_style_arc_color(ring, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ring, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(ring, LV_OPA_40, LV_PART_INDICATOR);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    s_place = make_label(&lv_font_montserrat_14, COL_ACCENT, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_text_letter_space(s_place, 3, 0);
    lv_label_set_text(s_place, KEVIN_WX_NAME);
    s_date = make_label(&lv_font_montserrat_14, COL_GREY, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_text_letter_space(s_date, 2, 0);

    // The icon left of centre, the temperature right of it.
    s_icon = lv_img_create(s_screen);
    lv_obj_align(s_icon, LV_ALIGN_TOP_MID, -66, 96);
    s_temp = make_label(&font_kevin_temp_84, COL_WHITE, LV_ALIGN_TOP_LEFT, 236, 118);

    s_cond  = make_label(&lv_font_montserrat_24, COL_WHITE, LV_ALIGN_TOP_MID, 0, 210);
    s_feels = make_label(&lv_font_montserrat_14, COL_GREY, LV_ALIGN_TOP_MID, 0, 240);
    s_hilo  = make_label(&lv_font_montserrat_20, COL_WHITE, LV_ALIGN_TOP_MID, 0, 262);
    s_stats = make_label(&lv_font_montserrat_14, COL_GREY, LV_ALIGN_TOP_MID, 0, 292);

    for (int i = 0; i < STRIP; ++i) {
        const int x = (i - STRIP / 2) * COL_W;
        Col &c = s_col[i];
        c.hour = make_label(&lv_font_montserrat_12, COL_GREY, LV_ALIGN_TOP_MID, x, 320);
        c.icon = lv_img_create(s_screen);
        lv_obj_align(c.icon, LV_ALIGN_TOP_MID, x, 336);
        c.temp = make_label(&lv_font_montserrat_16, COL_WHITE, LV_ALIGN_TOP_MID, x, 377);
        lv_label_set_recolor(c.temp, true);
        lv_obj_set_style_text_line_space(c.temp, -2, 0);
    }
    s_age = make_label(&lv_font_montserrat_12, COL_HINT, LV_ALIGN_TOP_MID, 0, 418);

    s_big = make_label(&lv_font_montserrat_24, COL_WHITE, LV_ALIGN_TOP_MID, 0, 186);
    s_small = make_label(&lv_font_montserrat_16, COL_GREY, LV_ALIGN_TOP_MID, 0, 222);

    render();
    return s_screen;
}

void kwxview::destroy() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    if (s_screen) { lv_obj_del(s_screen); s_screen = nullptr; }
    s_place = s_date = s_icon = s_temp = s_cond = s_feels = s_hilo = s_stats = s_age = s_big = s_small = nullptr;
    for (Col &c : s_col) c = Col{};
}

void kwxview::onTurn(int delta) { s_off += delta; render(); }
void kwxview::onPress() { s_waiting = true; s_force = true; render(); }

void kwxview::onEnter() {
    s_off = 0;
    KwxSnapshot w; KwxStatus st;
    kwx_get(w, st);
    if (!w.valid || kwx_now_ms() - w.fetchedMs > STALE_MS) s_force = true;
    s_active = true;
    if (!s_timer) s_timer = lv_timer_create(tick, 30000, nullptr);
    render();
}

void kwxview::onExit() {
    s_active = false;
    s_waiting = false;
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
}

bool kwxview::fetchStep() {
    if (!s_active) return false;
    const uint32_t now = kwx_now_ms();
    KwxSnapshot w; KwxStatus st;
    kwx_get(w, st);
    const uint32_t due = st == KWX_OK ? EVERY_MS : RETRY_MS;
    if (!s_force && s_lastTry && now - s_lastTry < due) return false;
    s_force = false;
    s_lastTry = now ? now : 1;
    kwx_fetch_now();
    return true;
}

void kwxview::onWeatherReady() { s_waiting = false; render(); }

int kwxview::hourOffset() { return s_off; }
