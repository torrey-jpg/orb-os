#pragma once
#include <lvgl.h>

// Kevin's Orb — Weather: today in one town (KEVIN_WX_* in config.h, Lufkin, TX).
//
// The temperature now with a picture and words for the sky, today's high and low, chance of
// rain, wind, sunset (or sunrise after dark), and the next hours along the bottom. Turn the
// knob to slide the hours along; push to refresh; rock for the menu.
//
// Fetched only while the screen is up: on entering (if the last one is over 10 minutes
// old) and every 15 minutes after. Each reply is about 3 KB and is given back as soon as it
// is read. See kwx.h.
namespace kwxview {
    lv_obj_t *build();
    void      destroy();
    void      onPress();          // refresh now
    void      onTurn(int delta);  // slide the hourly strip
    void      onEnter();
    void      onExit();

    bool      fetchStep();        // network task (core 0): true when the store changed
    void      onWeatherReady();   // UI thread (core 1)

    int       hourOffset();       // for the selftest
}
