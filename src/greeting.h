#pragma once
#include <lvgl.h>

// Kevin's Orb — the startup greeting, in place of the stock "THE ORB" title card.
//
//   a scan line sweeps the dial, a ring draws itself round the edge,
//   "GOOD MORNING" / "GOOD AFTERNOON" / "GOOD EVENING" types on, then "Dr. Hollar",
//   then the date, holds, and warps (warp.h) into whatever screen is underneath.
//
// Kevin's photo sits above the text when /greeting/photo.png is on the SD card (a round
// PNG with a transparent outside, up to 240 px; tools/greeting/make_photo.py makes one from
// any picture). It is kept on the card, not in the code, so it never reaches GitHub.
//
// The greeting follows the Orb's clock. If the clock has not been set yet it says
// "WELCOME" instead of guessing the time of day.
//
// The animation starts on its first frame, not when show() is called: on the device the
// splash goes up during setup()'s blocking work and only starts moving once setup() pumps
// the UI at the end, so timing it from show() would skip straight to the end.
namespace greeting {
    // Who is greeted. Shown exactly as written.
    constexpr const char *NAME = "Dr. Hollar";

    // Build the greeting on the top layer and start it. Returns its container, so
    // ui_splash_status() can still write a boot status line on it. `gone` is called once the
    // container has been deleted (at the warp's midpoint), so the caller can drop its pointer.
    lv_obj_t *show(void (*gone)());
}
