#pragma once
#include <lvgl.h>

// Kevin's Orb — Tooth Explorer.
//
// Turn the knob to step through the 32 permanent teeth (Universal numbering). A ring of 32
// dots around the dial is the dental arch, upper teeth across the top and lower across the
// bottom, as on a chart viewed facing the patient; the current tooth is lit.
//
// The tooth itself is shown in 3D in the middle of the dial. It spins slowly on its own;
// drag to turn it by hand (a finger on the Orb, the mouse in the simulator): sideways spins
// it about its long axis, up and down tumbles it end over end, both all the way round. The
// pictures are pre-rendered every 15 degrees both ways (24 x 24 per tooth) on the SD card,
// one file per tooth type: /teeth/t00.bin .. t15.bin (tools/teeth/render_teeth.py).
// Left-side teeth are the right-side pictures mirrored.
//
// Push the knob for the fun-fact card. Rock the knob (as everywhere) for the menu.
//
// Registered with app_shell::addLazy(). While this screen is on the dial it holds the
// current tooth's open file, its frame table (~5 KB), one compressed picture (<30 KB) and
// one decoded picture (~95 KB), all given back on exit (docs/memory.md).
namespace toothview {
    lv_obj_t *build();
    void      destroy();
    void      onPress();          // show / hide the fun-fact card
    void      onTurn(int delta);  // step teeth, wrapping 32 -> 1
    void      onEnter();
    void      onExit();
    int       current();          // the tooth shown (1..32), for the simulator's selftest
    int       frame();            // the picture frame shown, -1 if none (selftest)
}
