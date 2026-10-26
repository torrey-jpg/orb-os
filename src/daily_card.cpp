// Kevin's Orb — the first touch of the day. See daily_card.h.
#include "daily_card.h"
#include "app_shell.h"
#include "daily_text.h"
#include "orb_time.h"
#include "warp.h"
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ARDUINO
#include <Preferences.h>
#endif

namespace {

const uint32_t COL_ENAMEL = 0xF4F1E8;
// Accent: sky blue, chosen for Dr. Hollar's red/green colour blindness. Blue stays blue
// to a red/green colour-blind eye; the mint this replaced looks almost white to one
// (simulated, Machado 2009), so it could not be told apart from the white text.
const uint32_t COL_ACCENT   = 0x5BC0FF;
const uint32_t COL_GREY   = 0x9A9A9A;
const uint32_t COL_HINT   = 0x5C5C5C;

const char *FACTS = "/facts/dental_facts.txt";
const char *JOKES = "/facts/dental_jokes.txt";

const uint32_t STAGE_MS  = 25000;   // each card waits this long before moving on by itself
const uint32_t REVEAL_MS = 2500;    // the beat before the punchline

const char *DAYS_OF_WEEK[] = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY" };
const char *MONTHS[] = { "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY",
                         "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER" };

enum Stage { NONE, FACT, JOKE, PUNCH };
Stage       s_stage = NONE;
uint32_t    s_since = 0;
int         s_lastDay = 0;          // yyyymmdd the card last showed
bool        s_always  = false;      // simulator: SIM_DAILY=1

lv_obj_t   *s_root  = nullptr;
lv_obj_t   *s_title = nullptr;
lv_obj_t   *s_sub   = nullptr;
lv_obj_t   *s_body  = nullptr;
lv_obj_t   *s_punch = nullptr;
lv_obj_t   *s_hint  = nullptr;
lv_timer_t *s_timer = nullptr;

char s_fact[200];
char s_setup[160];
char s_punchText[120];

int ymd(const struct tm &t) { return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday; }

void save_day(int d) {
    s_lastDay = d;
#ifdef ARDUINO
    Preferences p;
    if (p.begin("kevinorb", false)) { p.putInt("dailyDay", d); p.end(); }
#else
    if (FILE *f = fopen("/tmp/orb_sim_daily", "w")) { fprintf(f, "%d\n", d); fclose(f); }
#endif
}

void load_day() {
#ifdef ARDUINO
    Preferences p;
    if (p.begin("kevinorb", true)) { s_lastDay = p.getInt("dailyDay", 0); p.end(); }
#else
    if (FILE *f = fopen("/tmp/orb_sim_daily", "r")) { if (fscanf(f, "%d", &s_lastDay) != 1) s_lastDay = 0; fclose(f); }
#endif
}

const lv_font_t *size_for(size_t len, bool big) {
    if (len <= 50) return big ? &lv_font_montserrat_28 : &lv_font_montserrat_26;
    if (len <= 90) return &lv_font_montserrat_24;
    return &lv_font_montserrat_22;
}

lv_obj_t *label(const lv_font_t *font, uint32_t color, int y, int width) {
    lv_obj_t *l = lv_label_create(s_root);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (width) { lv_obj_set_width(l, width); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP); }
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

void close_card() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    if (s_root)  { lv_obj_del(s_root); s_root = nullptr; }
    s_title = s_sub = s_body = s_punch = s_hint = nullptr;
    s_stage = NONE;
}

void show_fact() {
    s_stage = FACT;
    s_since = lv_tick_get();
    lv_label_set_text(s_title, "DENTAL FACT OF THE DAY");
    lv_obj_set_style_text_font(s_body, size_for(strlen(s_fact), true), 0);
    lv_label_set_text(s_body, s_fact);
    lv_obj_align(s_body, LV_ALIGN_CENTER, 0, 8);
    lv_obj_add_flag(s_punch, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_hint, s_setup[0] ? "push for today's joke" : "push to close");
}

void show_joke() {
    s_stage = JOKE;
    s_since = lv_tick_get();
    lv_label_set_text(s_title, "DENTAL JOKE OF THE DAY");
    lv_obj_set_style_text_font(s_body, size_for(strlen(s_setup), false), 0);
    lv_label_set_text(s_body, s_setup);
    lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_set_style_text_font(s_punch, size_for(strlen(s_punchText), true), 0);
    lv_label_set_text(s_punch, s_punchText);
    lv_obj_add_flag(s_punch, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_hint, "");
}

void reveal() {
    s_stage = PUNCH;
    s_since = lv_tick_get();
    lv_obj_update_layout(s_body);
    lv_obj_align_to(s_punch, s_body, LV_ALIGN_OUT_BOTTOM_MID, 0, 22);
    lv_obj_clear_flag(s_punch, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_hint, "push to close");
}

void advance() {
    if (s_stage == FACT)       { if (s_setup[0]) show_joke(); else close_card(); }
    else if (s_stage == JOKE)  reveal();
    else                       close_card();
}

void tick(lv_timer_t *) {
    const uint32_t t = lv_tick_get() - s_since;
    if (s_stage == JOKE && t >= REVEAL_MS) reveal();
    else if ((s_stage == FACT || s_stage == PUNCH) && t >= STAGE_MS) advance();
}

void touch_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) advance();
}

bool open_card(const struct tm &t) {
    const int i = daily_text::indexFor(t);
    if (!daily_text::lineFor(FACTS, i, s_fact, sizeof s_fact)) return false;
    char joke[300];
    s_setup[0] = s_punchText[0] = '\0';
    if (daily_text::lineFor(JOKES, i, joke, sizeof joke)) {
        if (char *bar = strchr(joke, '|')) {
            *bar = '\0';
            snprintf(s_setup, sizeof s_setup, "%.159s", joke);
            snprintf(s_punchText, sizeof s_punchText, "%.119s", bar + 1);
        }
    }

    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, 466, 466);
    lv_obj_center(s_root);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_root, touch_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *ring = lv_arc_create(s_root);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 430, 430);
    lv_obj_center(ring);
    lv_arc_set_bg_angles(ring, 0, 0);
    lv_arc_set_angles(ring, 0, 360);
    lv_obj_set_style_arc_color(ring, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ring, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(ring, LV_OPA_50, LV_PART_INDICATOR);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    s_title = label(&lv_font_montserrat_14, COL_ACCENT, 64, 0);
    lv_obj_set_style_text_letter_space(s_title, 3, 0);
    s_sub = label(&lv_font_montserrat_14, COL_GREY, 90, 0);
    lv_obj_set_style_text_letter_space(s_sub, 2, 0);
    char date[48];
    snprintf(date, sizeof date, "%s, %s %d", DAYS_OF_WEEK[t.tm_wday % 7], MONTHS[t.tm_mon % 12], t.tm_mday);
    lv_label_set_text(s_sub, date);
    lv_obj_t *rule = lv_obj_create(s_root);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 60, 2);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_style_bg_color(rule, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_70, 0);

    s_body = label(&lv_font_montserrat_24, COL_ENAMEL, 150, 330);
    lv_obj_set_style_text_line_space(s_body, 6, 0);
    s_punch = label(&lv_font_montserrat_26, COL_ACCENT, 260, 320);
    lv_obj_set_style_text_line_space(s_punch, 6, 0);
    s_hint = label(&lv_font_montserrat_12, COL_HINT, 384, 0);

    show_fact();
    s_timer = lv_timer_create(tick, 100, nullptr);
    return true;
}

}  // namespace

void daily_card::begin() {
#ifndef ARDUINO
    if (const char *e = getenv("SIM_DAILY")) s_always = atoi(e) != 0;
#endif
    load_day();
}

bool daily_card::showing() { return s_stage != NONE; }

bool daily_card::consume() {
    if (s_stage == NONE) return false;
    advance();
    return true;
}

bool daily_card::maybeShow() {
    if (s_stage != NONE || warp::active() || app_shell::browsing()) return false;
    struct tm t;
    if (!orb_local_time(&t)) return false;          // no date yet: nothing to choose from
    const int today = ymd(t);
    if (today == s_lastDay && !s_always) return false;
    if (!open_card(t)) return false;                // facts not on the card
    save_day(today);
    if (s_always) s_always = false;                 // once per run in the simulator
    printf("[daily] card for %d\n", today);
    return true;
}
