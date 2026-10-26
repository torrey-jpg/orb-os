// Kevin's Orb — Dental Fact of the Day. See fact_view.h.
#include "fact_view.h"
#include "daily_text.h"
#include "orb_time.h"
#include "theme_sd.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {

const uint32_t COL_ENAMEL = 0xF4F1E8;
// Accent: sky blue, chosen for Dr. Hollar's red/green colour blindness. Blue stays blue
// to a red/green colour-blind eye; the mint this replaced looks almost white to one
// (simulated, Machado 2009), so it could not be told apart from the white text.
const uint32_t COL_ACCENT   = 0x5BC0FF;
const uint32_t COL_GREY   = 0x9A9A9A;
const uint32_t COL_HINT   = 0x5C5C5C;

const char  *FACTS_PATH = "/facts/dental_facts.txt";
const size_t FACTS_MAX  = 128 * 1024;
const int    DAYS       = 366;

const char *DAYS_OF_WEEK[] = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY" };
const char *MONTHS[] = { "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY",
                         "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER" };

lv_obj_t   *s_screen = nullptr;
lv_obj_t   *s_date   = nullptr;
lv_obj_t   *s_fact   = nullptr;
lv_obj_t   *s_count  = nullptr;
lv_timer_t *s_timer  = nullptr;

uint8_t    *s_file   = nullptr;   // the whole list, as read
size_t      s_len    = 0;
const char *s_line[DAYS] = {};    // start of each fact inside s_file (newlines zeroed)
int         s_lines  = 0;
int         s_offset = 0;         // days from today
int         s_shown  = -1;

// Line index for a date: the shared leap-year layout (daily_text.h).
int index_for(const struct tm &t) { return daily_text::indexFor(t); }

void load_file() {
    if (s_file) return;
    s_file = theme_sd::read_whole(FACTS_PATH, s_len, FACTS_MAX);
    s_lines = 0;
    if (!s_file) return;
    char *p = (char *)s_file, *end = p + s_len;
    while (p < end && s_lines < DAYS) {
        char *nl = (char *)memchr(p, '\n', (size_t)(end - p));
        char *stop = nl ? nl : end;
        if (stop > p && stop[-1] == '\r') stop[-1] = '\0';
        if (nl) *nl = '\0';
        // The last line may have no newline: it ends at the end of the buffer, which is not
        // terminated, so it is only used if the buffer has room to hold a terminator.
        if (*p && *p != '#' && (nl || stop < end)) s_line[s_lines++] = p;
        p = nl ? nl + 1 : end;
    }
}

void free_file() {
    if (s_file) { theme_sd::free(s_file); s_file = nullptr; }
    s_len = 0; s_lines = 0; s_shown = -1;
}

const lv_font_t *font_for(size_t len) {
    if (len <= 60) return &lv_font_montserrat_28;
    if (len <= 90) return &lv_font_montserrat_24;
    return &lv_font_montserrat_22;
}

void render() {
    if (!s_screen) return;
    struct tm t;
    const bool set = orb_local_time(&t);
    if (set && s_offset) {
        time_t when = time(nullptr) + (time_t)s_offset * 86400;
        localtime_r(&when, &t);
    }
    char buf[48];
    if (set) {
        snprintf(buf, sizeof buf, "%s, %s %d", DAYS_OF_WEEK[t.tm_wday % 7], MONTHS[t.tm_mon % 12], t.tm_mday);
        lv_label_set_text(s_date, buf);
    } else {
        lv_label_set_text(s_date, "SET THE CLOCK FOR TODAY'S FACT");
    }

    load_file();
    if (s_lines < DAYS) {
        lv_obj_set_style_text_font(s_fact, &lv_font_montserrat_20, 0);
        lv_label_set_text(s_fact, "Dental facts not installed.\nSD card: /facts/dental_facts.txt");
        lv_label_set_text(s_count, "");
        s_shown = -1;
        return;
    }
    const int i = set ? index_for(t) : (((s_offset % DAYS) + DAYS) % DAYS);
    s_shown = i;
    const char *f = s_line[i];
    lv_obj_set_style_text_font(s_fact, font_for(strlen(f)), 0);
    lv_label_set_text(s_fact, f);
    lv_obj_align(s_fact, LV_ALIGN_CENTER, 0, 12);
    snprintf(buf, sizeof buf, s_offset == 0 ? "TODAY'S FACT" : "FACT %d OF 366", i + 1);
    lv_label_set_text(s_count, buf);
}

lv_obj_t *make_label(const lv_font_t *font, uint32_t color, int y) {
    lv_obj_t *l = lv_label_create(s_screen);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

// The day can turn over while the screen is up.
void tick(lv_timer_t *) { render(); }

}  // namespace

lv_obj_t *factview::build() {
    if (s_screen) return s_screen;
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    // A thin accent ring, like the other dental screens.
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

    lv_obj_t *title = make_label(&lv_font_montserrat_14, COL_ACCENT, 64);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_label_set_text(title, "DENTAL FACT OF THE DAY");

    s_date = make_label(&lv_font_montserrat_14, COL_GREY, 90);
    lv_obj_set_style_text_letter_space(s_date, 2, 0);

    lv_obj_t *rule = lv_obj_create(s_screen);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 60, 2);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_style_bg_color(rule, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_70, 0);

    s_fact = lv_label_create(s_screen);
    lv_obj_set_width(s_fact, 330);
    lv_label_set_long_mode(s_fact, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_fact, lv_color_hex(COL_ENAMEL), 0);
    lv_obj_set_style_text_align(s_fact, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_fact, 6, 0);

    s_count = make_label(&lv_font_montserrat_12, COL_ACCENT, 362);
    lv_obj_set_style_text_letter_space(s_count, 2, 0);

    lv_obj_t *hint = make_label(&lv_font_montserrat_12, COL_HINT, 384);
    lv_label_set_text(hint, "turn: other days   push: today");

    render();
    return s_screen;
}

void factview::destroy() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    free_file();
    if (s_screen) { lv_obj_del(s_screen); s_screen = nullptr; }
    s_date = s_fact = s_count = nullptr;
}

void factview::onTurn(int delta) { s_offset += delta; render(); }
void factview::onPress()         { s_offset = 0; render(); }

void factview::onEnter() {
    s_offset = 0;
    if (!s_timer) s_timer = lv_timer_create(tick, 30000, nullptr);
    render();
}

void factview::onExit() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    free_file();
}

int factview::index() { return s_shown; }
