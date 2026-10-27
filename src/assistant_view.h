#pragma once
#include <lvgl.h>

// Kevin's Orb — Assistant: ask a question out loud, hear (and read) ChatGPT's answer.
//
// Push to start listening; push again when done (or just stop talking: a pause ends it).
// While it thinks, a spinner turns. The answer is spoken through the speaker and shown as
// text; turn the knob to scroll a long one, push to stop it talking or to ask another.
// Follow-up questions remember the last three exchanges until the screen is left.
//
// The work happens on the network task (assist_step in assistant.cpp); this screen asks and
// shows. Colour-blind friendly: every state is in words; the accent is the usual sky blue.
namespace assistview {
    lv_obj_t *build();
    void      destroy();
    void      onPress();
    void      onTurn(int delta);
    void      onEnter();
    void      onExit();

    bool      fetchStep();        // network task (core 0): true when something changed
    void      onReady();          // UI thread (core 1): redraw
}
