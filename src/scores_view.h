#pragma once
#include <lvgl.h>

// Kevin's Orb — Football Scores.
//
// One game per page: Kevin's teams first (Arkansas, Texas, Cowboys, Chiefs), then the rest of
// the SEC's games this week, live ones first. Turn the knob for the next game; push to
// refresh now; rock for the menu.
//
// Fetched only while the screen is up: when it is entered, every 30 seconds while any game is
// live, every 10 minutes otherwise. The ESPN reply is read and given straight back; what is
// kept is the game list (about 6 KB, in PSRAM). See scores.h.
//
// Colour-blind friendly by construction: who won is shown by a pointer shape and bright vs
// dim text, who has the ball by a football shape and the words "ARK ball", and live games by
// the word LIVE. The team colour bars are decoration; every fact is also in words.
namespace scoresview {
    lv_obj_t *build();
    void      destroy();
    void      onPress();          // refresh now
    void      onTurn(int delta);  // next / previous game
    void      onEnter();
    void      onExit();

    // Network task (core 0): fetch if due. Returns true when the store changed, and the
    // UI thread should then call onScoresReady().
    bool      fetchStep();
    // UI thread (core 1): fresh scores are in the store.
    void      onScoresReady();

    int       index();            // the game shown, for the selftest
}
