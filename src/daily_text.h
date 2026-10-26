#pragma once
#include <stddef.h>
#include <time.h>

// Kevin's Orb — reading the day's line from a 366-line file on the SD card
// (/facts/dental_facts.txt, /facts/dental_jokes.txt). Shared by the Dental Fact screen and
// the first-touch-of-the-day cards.
//
// The files are laid out on a leap-year calendar: line 1 is January 1, line 60 is
// February 29, line 366 is December 31. Lines starting with # are notes and blank lines are
// skipped. In an ordinary year every date from March 1 on skips the February 29 line.
namespace daily_text {
    constexpr int DAYS = 366;

    // 0..365 for a date, following the layout above.
    int indexFor(const struct tm &t);

    // Copy line `index` (0-based, notes skipped) of `path` into `out`. False if the file is
    // missing or shorter than that.
    bool lineFor(const char *path, int index, char *out, size_t cap);
}
