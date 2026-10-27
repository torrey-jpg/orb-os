// Kevin's Orb — the first touch of the day. See daily_card.h.
#include "daily_card.h"
#include "app_shell.h"
#include "daily_text.h"
#include "config.h"
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
const char *FAMILY_FILE = "/family/messages.txt";   // Torrey's notes; see KEVIN_FAMILY_* in config.h

const uint32_t STAGE_MS  = 25000;   // each card waits this long before moving on by itself
const uint32_t REVEAL_MS = 2500;    // the beat before the punchline

const char *DAYS_OF_WEEK[] = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY" };
const char *MONTHS[] = { "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY",
                         "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER" };

enum Stage { NONE, FACT, JOKE, PUNCH, FAMILY };
Stage       s_stage = NONE;
uint32_t    s_since = 0;
int         s_lastDay = 0;          // yyyymmdd the card last showed
bool        s_always  = false;      // simulator: SIM_DAILY=1
long        s_famDay  = 0;          // day number the last note from Mom & Dad showed (0 = never)
int         s_famIdx  = 0;          // which note comes next
bool        s_famForce = false;     // simulator: SIM_FAMILY=1 shows one today regardless

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
char s_famText[300];                // today's note, empty when it is not a note day

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

// Days since 1970-01-01 for the local date (Howard Hinnant's days_from_civil).
long day_number(const struct tm &t) {
    int y = t.tm_year + 1900, m = t.tm_mon + 1, d = t.tm_mday;
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

void save_family(long day, int nextIdx) {
    s_famDay = day; s_famIdx = nextIdx;
#ifdef ARDUINO
    Preferences p;
    if (p.begin("kevinorb", false)) { p.putLong("famDay", (int32_t)day); p.putInt("famIdx", nextIdx); p.end(); }
#else
    if (FILE *f = fopen("/tmp/orb_sim_family", "w")) { fprintf(f, "%ld %d\n", day, nextIdx); fclose(f); }
#endif
}

void load_family() {
#ifdef ARDUINO
    Preferences p;
    if (p.begin("kevinorb", true)) { s_famDay = p.getLong("famDay", 0); s_famIdx = p.getInt("famIdx", 0); p.end(); }
#else
    if (FILE *f = fopen("/tmp/orb_sim_family", "r")) { if (fscanf(f, "%ld %d", &s_famDay, &s_famIdx) != 2) { s_famDay = 0; s_famIdx = 0; } fclose(f); }
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
    lv_label_set_text(s_hint, s_setup[0] ? "push for today's joke" : s_famText[0] ? "push for a note from Mom & Dad" : "push to close");
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
    lv_label_set_text(s_hint, s_famText[0] ? "push for a note from Mom & Dad" : "push to close");
}

// Kevin's Orb: the note from Mom & Dad, after the joke on a note day.
void show_family() {
    s_stage = FAMILY;
    s_since = lv_tick_get();
    lv_label_set_text(s_title, "FROM MOM & DAD");
    lv_obj_set_style_text_font(s_body, size_for(strlen(s_famText), true), 0);
    lv_label_set_text(s_body, s_famText);
    lv_obj_align(s_body, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_text_font(s_punch, &lv_font_montserrat_22, 0);
    lv_label_set_text(s_punch, KEVIN_FAMILY_SIGN);
    lv_obj_update_layout(s_body);
    lv_obj_align_to(s_punch, s_body, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);
    lv_obj_clear_flag(s_punch, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_hint, "push to close");
    printf("[daily] note from Mom & Dad: %s\n", s_famText);
}

void advance() {
    if (s_stage == FACT)       { if (s_setup[0]) show_joke(); else if (s_famText[0]) show_family(); else close_card(); }
    else if (s_stage == JOKE)  reveal();
    else if (s_stage == PUNCH && s_famText[0]) show_family();
    else                       close_card();
}

void tick(lv_timer_t *) {
    const uint32_t t = lv_tick_get() - s_since;
    if (s_stage == JOKE && t >= REVEAL_MS) reveal();
    else if ((s_stage == FACT || s_stage == PUNCH || s_stage == FAMILY) && t >= STAGE_MS) advance();
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
    // A note from Mom & Dad, when one is due: the next in turn, and from now on the clock
    // for the next one starts today. Marked as shown here, when the card opens, because the
    // card always reaches it (a push or its own timer moves it along).
    s_famText[0] = '\0';
#if KEVIN_FAMILY_EVERY > 0
    const long today = day_number(t);
    if (s_famForce || s_famDay == 0 || today - s_famDay >= KEVIN_FAMILY_EVERY) {
        const int n = daily_text::countLines(FAMILY_FILE);
        if (n > 0 && daily_text::lineFor(FAMILY_FILE, s_famIdx % n, s_famText, sizeof s_famText))
            save_family(today, (s_famIdx + 1) % n);
        s_famForce = false;
    }
#endif

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
    if (const char *e = getenv("SIM_FAMILY")) s_famForce = atoi(e) != 0;
#endif
    load_day();
    load_family();
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
