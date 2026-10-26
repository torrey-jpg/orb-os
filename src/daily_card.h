#pragma once

// Kevin's Orb — the first touch of the day.
//
// The first time each day the knob is turned or pushed (or the screen touched to wake the
// screensaver), a card comes up over whatever is on screen with the Dental Fact of the Day.
// The next touch, or 25 seconds, turns it into the Dental Joke of the Day: the setup first,
// the punchline a beat later (a touch shows it straight away). One more touch, or another
// 25 seconds, closes it. The touches that move the card along are used up by it, so they
// never also act on the screen underneath.
//
// Once a day: the date it last showed is saved (NVS on the Orb), so a restart does not show
// it again. Nothing shows until the Orb's clock has been set. Facts and jokes come from
// /facts/dental_facts.txt and /facts/dental_jokes.txt (daily_text.h). In the simulator,
// SIM_DAILY=1 shows it on every first touch, so it can be seen more than once a day.
namespace daily_card {
    void begin();

    // Every knob input, through input_router. If the card is up, moves it along and returns
    // true (the caller drops the input).
    bool consume();

    // A first touch: shows the card if it has not been shown today. Returns true if it did.
    bool maybeShow();

    bool showing();
}
