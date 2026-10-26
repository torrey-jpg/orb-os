#pragma once
#include <lvgl.h>

// Kevin's Orb — Wedding countdown: Kevin & Cayleigh, Saturday May 29, 2027
// (KEVIN_WEDDING_* in config.h).
//
// Before the day: the days to go in large figures, then hours : minutes : seconds ticking
// down to midnight, a gold ring round the dial that fills over the final year, and two
// interlocked rings at the top. Turn the knob for other ways of counting (weeks, months,
// hours, sleeps). On the day: "TODAY'S THE DAY". After it: days married, and the next
// anniversary.
//
// Offline, nothing fetched. Gold is decoration; every number is also in words.
namespace weddingview {
    lv_obj_t *build();
    void      destroy();
    void      onPress();
    void      onTurn(int delta);
    void      onEnter();
    void      onExit();
    int       daysToGo();   // for the selftest; negative after the day
}
