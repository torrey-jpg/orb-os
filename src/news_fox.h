#pragma once
// Kevin's Orb — the News screen's headlines from Fox News, read straight from Fox's own
// "latest" RSS feed (https://moxie.foxnews.com/google-publisher/latest.xml) instead of Zion's
// BBC/Guardian gateway. Switched on by KEVIN_NEWS_FOX in config.h; intel_client.cpp hands
// both of its calls here, so the News screen itself is unchanged.
//
// The feed was measured on 2026-10-26: about 170 KB, 25 stories, sent with its length up
// front, so it streams straight into PSRAM and is given back as soon as it has been read.
// Titles and summaries are plain text with &amp; / &apos; entities. Sportsbook promotions
// (Fox's "outkick-betting" section, 7 of those 25) are left out: they are adverts, not news.
#include "intel.h"
#include <stddef.h>

bool news_fox_fetch(int want, IntelSnapshot &out);
IntelBriefState news_fox_brief(const char *key, char *headlineOut, size_t headlineCap,
                               char *sourceOut, size_t sourceCap, char *bodyOut, size_t bodyCap);

// The simulator can read a saved copy of the feed instead: SIM_FOX_FILE=/path/latest.xml
