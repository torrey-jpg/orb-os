#pragma once
#include <lvgl.h>

// Kevin's Orb — Dental Fact of the Day.
//
// One dental fact a day from /facts/dental_facts.txt on the SD card: 366 lines, line 1 for
// January 1 through line 366 for December 31, with line 60 for February 29 (shown only in
// leap years). Lines starting with # are notes. The list is ours, checked and offline, so it
// needs no internet and never changes unless the file does.
//
// Turn the knob to look at other days' facts; push to come back to today. Rock for the menu.
// The file (~35 KB) is read when the screen is entered and released when it is left.
namespace factview {
    lv_obj_t *build();
    void      destroy();
    void      onPress();          // back to today
    void      onTurn(int delta);  // a day earlier / later
    void      onEnter();
    void      onExit();
    int       index();            // the fact shown (0..365), -1 if none; for the selftest
}
