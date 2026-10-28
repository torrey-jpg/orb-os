#pragma once
// Kevin's Orb — the screensaver's picture scenes: an original picture from the SD card
// (/saver/<name>.png, painted by tools/saver/render_*.py) with live light drawn over it —
// twinkling stars and shooting stars, rising bubbles and pulsing jellyfish, glowing spots
// and drifting lights. screensaver.cpp decides when; this file draws one.
//
// Memory: the picture (424 KB) and the light sprites (about 75 KB) are taken from PSRAM in
// show() and every byte is given back in hide().
#include <lvgl.h>
#include <stdint.h>

namespace saver_scene {
    int  count();                         // how many scenes there are
    const char *name(int idx);            // "cosmic", "ocean", ...
    // Builds scene idx inside parent (full screen). False if its picture could not be read;
    // nothing is left allocated in that case.
    bool show(lv_obj_t *parent, int idx);
    void tick();                          // advance the animation; call every frame
    void hide();                          // delete everything and free the memory
    bool active();
}
