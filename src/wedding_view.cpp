// Kevin's Orb — Wedding countdown. See wedding_view.h.
#include "wedding_view.h"
#include "config.h"
#include "orb_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(font_kevin_temp_84);   // digits 0-9, '-' and the degree sign

namespace {

const uint32_t COL_WHITE  = 0xF4F1E8;
const uint32_t COL_ACCENT = 0x5BC0FF;   // sky blue: the colour-blind-safe accent on every screen
const uint32_t COL_GOLD   = 0xE6C27A;   // wedding gold, decoration only
const uint32_t COL_GREY   = 0x9A9A9A;
const uint32_t COL_HINT   = 0x5C5C5C;

const int Y = KEVIN_WEDDING_Y, M = KEVIN_WEDDING_M, D = KEVIN_WEDDING_D;
const int MODES = 4;   // what a turn cycles through on the bottom line

lv_obj_t   *s_screen = nullptr;
lv_obj_t   *s_names = nullptr, *s_date = nullptr, *s_big = nullptr, *s_unit = nullptr;
lv_obj_t   *s_hms = nullptr, *s_hmsLabel = nullptr, *s_extra = nullptr, *s_hint = nullptr;
lv_obj_t   *s_progress = nullptr;
lv_timer_t *s_timer = nullptr;
int         s_mode = 0;
int         s_days = 0;

// Days since 1970-01-01 for a calendar date (Howard Hinnant's days_from_civil).
long day_number(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

// Whole months from (y,m,d) to the wedding, and the days left over after them.
void months_days(int y, int m, int d, int &months, int &rest) {
    months = (Y - y) * 12 + (M - m);
    if (D < d) --months;
    if (months < 0) months = 0;
    int ym = y * 12 + (m - 1) + months;
    int yy = ym / 12, mm = ym % 12 + 1;
    // The same day number in that month, or its last day if it is shorter.
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int dim = mdays[mm - 1] + (mm == 2 && ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0));
    int dd = d > dim ? dim : d;
    rest = (int)(day_number(Y, M, D) - day_number(yy, mm, dd));
}

void group(char *out, size_t cap, long v) {   // 5160 -> "5,160"
    char raw[24];
    snprintf(raw, sizeof raw, "%ld", v);
    const int n = (int)strlen(raw);
    size_t o = 0;
    for (int i = 0; i < n && o + 1 < cap; ++i) {
        out[o++] = raw[i];
        if ((n - 1 - i) % 3 == 0 && i != n - 1 && o + 1 < cap) out[o++] = ',';
    }
    out[o] = 0;
}

void set_visible(lv_obj_t *o, bool on) {
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void render() {
    if (!s_screen) return;
    struct tm t;
    bool clockSet = orb_local_time(&t);
#if !defined(ARDUINO)
    // Simulator: SIM_TODAY=2027-05-29 pretends it is that day, to see the day and after.
    if (const char *f = getenv("SIM_TODAY")) {
        int y, m, d;
        if (sscanf(f, "%d-%d-%d", &y, &m, &d) == 3) { t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; clockSet = true; }
    }
#endif
    if (!clockSet) {
        lv_label_set_text(s_big, "");
        lv_label_set_text(s_unit, "WAITING FOR THE CLOCK");
        lv_label_set_text(s_hms, ""); lv_label_set_text(s_hmsLabel, "");
        lv_label_set_text(s_extra, ""); lv_label_set_text(s_hint, "");
        lv_arc_set_value(s_progress, 0);
        return;
    }
    const long today = day_number(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    const long wday  = day_number(Y, M, D);
    const long days  = wday - today;              // calendar days to go
    if (days != 0) lv_obj_align(s_unit, LV_ALIGN_TOP_MID, 0, 232);
    s_days = (int)days;
    char buf[64], n[24];

    if (days > 0) {
        snprintf(buf, sizeof buf, "%ld", days);
        lv_obj_set_style_text_font(s_big, &font_kevin_temp_84, 0);
        lv_label_set_text(s_big, buf);
        lv_label_set_text(s_unit, days == 1 ? "DAY TO GO" : "DAYS TO GO");
        // Hours, minutes and seconds to the start of the day after this one; the whole
        // days above already count the rest.
        const int secs = 86400 - (t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec);
        snprintf(buf, sizeof buf, "%02d : %02d : %02d", secs / 3600 % 24, secs / 60 % 60, secs % 60);
        lv_label_set_text(s_hms, buf);
        lv_label_set_text(s_hmsLabel, "HRS        MIN        SEC");
        set_visible(s_hms, true); set_visible(s_hmsLabel, true);
        // The bottom line, which a turn changes.
        const long hours = (days - 1) * 24 + secs / 3600;
        switch (s_mode) {
            case 0: {
                int mo, rest;
                months_days(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, mo, rest);
                if (mo > 0) snprintf(buf, sizeof buf, "%d month%s, %d day%s", mo, mo == 1 ? "" : "s", rest, rest == 1 ? "" : "s");
                else        snprintf(buf, sizeof buf, "%ld day%s", days, days == 1 ? "" : "s");
                break;
            }
            case 1: snprintf(buf, sizeof buf, "%ld week%s, %ld day%s", days / 7, days / 7 == 1 ? "" : "s", days % 7, days % 7 == 1 ? "" : "s"); break;
            case 2: group(n, sizeof n, hours); snprintf(buf, sizeof buf, "%s hours", n); break;
            default: snprintf(buf, sizeof buf, "%ld sleep%s", days, days == 1 ? "" : "s"); break;
        }
        lv_label_set_text(s_extra, buf);
        lv_label_set_text(s_hint, "turn for more");
        // The gold ring fills over the final year.
        const int filled = days >= 365 ? 0 : (int)((365 - days) * 1000 / 365);
        lv_arc_set_value(s_progress, filled);
    } else if (days == 0) {
        lv_obj_set_style_text_font(s_big, &lv_font_montserrat_40, 0);
        lv_label_set_text(s_big, "TODAY'S\nTHE DAY");
        lv_label_set_text(s_unit, "CONGRATULATIONS!");
        lv_obj_align(s_unit, LV_ALIGN_TOP_MID, 0, 262);   // below the two-line headline
        set_visible(s_hms, false); set_visible(s_hmsLabel, false);
        lv_label_set_text(s_extra, "");
        lv_label_set_text(s_hint, "");
        lv_arc_set_value(s_progress, 1000);
    } else {
        // Married: how long, and the next anniversary.
        const long married = -days;
        snprintf(buf, sizeof buf, "%ld", married);
        lv_obj_set_style_text_font(s_big, &font_kevin_temp_84, 0);
        lv_label_set_text(s_big, buf);
        lv_label_set_text(s_unit, married == 1 ? "DAY MARRIED" : "DAYS MARRIED");
        set_visible(s_hms, false); set_visible(s_hmsLabel, false);
        int ay = t.tm_year + 1900;
        if (day_number(ay, M, D) <= today) ++ay;
        const long toAnn = day_number(ay, M, D) - today;
        const int years = ay - Y;
        snprintf(buf, sizeof buf, "%d%s anniversary in %ld day%s", years,
                 years % 100 >= 11 && years % 100 <= 13 ? "th" : years % 10 == 1 ? "st" : years % 10 == 2 ? "nd" : years % 10 == 3 ? "rd" : "th",
                 toAnn, toAnn == 1 ? "" : "s");
        lv_label_set_text(s_extra, buf);
        lv_label_set_text(s_hint, "");
        lv_arc_set_value(s_progress, (int)((365 - toAnn) * 1000 / 365));
    }
}

void tick(lv_timer_t *) { render(); }

lv_obj_t *make_label(const lv_font_t *font, uint32_t color, int y, int spacing = 0) {
    lv_obj_t *l = lv_label_create(s_screen);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (spacing) lv_obj_set_style_text_letter_space(l, spacing, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(l, "");
    return l;
}

// One wedding ring: a gold circle outline with a small highlight.
void ring(int dx, int y) {
    lv_obj_t *r = lv_obj_create(s_screen);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 30, 30);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(r, 3, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_border_opa(r, LV_OPA_COVER, 0);
    lv_obj_align(r, LV_ALIGN_TOP_MID, dx, y);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
}

}  // namespace

lv_obj_t *weddingview::build() {
    if (s_screen) return s_screen;
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    // The progress ring: a faint full track, gold filling clockwise from the top.
    s_progress = lv_arc_create(s_screen);
    lv_obj_set_size(s_progress, 436, 436);
    lv_obj_center(s_progress);
    lv_arc_set_rotation(s_progress, 270);
    lv_arc_set_bg_angles(s_progress, 0, 360);
    lv_arc_set_range(s_progress, 0, 1000);
    lv_arc_set_value(s_progress, 0);
    lv_obj_remove_style(s_progress, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_progress, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_progress, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_progress, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_progress, lv_color_hex(COL_GOLD), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_progress, true, LV_PART_INDICATOR);

    ring(-10, 46);
    ring(10, 46);

    s_names = make_label(&lv_font_montserrat_20, COL_WHITE, 88, 3);
    lv_label_set_text(s_names, KEVIN_WEDDING_NAMES);
    s_date = make_label(&lv_font_montserrat_14, COL_GREY, 116, 2);
    {
        static const char *DOW[] = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY" };
        static const char *MON[] = { "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY",
                                     "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER" };
        const long dn = day_number(Y, M, D);
        const int dow = (int)(((dn % 7) + 7 + 4) % 7);   // 1970-01-01 was a Thursday
        char buf[48];
        snprintf(buf, sizeof buf, "%s, %s %d, %d", DOW[dow], MON[(M - 1) % 12], D, Y);
        lv_label_set_text(s_date, buf);
    }

    s_big  = make_label(&font_kevin_temp_84, COL_WHITE, 146);
    s_unit = make_label(&lv_font_montserrat_20, COL_ACCENT, 232, 4);
    s_hms  = make_label(&lv_font_montserrat_28, COL_WHITE, 272);
    s_hmsLabel = make_label(&lv_font_montserrat_12, COL_GREY, 306, 1);
    s_extra = make_label(&lv_font_montserrat_18, COL_WHITE, 340);
    s_hint  = make_label(&lv_font_montserrat_12, COL_HINT, 372);

    render();
    return s_screen;
}

void weddingview::destroy() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    if (s_screen) { lv_obj_del(s_screen); s_screen = nullptr; }
    s_names = s_date = s_big = s_unit = s_hms = s_hmsLabel = s_extra = s_hint = s_progress = nullptr;
}

void weddingview::onTurn(int delta) { s_mode = ((s_mode + delta) % MODES + MODES) % MODES; render(); }
void weddingview::onPress() { s_mode = 0; render(); }

void weddingview::onEnter() {
    s_mode = 0;
    if (!s_timer) s_timer = lv_timer_create(tick, 1000, nullptr);   // the seconds tick
    render();
}

void weddingview::onExit() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
}

int weddingview::daysToGo() { return s_days; }
