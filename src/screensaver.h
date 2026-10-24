#pragma once
#include <stdint.h>

// Kevin's Orb — the hologram screensaver.
//
// After a spell with nobody touching the knob or the screen, a tooth appears as a mint
// hologram above a glowing emitter: it turns slowly, a scan band sweeps through it, it
// flickers now and then, and every half minute it glitches to a different tooth. The time
// stays on screen above it, so the Orb is still a clock while it plays, and the whole
// picture drifts a few pixels over minutes so nothing sits in one place on the AMOLED.
//
// Any turn, push or touch brings back the screen that was there, and that first input is
// used up waking it rather than also acting on the screen underneath.
//
// It draws from the same /teeth pictures as the Tooth Explorer (tooth_frames.h) and holds
// them only while it is showing.
namespace screensaver {
    // How long with no input before it starts. The simulator honours SIM_SAVER_MS so it can
    // be watched without waiting.
    constexpr uint32_t IDLE_MS = 10u * 60u * 1000u;

    void begin();          // once, after the app shell has started
    bool showing();

    // Every knob input goes through here first (input_router). Restarts the idle clock and,
    // if the screensaver is up, closes it and returns true: the caller drops that input.
    bool wake();

    void setEnabled(bool on);   // off: never starts (and closes it if it is up)
}
