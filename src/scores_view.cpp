// Kevin's Orb — Football Scores. See scores_view.h.
#include "scores_view.h"
#include "scores.h"
#include "orb_time.h"
#include <atomic>
#include <initializer_list>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {

// Same palette as the other dental screens. Accent: sky blue, chosen for Dr. Hollar's
// red/green colour blindness (blue stays blue to a red/green colour-blind eye).
const uint32_t COL_WHITE  = 0xF4F1E8;
const uint32_t COL_ACCENT = 0x5BC0FF;
const uint32_t COL_GREY   = 0x9A9A9A;
const uint32_t COL_DIM    = 0x6E6E6E;   // the losing side of a final
const uint32_t COL_HINT   = 0x5C5C5C;
const uint32_t COL_RULE   = 0x34383C;

const uint32_t LIVE_MS  = 30UL * 1000;        // a game is on: every 30 s
const uint32_t IDLE_MS  = 10UL * 60 * 1000;   // nothing on: every 10 min
const uint32_t RETRY_MS = 60UL * 1000;        // it failed: try again in a minute
const uint32_t STALE_MS = 60UL * 1000;        // entering with older scores than this refreshes

// Shared with the network task.
std::atomic<bool> s_active{false};   // the screen is up: fetching is allowed
std::atomic<bool> s_force{false};    // fetch on the next step, whatever the timer says
uint32_t          s_lastTry = 0;     // network task only

struct Row {
    lv_obj_t *bar, *name, *sub, *score, *mark;
};

lv_obj_t   *s_screen = nullptr;
lv_obj_t   *s_header = nullptr, *s_status = nullptr;
Row         s_row[2] = {};
lv_obj_t   *s_rule = nullptr;
lv_obj_t   *s_down = nullptr, *s_net = nullptr, *s_age = nullptr;
lv_obj_t   *s_big = nullptr, *s_small = nullptr;   // messages and bye weeks
lv_timer_t *s_timer = nullptr;
int         s_idx = 0;
int         s_shown = -1;
bool        s_waiting = false;   // a push asked for fresh scores and they have not come yet

void upper(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; ++i) dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = 0;
}

lv_obj_t *make_label(const lv_font_t *font, uint32_t color, lv_align_t align, int x, int y) {
    lv_obj_t *l = lv_label_create(s_screen);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_align(l, align, x, y);
    lv_label_set_text(l, "");
    return l;
}

// A team colour as a bar: decoration only. A colour too dark to see on black is lifted to a
// grey so the bar is still there.
lv_color_t bar_color(uint32_t c) {
    const int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    if (r * 3 + g * 6 + b < 0x40 * 10) return lv_color_hex(0x606060);
    return lv_color_hex(c);
}

const lv_font_t *name_font(const char *s) {
    const size_t n = strlen(s);
    if (n <= 9)  return &lv_font_montserrat_28;
    if (n <= 13) return &lv_font_montserrat_24;
    return &lv_font_montserrat_20;
}

// "TODAY 2:30 PM", "TOMORROW 7:00 PM", "SAT 10/31 · 2:30 PM", "SAT 10/31 · TIME TBA".
// Local time, from ESPN's UTC kickoff. Without a set clock, ESPN's own words (Eastern).
void kickoff_text(const ScoreGame &g, char *out, size_t cap) {
    struct tm now;
    if (!orb_local_time(&now) || !g.start) { upper(out, cap, g.detail); return; }
    struct tm k;
    time_t st = g.start;
    localtime_r(&st, &k);
    static const char *DOW[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    char when[24];
    const int h12 = k.tm_hour % 12 == 0 ? 12 : k.tm_hour % 12;
    snprintf(when, sizeof when, "%d:%02d %s", h12, k.tm_min, k.tm_hour < 12 ? "AM" : "PM");
    // Whole days apart, by calendar date rather than by 24-hour blocks.
    struct tm a = now, b = k;
    a.tm_hour = b.tm_hour = 12; a.tm_min = b.tm_min = a.tm_sec = b.tm_sec = 0;
    a.tm_isdst = b.tm_isdst = -1;
    const long days = (long)((mktime(&b) - mktime(&a)) / 86400);
    if (g.timeValid && days == 0)      snprintf(out, cap, "TODAY %s", when);
    else if (g.timeValid && days == 1) snprintf(out, cap, "TOMORROW %s", when);
    else if (g.timeValid)              snprintf(out, cap, "%s %d/%d | %s", DOW[k.tm_wday % 7], k.tm_mon + 1, k.tm_mday, when);
    else                               snprintf(out, cap, "%s %d/%d | TIME TBA", DOW[k.tm_wday % 7], k.tm_mon + 1, k.tm_mday);
}

void show_row(int r, const ScoreTeam &t, const ScoreGame &g) {
    Row &w = s_row[r];
    const bool final = g.state == GAME_FINAL;
    const bool loser = final && !t.winner;
    const uint32_t ink = loser ? COL_DIM : COL_WHITE;

    lv_obj_set_style_bg_color(w.bar, bar_color(t.color), 0);
    lv_obj_set_style_bg_opa(w.bar, loser ? LV_OPA_50 : LV_OPA_COVER, 0);

    char nm[40], buf[64];
    upper(nm, sizeof nm, t.main);
    lv_obj_set_style_text_font(w.name, name_font(nm), 0);
    lv_obj_set_style_text_color(w.name, lv_color_hex(ink), 0);
    if (t.rank) snprintf(buf, sizeof buf, "#5BC0FF %d# %s", t.rank, nm);   // rank in the accent
    else        snprintf(buf, sizeof buf, "%s", nm);
    lv_label_set_text(w.name, buf);

    if (t.sub[0] && t.record[0]) snprintf(buf, sizeof buf, "%s | %s", t.sub, t.record);
    else snprintf(buf, sizeof buf, "%s", t.sub[0] ? t.sub : t.record);
    lv_label_set_text(w.sub, buf);
    lv_obj_set_style_text_color(w.sub, lv_color_hex(loser ? COL_DIM : COL_GREY), 0);

    if (t.score >= 0) { snprintf(buf, sizeof buf, "%d", t.score); lv_label_set_text(w.score, buf); }
    else lv_label_set_text(w.score, "");
    lv_obj_set_style_text_color(w.score, lv_color_hex(ink), 0);

    // The marker beside the score: a pointer at the winner of a final, a football for the
    // team with the ball. Shapes, so they read the same in any colour vision.
    if (final && t.winner) {
        lv_label_set_text(w.mark, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_color(w.mark, lv_color_hex(COL_WHITE), 0);
        lv_obj_set_style_bg_opa(w.mark, LV_OPA_TRANSP, 0);
        lv_obj_set_size(w.mark, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_clear_flag(w.mark, LV_OBJ_FLAG_HIDDEN);
    } else if (g.state == GAME_LIVE && t.possession) {
        lv_label_set_text(w.mark, "");
        lv_obj_set_size(w.mark, 18, 11);   // a football
        lv_obj_set_style_bg_color(w.mark, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_bg_opa(w.mark, LV_OPA_COVER, 0);
        lv_obj_clear_flag(w.mark, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w.mark, LV_OBJ_FLAG_HIDDEN);
    }
    for (lv_obj_t *o : { w.bar, w.name, w.sub, w.score }) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void hide_rows(bool hide) {
    for (Row &w : s_row)
        for (lv_obj_t *o : { w.bar, w.name, w.sub, w.score, w.mark }) {
            if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            else      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    if (hide) lv_obj_add_flag(s_rule, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_clear_flag(s_rule, LV_OBJ_FLAG_HIDDEN);
}

void message(const char *big, const char *small) {
    hide_rows(true);
    lv_label_set_text(s_big, big);
    lv_label_set_text(s_small, small);
    lv_obj_clear_flag(s_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_small, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_down, "");
    lv_label_set_text(s_net, "");
}

void age_line(uint32_t fetchedMs, ScoresStatus st) {
    char buf[64], ago[24];
    if (!fetchedMs) { lv_label_set_text(s_age, ""); return; }
    const uint32_t s = (scores_now_ms() - fetchedMs) / 1000;
    if (s < 90)        snprintf(ago, sizeof ago, "just now");
    else if (s < 3600) snprintf(ago, sizeof ago, "%lu min ago", (unsigned long)(s / 60));
    else               snprintf(ago, sizeof ago, "%lu hr ago", (unsigned long)(s / 3600));
    if (s_waiting)            snprintf(buf, sizeof buf, "updating...");
    else if (st == SCORES_OK) snprintf(buf, sizeof buf, "updated %s", ago);
    else                      snprintf(buf, sizeof buf, "couldn't update | last %s", ago);
    lv_label_set_text(s_age, buf);
}

void render() {
    if (!s_screen) return;
    int count = 0; uint32_t fetchedMs = 0; ScoresStatus st = SCORES_NEVER; bool live = false;
    scores_meta(count, fetchedMs, st, live);
    age_line(fetchedMs, st);

    if (count == 0) {
        s_shown = -1;
        lv_label_set_text(s_header, "FOOTBALL SCORES");
        lv_label_set_text(s_status, "");
        if (st == SCORES_NEVER || (s_waiting && !fetchedMs)) message("Getting scores...", "from ESPN");
        else if (st == SCORES_NO_WIFI)    message("No Wi-Fi", "The Orb isn't connected to Wi-Fi.");
        else if (st == SCORES_FAILED)     message("Can't reach ESPN", "Will try again in a minute.");
        else if (st == SCORES_UNREADABLE) message("Can't read the scores", "ESPN sent something unexpected.");
        else                              message("No games this week", "SEC and NFL: off week");
        return;
    }
    if (s_idx >= count) s_idx = count - 1;
    if (s_idx < 0) s_idx = 0;
    ScoreGame g;
    if (!scores_game(s_idx, g)) return;
    s_shown = s_idx;

    char buf[64], tmp[40];
    snprintf(buf, sizeof buf, "%s | %d OF %d",
             g.fav >= 0 ? "MY TEAMS" : (g.league == LEAGUE_NFL ? "NFL" : "SEC"), s_idx + 1, count);
    lv_label_set_text(s_header, buf);

    if (g.state == GAME_BYE) {
        lv_label_set_text(s_status, "BYE WEEK");
        lv_obj_set_style_text_color(s_status, lv_color_hex(COL_GREY), 0);
        upper(tmp, sizeof tmp, g.home.main);
        message(tmp, "No game this week");
        return;
    }
    lv_obj_add_flag(s_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_small, LV_OBJ_FLAG_HIDDEN);
    hide_rows(false);

    // The status line says in words what state the game is in.
    if (g.state == GAME_LIVE) {
        upper(tmp, sizeof tmp, g.detail);
        snprintf(buf, sizeof buf, "LIVE | %s", tmp);
        lv_obj_set_style_text_color(s_status, lv_color_hex(COL_ACCENT), 0);
    } else if (g.state == GAME_FINAL) {
        upper(buf, sizeof buf, g.detail[0] ? g.detail : "Final");
        lv_obj_set_style_text_color(s_status, lv_color_hex(COL_WHITE), 0);
    } else {
        kickoff_text(g, buf, sizeof buf);
        lv_obj_set_style_text_color(s_status, lv_color_hex(COL_WHITE), 0);
    }
    lv_label_set_text(s_status, buf);

    show_row(0, g.away, g);
    show_row(1, g.home, g);

    // Down and distance, led by who has the ball, in words.
    if (g.state == GAME_LIVE && g.down[0]) {
        const char *who = g.away.possession ? g.away.abbr : g.home.possession ? g.home.abbr : "";
        if (who[0]) snprintf(buf, sizeof buf, "%s ball | %s", who, g.down);
        else        snprintf(buf, sizeof buf, "%s", g.down);
        lv_label_set_text(s_down, buf);
    } else {
        lv_label_set_text(s_down, "");
    }
    if (g.network[0]) snprintf(buf, sizeof buf, "TV: %s", g.network);
    else buf[0] = 0;
    lv_label_set_text(s_net, buf);
}

void tick(lv_timer_t *) { render(); }

void build_row(int r, int y) {
    Row &w = s_row[r];
    w.bar = lv_obj_create(s_screen);
    lv_obj_remove_style_all(w.bar);
    lv_obj_set_size(w.bar, 6, 54);
    lv_obj_set_style_radius(w.bar, 3, 0);
    lv_obj_set_style_bg_opa(w.bar, LV_OPA_COVER, 0);
    lv_obj_align(w.bar, LV_ALIGN_TOP_LEFT, 66, y);

    w.name = make_label(&lv_font_montserrat_28, COL_WHITE, LV_ALIGN_TOP_LEFT, 82, y - 2);
    lv_label_set_recolor(w.name, true);
    lv_label_set_long_mode(w.name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(w.name, 230);
    w.sub = make_label(&lv_font_montserrat_14, COL_GREY, LV_ALIGN_TOP_LEFT, 84, y + 34);
    lv_label_set_long_mode(w.sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(w.sub, 220);

    w.score = make_label(&lv_font_montserrat_48, COL_WHITE, LV_ALIGN_TOP_RIGHT, -76, y - 2);
    lv_obj_set_style_text_align(w.score, LV_TEXT_ALIGN_RIGHT, 0);

    w.mark = lv_label_create(s_screen);
    lv_obj_set_style_text_font(w.mark, &lv_font_montserrat_20, 0);
    lv_obj_set_style_radius(w.mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(w.mark, LV_ALIGN_TOP_RIGHT, -50, y + 18);
    lv_obj_add_flag(w.mark, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

lv_obj_t *scoresview::build() {
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

    s_header = make_label(&lv_font_montserrat_14, COL_ACCENT, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_text_letter_space(s_header, 2, 0);
    s_status = make_label(&lv_font_montserrat_20, COL_WHITE, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_style_text_letter_space(s_status, 1, 0);

    build_row(0, 136);
    s_rule = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_rule);
    lv_obj_set_size(s_rule, 300, 1);
    lv_obj_set_style_bg_color(s_rule, lv_color_hex(COL_RULE), 0);
    lv_obj_set_style_bg_opa(s_rule, LV_OPA_COVER, 0);
    lv_obj_align(s_rule, LV_ALIGN_TOP_MID, 0, 205);
    build_row(1, 220);

    s_down = make_label(&lv_font_montserrat_16, COL_WHITE, LV_ALIGN_TOP_MID, 0, 296);
    s_net  = make_label(&lv_font_montserrat_14, COL_GREY, LV_ALIGN_TOP_MID, 0, 322);
    s_age  = make_label(&lv_font_montserrat_12, COL_GREY, LV_ALIGN_TOP_MID, 0, 356);
    lv_obj_t *hint = make_label(&lv_font_montserrat_12, COL_HINT, LV_ALIGN_TOP_MID, 0, 378);
    lv_label_set_text(hint, "turn: next game   push: refresh");

    s_big = make_label(&lv_font_montserrat_28, COL_WHITE, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_set_style_text_align(s_big, LV_TEXT_ALIGN_CENTER, 0);
    s_small = make_label(&lv_font_montserrat_16, COL_GREY, LV_ALIGN_TOP_MID, 0, 222);
    lv_obj_set_style_text_align(s_small, LV_TEXT_ALIGN_CENTER, 0);

    render();
    return s_screen;
}

void scoresview::destroy() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    if (s_screen) { lv_obj_del(s_screen); s_screen = nullptr; }
    s_header = s_status = s_rule = s_down = s_net = s_age = s_big = s_small = nullptr;
    for (Row &w : s_row) w = Row{};
}

void scoresview::onTurn(int delta) {
    int count = 0; uint32_t f = 0; ScoresStatus st; bool live;
    scores_meta(count, f, st, live);
    if (count > 0) s_idx = ((s_idx + delta) % count + count) % count;
    render();
}

void scoresview::onPress() {
    s_waiting = true;
    s_force = true;
    render();
}

void scoresview::onEnter() {
    s_idx = 0;
    int count = 0; uint32_t fetchedMs = 0; ScoresStatus st; bool live;
    scores_meta(count, fetchedMs, st, live);
    if (!fetchedMs || scores_now_ms() - fetchedMs > STALE_MS) s_force = true;
    s_active = true;
    if (!s_timer) s_timer = lv_timer_create(tick, 30000, nullptr);
    render();
}

void scoresview::onExit() {
    s_active = false;
    s_waiting = false;
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
}

bool scoresview::fetchStep() {
    if (!s_active) return false;
    const uint32_t now = scores_now_ms();
    int count = 0; uint32_t fetchedMs = 0; ScoresStatus st = SCORES_NEVER; bool live = false;
    scores_meta(count, fetchedMs, st, live);
    const uint32_t due = st == SCORES_OK ? (live ? LIVE_MS : IDLE_MS) : RETRY_MS;
    if (!s_force && s_lastTry && now - s_lastTry < due) return false;
    s_force = false;
    s_lastTry = now ? now : 1;
    scores_fetch_now();
    return true;
}

void scoresview::onScoresReady() {
    s_waiting = false;
    render();
}

int scoresview::index() { return s_shown; }
