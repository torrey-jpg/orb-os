#pragma once
#include <lvgl.h>

// Kevin's Orb — the warp transition.
//
// When an app is chosen from the menu, the dial jumps to hyperspace instead of cutting:
// light streaks rush out from the centre while the dark closes in from the rim, the screen
// swaps while the dial is fully dark, a flash marks the arrival, and the dark opens back out
// to reveal the new screen. About one second, with a whoosh from the speaker
// (/sounds/warp.pcm on the SD card: raw PCM, 16 kHz, 16-bit, stereo, the format audio.cpp
// streams; tools/sounds/make_warp.py writes it).
//
// Everything is drawn on lv_layer_top(), above whatever screen is loaded, out of a few dozen
// small LVGL objects that exist only while the warp runs.
namespace warp {
    // Start a warp. `swap` is called once, at the midpoint, while the dial is fully dark;
    // it is where the caller loads the new screen. `arg` is handed back to it.
    // If a warp is already running, the swap happens immediately instead.
    void start(void (*swap)(int arg), int arg);

    // True from start() until the reveal finishes. The app shell ignores the knob meanwhile,
    // so a turn cannot land on a screen that is halfway through arriving.
    bool active();

    // Off: every start() swaps immediately, the old instant cut. On by default.
    void setEnabled(bool on);
}
