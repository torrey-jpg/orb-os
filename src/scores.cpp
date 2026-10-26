// Kevin's Orb — football scores. See scores.h.
#include "scores.h"
#include "net_fetch.h"
#include "config.h"
#include <ArduinoJson.h>
#include <mutex>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#else
#include <chrono>
#include <cstdarg>
static struct { void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } } Serial;
static uint32_t millis() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
static void *heap_caps_malloc(size_t n, int) { return malloc(n); }
static void  heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_SPIRAM 0
#endif

namespace {

const char *URL_NFL = "https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard";
const char *URL_CFB = "https://site.api.espn.com/apis/site/v2/sports/football/college-football/scoreboard?groups=8&limit=100";
// A week of NFL games measured 411 KB and the top-25 college board 878 KB (saved ESPN
// replies, 2022). The SEC board is a subset of a similar size. The body lives in PSRAM only
// for the moment it is being read, and is given back straight after.
const size_t BODY_MAX = 1536 * 1024;
// The name these requests introduce themselves with. ESPN refuses the Orb's usual one
// (ORB_USER_AGENT, "TheOrbOS (ESP32-S3 hobby; +https://...)") with HTTP 403, and accepts this,
// the ESP32 library's default and a plain browser-style one: tested from a Mac, 2026-10-25.
const char *ESPN_USER_AGENT = "KevinOrb/1.0";

// Kevin's teams, in the order he named them. ESPN team ids (confirmed against saved ESPN
// replies): Arkansas 8, Texas 251, Dallas 6, Kansas City 12.
struct Fav { ScoreLeague league; int32_t id; const char *main; const char *sub; };
const Fav FAVS[] = {
    { LEAGUE_CFB,   8, "Arkansas", "Razorbacks" },
    { LEAGUE_CFB, 251, "Texas",    "Longhorns" },
    { LEAGUE_NFL,   6, "Cowboys",  "Dallas" },
    { LEAGUE_NFL,  12, "Chiefs",   "Kansas City" },
};
const int NFAV = (int)(sizeof(FAVS) / sizeof(FAVS[0]));

// The store, and the network task's working copy. Both in PSRAM, taken the first time scores
// are fetched and never given back: about 13 KB together, and never freeing them means the
// screen (core 1) can never be reading one while the network task (core 0) takes it away.
// That is the same choice docs/memory.md describes for the weather map's masks.
std::mutex   s_mutex;
ScoreGame   *s_store = nullptr;
int          s_count = 0;
uint32_t     s_fetchedMs = 0;
ScoresStatus s_status = SCORES_NEVER;

ScoreGame *s_work = nullptr;
int        s_workCount = 0;

void copy_str(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    if (!src) { dst[0] = 0; return; }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

// "2026-10-24T19:30Z" -> seconds since 1970, UTC. Written out by hand: timegm() is not
// on every C library this builds against, and mktime() would apply the local time zone.
time_t parse_iso_utc(const char *s) {
    int Y = 0, M = 0, D = 0, h = 0, m = 0;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d", &Y, &M, &D, &h, &m) < 3) return 0;
    // days from 1970-01-01 (Howard Hinnant's days_from_civil)
    Y -= M <= 2;
    const long era = (Y >= 0 ? Y : Y - 399) / 400;
    const unsigned yoe = (unsigned)(Y - era * 400);
    const unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097 + (long)doe - 719468;
    return (time_t)days * 86400 + h * 3600 + m * 60;
}

uint32_t parse_hex_color(const char *s) {
    if (!s || !*s) return 0x808080;
    return (uint32_t)strtoul(s, nullptr, 16) & 0xFFFFFF;
}

int fav_index(ScoreLeague lg, int32_t id) {
    for (int i = 0; i < NFAV; ++i)
        if (FAVS[i].league == lg && FAVS[i].id == id) return i;
    return -1;
}

// Only the fields the screen shows survive the parse; ESPN's reply is mostly links, stats
// and leaders, which the filter drops as it reads, so the parsed document stays small.
void build_filter(JsonDocument &f) {
    JsonObject ev = f["events"].add<JsonObject>();
    ev["date"] = true;
    ev["status"]["type"]["state"] = true;
    ev["status"]["type"]["shortDetail"] = true;
    JsonObject c = ev["competitions"].add<JsonObject>();
    c["timeValid"] = true;
    c["situation"]["possession"] = true;
    c["situation"]["downDistanceText"] = true;
    c["broadcasts"].add<JsonObject>()["names"] = true;
    JsonObject t = c["competitors"].add<JsonObject>();
    t["homeAway"] = true;
    t["score"] = true;
    t["winner"] = true;
    t["curatedRank"]["current"] = true;
    t["records"].add<JsonObject>()["summary"] = true;
    JsonObject tm = t["team"].to<JsonObject>();
    tm["id"] = true; tm["abbreviation"] = true; tm["location"] = true;
    tm["name"] = true; tm["shortDisplayName"] = true; tm["color"] = true;
}

void read_team(JsonObjectConst c, ScoreLeague lg, ScoreState st, const char *possId, ScoreTeam &t) {
    memset(&t, 0, sizeof t);
    JsonObjectConst tm = c["team"];
    t.id = (int32_t)atol(tm["id"] | "0");
    copy_str(t.abbr, sizeof t.abbr, tm["abbreviation"] | "");
    // College teams go by their school ("Arkansas"), NFL teams by their nickname ("Cowboys").
    const char *location = tm["location"] | "";
    const char *nick     = tm["name"] | (tm["shortDisplayName"] | "");
    if (lg == LEAGUE_CFB) { copy_str(t.main, sizeof t.main, location); copy_str(t.sub, sizeof t.sub, nick); }
    else                  { copy_str(t.main, sizeof t.main, nick);     copy_str(t.sub, sizeof t.sub, location); }
    if (!t.main[0]) copy_str(t.main, sizeof t.main, t.abbr);
    copy_str(t.record, sizeof t.record, c["records"][0]["summary"] | "");
    t.color = parse_hex_color(tm["color"] | "");
    const int r = c["curatedRank"]["current"] | 0;
    t.rank = (r >= 1 && r <= 25) ? (uint8_t)r : 0;
    t.score = (st == GAME_PRE) ? -1 : (int16_t)atoi(c["score"] | "0");
    t.winner = (st == GAME_FINAL) && (c["winner"] | false);
    t.possession = (st == GAME_LIVE) && possId && *possId && strcmp(possId, tm["id"] | "") == 0;
}

// One league's reply into the working set. Returns false if the reply cannot be read.
// `found` gets a mark for each of Kevin's teams seen in it.
bool parse_league(const uint8_t *body, size_t len, ScoreLeague lg, bool found[NFAV], int &events) {
    events = 0;
    JsonDocument filter;
    build_filter(filter);
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, (const char *)body, len,
                                                     DeserializationOption::Filter(filter),
                                                     DeserializationOption::NestingLimit(40));
    if (err) {
        Serial.printf("[scores] %s: can't read ESPN's reply: %s\n", lg == LEAGUE_NFL ? "NFL" : "SEC", err.c_str());
        return false;
    }
    JsonArrayConst evs = doc["events"];
    if (evs.isNull()) return false;
    for (JsonObjectConst ev : evs) {
        ++events;
        JsonObjectConst comp = ev["competitions"][0];
        JsonArrayConst cs = comp["competitors"];
        if (cs.size() < 2) continue;
        const char *state = ev["status"]["type"]["state"] | "pre";
        const ScoreState st = !strcmp(state, "in") ? GAME_LIVE : !strcmp(state, "post") ? GAME_FINAL : GAME_PRE;
        // Possession arrives as a team id, as text.
        const char *possId = comp["situation"]["possession"] | "";
        char possBuf[12] = "";
        if (comp["situation"]["possession"].is<long>()) {
            snprintf(possBuf, sizeof possBuf, "%ld", comp["situation"]["possession"].as<long>());
            possId = possBuf;
        }
        ScoreGame g;
        memset(&g, 0, sizeof g);
        g.league = lg;
        g.state = st;
        g.start = parse_iso_utc(ev["date"] | "");
        g.timeValid = comp["timeValid"] | true;
        copy_str(g.detail, sizeof g.detail, ev["status"]["type"]["shortDetail"] | "");
        copy_str(g.network, sizeof g.network, comp["broadcasts"][0]["names"][0] | "");
        if (st == GAME_LIVE) copy_str(g.down, sizeof g.down, comp["situation"]["downDistanceText"] | "");
        for (JsonObjectConst c : cs) {
            const bool home = !strcmp(c["homeAway"] | "", "home");
            read_team(c, lg, st, possId, home ? g.home : g.away);
        }
        const int fa = fav_index(lg, g.away.id), fh = fav_index(lg, g.home.id);
        if (fa >= 0) found[fa] = true;
        if (fh >= 0) found[fh] = true;
        g.fav = (int8_t)(fa < 0 ? fh : fh < 0 ? fa : std::min(fa, fh));
        // The NFL board is the whole league; only Kevin's two teams are kept from it.
        if (lg == LEAGUE_NFL && g.fav < 0) continue;
        if (s_workCount < SCORES_MAX_GAMES) s_work[s_workCount++] = g;
    }
    return true;
}

// Kevin's teams first, in his order; then everyone else live, coming up, then finished;
// teams with the week off last.
bool game_before(const ScoreGame &a, const ScoreGame &b) {
    auto bucket = [](const ScoreGame &g) -> int {
        if (g.state == GAME_BYE) return 10;
        if (g.fav >= 0) return g.fav;           // 0..3
        return 4 + (g.state == GAME_LIVE ? 0 : g.state == GAME_PRE ? 1 : 2);
    };
    const int ba = bucket(a), bb = bucket(b);
    if (ba != bb) return ba < bb;
    return a.start < b.start;
}

bool fetch_body(const char *url, const char *simEnv, uint8_t **body, size_t *len) {
#ifndef ARDUINO
    if (const char *path = getenv(simEnv)) {
        FILE *f = fopen(path, "rb");
        if (!f) { Serial.printf("[scores] can't open %s\n", path); return false; }
        fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n <= 0 || (size_t)n > BODY_MAX) { fclose(f); return false; }
        *body = (uint8_t *)malloc((size_t)n);
        *len = fread(*body, 1, (size_t)n, f);
        fclose(f);
        return *len > 0;
    }
#else
    (void)simEnv;
#endif
    return net_fetch_psram(url, ESPN_USER_AGENT, body, len, BODY_MAX, 5000, 15000);
}

void store(ScoresStatus st, bool replaceGames) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_status = st;
    if (replaceGames) {
        memcpy(s_store, s_work, sizeof(ScoreGame) * (size_t)s_workCount);
        s_count = s_workCount;
        s_fetchedMs = millis();
        if (!s_fetchedMs) s_fetchedMs = 1;
    }
}

}  // namespace

void scores_meta(int &count, uint32_t &fetchedMs, ScoresStatus &status, bool &anyLive) {
    std::lock_guard<std::mutex> lock(s_mutex);
    count = s_store ? s_count : 0;
    fetchedMs = s_fetchedMs;
    status = s_status;
    anyLive = false;
    for (int i = 0; i < count; ++i) if (s_store[i].state == GAME_LIVE) { anyLive = true; break; }
}

uint32_t scores_now_ms() { return millis(); }

bool scores_game(int i, ScoreGame &out) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_store || i < 0 || i >= s_count) return false;
    out = s_store[i];
    return true;
}

bool scores_fetch_now() {
    if (!s_store) {
        ScoreGame *a = (ScoreGame *)heap_caps_malloc(sizeof(ScoreGame) * SCORES_MAX_GAMES, MALLOC_CAP_SPIRAM);
        ScoreGame *b = (ScoreGame *)heap_caps_malloc(sizeof(ScoreGame) * SCORES_MAX_GAMES, MALLOC_CAP_SPIRAM);
        if (!a || !b) { if (a) heap_caps_free(a); if (b) heap_caps_free(b); return false; }
        s_work = b;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_store = a;
    }
#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) { store(SCORES_NO_WIFI, false); return true; }
#endif
    s_workCount = 0;
    bool found[NFAV] = {};
    bool ok[2] = {};
    int events[2] = {};
    const struct { ScoreLeague lg; const char *url; const char *env; } legs[2] = {
        { LEAGUE_CFB, URL_CFB, "SIM_SCORES_CFB" },
        { LEAGUE_NFL, URL_NFL, "SIM_SCORES_NFL" },
    };
    bool fetched = true;
    for (const auto &leg : legs) {
        uint8_t *body = nullptr; size_t len = 0;
        if (!fetch_body(leg.url, leg.env, &body, &len)) { fetched = false; continue; }
        ok[leg.lg] = parse_league(body, len, leg.lg, found, events[leg.lg]);
        heap_caps_free(body);   // given back as soon as it has been read
    }
    if (!ok[LEAGUE_NFL] || !ok[LEAGUE_CFB]) {
        // Keep showing the last good set; say what went wrong.
        store(fetched ? SCORES_UNREADABLE : SCORES_FAILED, false);
        return true;
    }
    // A team of Kevin's with no game on a board that has games this week is on a bye.
    for (int i = 0; i < NFAV; ++i) {
        if (found[i] || events[FAVS[i].league] == 0 || s_workCount >= SCORES_MAX_GAMES) continue;
        ScoreGame &g = s_work[s_workCount++];
        memset(&g, 0, sizeof g);
        g.league = FAVS[i].league;
        g.state = GAME_BYE;
        g.fav = (int8_t)i;
        g.home.id = FAVS[i].id;
        g.home.score = g.away.score = -1;
        copy_str(g.home.main, sizeof g.home.main, FAVS[i].main);
        copy_str(g.home.sub, sizeof g.home.sub, FAVS[i].sub);
    }
    std::stable_sort(s_work, s_work + s_workCount, game_before);
    store(SCORES_OK, true);
    Serial.printf("[scores] %d games (SEC board %d, NFL board %d)\n", s_workCount, events[LEAGUE_CFB], events[LEAGUE_NFL]);
    return true;
}
