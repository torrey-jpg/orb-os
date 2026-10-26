#pragma once
// Kevin's Orb — football scores: the data, the fetch, and the hand-off between the network
// task (core 0, which fetches) and the screen (core 1, which draws).
//
// Source: ESPN's public scoreboard feed (site.api.espn.com), free, no key, HTTPS. It is not
// an official, documented API, so it can change without notice; everything that reads it is
// in scores.cpp, and a reply it cannot understand shows as "can't read the scores", never as
// wrong numbers.
//
//   College: the SEC's games this week (scoreboard?groups=8; 8 is the SEC on ESPN).
//   NFL:     this week's games, of which only the Cowboys' and the Chiefs' are kept.
//
// Kevin's teams come first, in the order he named them: Arkansas, Texas, Cowboys, Chiefs.
// Matched by ESPN's team id, not by name, so a renamed or re-abbreviated team still matches.
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define SCORES_MAX_GAMES 24

enum ScoreLeague : uint8_t { LEAGUE_NFL = 0, LEAGUE_CFB = 1 };
enum ScoreState  : uint8_t { GAME_PRE = 0, GAME_LIVE = 1, GAME_FINAL = 2, GAME_BYE = 3 };

struct ScoreTeam {
    int32_t  id;
    int16_t  score;        // -1 when there is no score yet
    uint8_t  rank;         // AP/CFP rank 1..25, 0 when unranked (and always 0 in the NFL)
    bool     possession;   // has the ball (live games)
    bool     winner;       // won (final games)
    char     abbr[8];      // "ARK"
    char     main[24];     // the big name: "Arkansas" (college) or "Cowboys" (NFL)
    char     sub[24];      // the small name: "Razorbacks" (college) or "Dallas" (NFL)
    char     record[10];   // "5-2"
    uint32_t color;        // the team's colour, decoration only: the name always says who it is
};

struct ScoreGame {
    ScoreLeague league;
    ScoreState  state;
    int8_t      fav;          // 0..3 = one of Kevin's teams (in his order), -1 otherwise
    bool        timeValid;    // false when the kickoff time is still "TBA"
    time_t      start;        // kickoff, UTC
    char        detail[24];   // ESPN's own short status: "4:32 - 2nd", "Halftime", "Final/OT"
    char        network[16];  // "ESPN", "SECN", "CBS"
    char        down[40];     // "3rd & 7 at ARK 45" while live, else empty
    ScoreTeam   away, home;
};

enum ScoresStatus : uint8_t {
    SCORES_NEVER = 0,   // not asked yet
    SCORES_OK,          // a good set is held (it may hold no games: an off week)
    SCORES_NO_WIFI,     // the Orb is not on WiFi
    SCORES_FAILED,      // ESPN did not answer
    SCORES_UNREADABLE,  // ESPN answered with something this code cannot read
};

// ---- the store (any thread) -------------------------------------------------------------
// A count, when it arrived, and what happened last time; then one game at a time, so the
// screen never has to copy the whole set onto a stack.
void scores_meta(int &count, uint32_t &fetchedMs, ScoresStatus &status, bool &anyLive);
bool scores_game(int i, ScoreGame &out);
// The clock fetchedMs is measured on, so the screen can say how old the scores are.
uint32_t scores_now_ms();

// ---- the fetch (network task only) ------------------------------------------------------
// Fetches both leagues, sorts, and stores. Returns true when the store changed (a fresh set,
// or a new failure state the screen should show). Blocks for a few seconds at most.
bool scores_fetch_now();

// The simulator can read two saved ESPN replies instead of the network:
// SIM_SCORES_NFL=/path/nfl.json SIM_SCORES_CFB=/path/sec.json
