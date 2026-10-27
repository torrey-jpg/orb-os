// Kevin's Orb — Assistant screen. See assistant_view.h.
#include "assistant_view.h"
#include "assistant.h"
#include <stdio.h>
#include <string.h>

namespace {

const uint32_t COL_WHITE  = 0xF4F1E8;
const uint32_t COL_ACCENT = 0x5BC0FF;   // sky blue, the colour-blind-safe accent
const uint32_t COL_GREY   = 0x9A9A9A;
const uint32_t COL_HINT   = 0x5C5C5C;

const int RING = 132;                   // the listening ring, at rest
const int RING_CY = 190;                // its centre, from the top

lv_obj_t   *s_screen = nullptr;
lv_obj_t   *s_title = nullptr, *s_speaker = nullptr;
lv_obj_t   *s_ring = nullptr, *s_spin = nullptr, *s_inner = nullptr, *s_status = nullptr;
lv_obj_t   *s_q = nullptr, *s_box = nullptr, *s_a = nullptr;
lv_obj_t   *s_msg = nullptr, *s_hint = nullptr;
lv_timer_t *s_timer = nullptr;
AssistState s_shown = (AssistState)255;

void show(lv_obj_t *o, bool on) { if (o) { on ? lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); } }

lv_obj_t *label(const lv_font_t *f, uint32_t col, int y, int w = 0) {
    lv_obj_t *l = lv_label_create(s_screen);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (w) { lv_obj_set_width(l, w); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP); }
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(l, "");
    return l;
}

void ring_size(int d) {
    lv_obj_set_size(s_ring, d, d);
    lv_obj_align(s_ring, LV_ALIGN_TOP_MID, 0, RING_CY - d / 2);
}

// Full redraw for a new state; the level meter alone is cheaper (see tick()).
void render() {
    if (!s_screen) return;
    const AssistState st = assist_state();
    const AssistState was = s_shown;
    s_shown = st;
    char q[400], a[1200], err[160], buf[480];
    assist_texts(q, sizeof q, a, sizeof a, err, sizeof err);

    const bool answerMode = st == AS_SPEAKING || st == AS_ANSWERED;
    const bool busy = st == AS_HEARD || st == AS_THINKING || st == AS_VOICING;
    show(s_ring, !answerMode && st != AS_ERROR && st != AS_NOKEY && !busy);
    show(s_spin, busy);
    show(s_inner, !answerMode && st != AS_ERROR && st != AS_NOKEY);
    show(s_status, !answerMode && st != AS_ERROR && st != AS_NOKEY);
    show(s_q, answerMode);
    show(s_box, answerMode);
    show(s_msg, st == AS_ERROR || st == AS_NOKEY);
    show(s_speaker, st == AS_SPEAKING);
    ring_size(RING);
    lv_obj_set_style_border_opa(s_ring, st == AS_LISTENING ? LV_OPA_COVER : LV_OPA_60, 0);
    lv_obj_set_style_bg_opa(s_ring, st == AS_LISTENING ? LV_OPA_20 : LV_OPA_TRANSP, 0);

    switch (st) {
        case AS_NOKEY:
            lv_label_set_text(s_msg, "Add your OpenAI key\nto src/secrets.h");
            lv_label_set_text(s_hint, "");
            break;
        case AS_IDLE:
            lv_label_set_text(s_inner, "Push\nto ask");
            lv_label_set_text(s_status, "Ask me anything.");
            lv_label_set_text(s_hint, "push: start talking");
            break;
        case AS_LISTENING:
            lv_label_set_text(s_inner, "Listening");
            lv_label_set_text(s_status, "Push again when you're done,\nor just pause.");
            lv_label_set_text(s_hint, "push: done");
            break;
        case AS_HEARD:
            lv_label_set_text(s_inner, "Got it");
            lv_label_set_text(s_status, "Working out what you said...");
            lv_label_set_text(s_hint, "");
            break;
        case AS_THINKING:
        case AS_VOICING:
            lv_label_set_text(s_inner, "Thinking");
            snprintf(buf, sizeof buf, "\"%s\"", q);
            lv_label_set_text(s_status, buf);
            lv_label_set_text(s_hint, "");
            break;
        case AS_SPEAKING:
        case AS_ANSWERED:
            snprintf(buf, sizeof buf, "You asked: %s", q);
            lv_label_set_text(s_q, buf);
            lv_label_set_text(s_a, a);
            // A short answer sits in the middle of its box; a long one starts at the top and scrolls.
            lv_obj_update_layout(s_box);
            {
                const int h = lv_obj_get_height(s_a), boxH = lv_obj_get_height(s_box);
                lv_obj_set_y(s_a, h < boxH ? (boxH - h) / 2 : 0);
            }
            if (was != st) lv_obj_scroll_to_y(s_box, 0, LV_ANIM_OFF);   // a new answer starts at the top
            lv_label_set_text(s_hint, st == AS_SPEAKING ? "push: stop   turn: scroll" : "push: ask again   turn: scroll");
            break;
        case AS_ERROR:
            lv_label_set_text(s_msg, err[0] ? err : "Something went wrong.");
            lv_label_set_text(s_hint, "push: try again");
            break;
    }
}

// While listening, the ring breathes with the voice: wider and brighter as it gets louder.
void tick(lv_timer_t *) {
    const AssistState st = assist_state();
    if (st != s_shown) { render(); return; }
    if (st == AS_LISTENING) ring_size(RING + assist_level() * 6 / 10);
}

}  // namespace

lv_obj_t *assistview::build() {
    if (s_screen) return s_screen;
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *edge = lv_arc_create(s_screen);
    lv_obj_remove_style_all(edge);
    lv_obj_set_size(edge, 430, 430);
    lv_obj_center(edge);
    lv_arc_set_bg_angles(edge, 0, 0);
    lv_arc_set_angles(edge, 0, 360);
    lv_obj_set_style_arc_color(edge, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(edge, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(edge, LV_OPA_40, LV_PART_INDICATOR);
    lv_obj_clear_flag(edge, LV_OBJ_FLAG_CLICKABLE);

    s_title = label(&lv_font_montserrat_14, COL_ACCENT, 50);
    lv_obj_set_style_text_letter_space(s_title, 3, 0);
    lv_label_set_text(s_title, "ASSISTANT");
    s_speaker = label(&lv_font_montserrat_16, COL_ACCENT, 47);
    lv_obj_align(s_speaker, LV_ALIGN_TOP_MID, 72, 47);
    lv_label_set_text(s_speaker, LV_SYMBOL_VOLUME_MAX);

    // The ring: listening, it breathes; thinking, a spinner turns in its place.
    s_ring = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ring, 3, 0);
    lv_obj_set_style_border_color(s_ring, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_color(s_ring, lv_color_hex(COL_ACCENT), 0);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    ring_size(RING);

    s_spin = lv_spinner_create(s_screen, 1100, 70);
    lv_obj_set_size(s_spin, RING, RING);
    lv_obj_align(s_spin, LV_ALIGN_TOP_MID, 0, RING_CY - RING / 2);
    lv_obj_set_style_arc_color(s_spin, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_spin, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spin, lv_color_hex(0x262A2E), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spin, 4, LV_PART_MAIN);
    lv_obj_clear_flag(s_spin, LV_OBJ_FLAG_CLICKABLE);

    s_inner = label(&lv_font_montserrat_20, COL_WHITE, 0);
    lv_obj_align(s_inner, LV_ALIGN_TOP_MID, 0, RING_CY - 24);
    s_status = label(&lv_font_montserrat_16, COL_GREY, 276, 320);

    // The answer: the question small above, the answer large in a box the knob scrolls.
    s_q = label(&lv_font_montserrat_14, COL_GREY, 80, 300);
    lv_label_set_long_mode(s_q, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_q, 38);
    s_box = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, 340, 236);
    lv_obj_align(s_box, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_set_scrollbar_mode(s_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_box, LV_OBJ_FLAG_CLICKABLE);
    s_a = lv_label_create(s_box);
    lv_obj_set_width(s_a, 340);
    lv_label_set_long_mode(s_a, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_a, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_a, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_align(s_a, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_a, 4, 0);

    s_msg = label(&lv_font_montserrat_20, COL_WHITE, 170, 320);
    s_hint = label(&lv_font_montserrat_12, COL_HINT, 392);

    render();
    return s_screen;
}

void assistview::destroy() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    if (s_screen) { lv_obj_del(s_screen); s_screen = nullptr; }
    s_title = s_speaker = s_ring = s_spin = s_inner = s_status = s_q = s_box = s_a = s_msg = s_hint = nullptr;
    s_shown = (AssistState)255;
}

void assistview::onPress() { assist_push(); render(); }

void assistview::onTurn(int delta) {
    if (s_box && !lv_obj_has_flag(s_box, LV_OBJ_FLAG_HIDDEN)) lv_obj_scroll_by_bounded(s_box, 0, -delta * 40, LV_ANIM_ON);
}

void assistview::onEnter() {
    assist_open();
    if (!s_timer) s_timer = lv_timer_create(tick, 60, nullptr);
    render();
}

void assistview::onExit() {
    assist_close();
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
}

bool assistview::fetchStep() { return assist_step(); }
void assistview::onReady() { render(); }
