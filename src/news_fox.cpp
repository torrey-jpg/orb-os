// Kevin's Orb — Fox News headlines. See news_fox.h.
#include "news_fox.h"
#include "net_fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#else
#include <chrono>
#include <cstdarg>
static struct { void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } } Serial;
static uint32_t millis() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
static void heap_caps_free(void *p) { free(p); }
#endif

namespace {

const char  *FEED_URL   = "https://moxie.foxnews.com/google-publisher/latest.xml";
const char  *USER_AGENT = "KevinOrb/1.0";
const size_t FEED_MAX   = 768 * 1024;   // measured 173 KB; room for a much bigger day
const int    HEADLINE_MAX = 70;         // what the News screen is laid out for (as the gateway cut)

// The whole feed, fetched (or, in the simulator, read from SIM_FOX_FILE). Caller frees.
bool get_feed(uint8_t **buf, size_t *len) {
#ifndef ARDUINO
    if (const char *path = getenv("SIM_FOX_FILE")) {
        FILE *f = fopen(path, "rb");
        if (!f) { Serial.printf("[fox] can't open %s\n", path); return false; }
        *buf = (uint8_t *)malloc(FEED_MAX);
        *len = fread(*buf, 1, FEED_MAX, f);
        fclose(f);
        return *len > 0;
    }
#endif
    return net_fetch_psram(FEED_URL, USER_AGENT, buf, len, FEED_MAX, 5000, 15000);
}

const char *find(const char *from, const char *to, const char *needle) {
    const size_t n = strlen(needle);
    for (const char *p = from; p + n <= to; ++p)
        if (*p == *needle && !memcmp(p, needle, n)) return p;
    return nullptr;
}

// The text between <tag> and </tag> inside [from, to); empty if absent.
bool tag_text(const char *from, const char *to, const char *tag, const char **s, const char **e) {
    char open[32], close[32];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char *a = find(from, to, open);
    if (!a) return false;
    a += strlen(open);
    const char *b = find(a, to, close);
    if (!b) return false;
    // A CDATA wrapper, if Fox ever adds one.
    if (b - a >= 12 && !memcmp(a, "<![CDATA[", 9) && !memcmp(b - 3, "]]>", 3)) { a += 9; b -= 3; }
    *s = a; *e = b;
    return true;
}

// Entities and typographic punctuation to plain ASCII: the screen's fonts are ASCII plus a
// few symbols, and a curly quote would draw as a box. Anything else outside ASCII is dropped.
void decode(const char *s, const char *e, char *out, size_t cap) {
    size_t o = 0;
    auto put = [&](const char *t) { while (*t && o + 1 < cap) out[o++] = *t++; };
    while (s < e && o + 1 < cap) {
        if (*s == '&') {
            const char *semi = (const char *)memchr(s, ';', (size_t)(e - s));
            if (semi && semi - s <= 10) {
                char ent[12]; const size_t n = (size_t)(semi - s - 1);
                memcpy(ent, s + 1, n); ent[n] = 0;
                long cp = -1;
                if (!strcmp(ent, "amp")) put("&");
                else if (!strcmp(ent, "apos")) put("'");
                else if (!strcmp(ent, "quot")) put("\"");
                else if (!strcmp(ent, "lt")) put("<");
                else if (!strcmp(ent, "gt")) put(">");
                else if (!strcmp(ent, "nbsp")) put(" ");
                else if (ent[0] == '#') cp = (ent[1] == 'x' || ent[1] == 'X') ? strtol(ent + 2, nullptr, 16) : strtol(ent + 1, nullptr, 10);
                if (cp >= 0) {
                    if (cp >= 32 && cp < 127) { char c[2] = { (char)cp, 0 }; put(c); }
                    else if (cp == 0x2018 || cp == 0x2019) put("'");
                    else if (cp == 0x201C || cp == 0x201D) put("\"");
                    else if (cp == 0x2013 || cp == 0x2014) put("-");
                    else if (cp == 0x2026) put("...");
                    else if (cp == 0xA0) put(" ");
                }
                s = semi + 1;
                continue;
            }
        }
        const unsigned char c = (unsigned char)*s;
        if (c < 0x80) { if (c >= 32 || c == ' ') out[o++] = (char)c; else if (c == '\n' || c == '\t') out[o++] = ' '; ++s; continue; }
        // UTF-8: the same punctuation, raw.
        if (e - s >= 3 && c == 0xE2 && (unsigned char)s[1] == 0x80) {
            const unsigned char d = (unsigned char)s[2];
            if (d == 0x98 || d == 0x99) put("'");
            else if (d == 0x9C || d == 0x9D) put("\"");
            else if (d == 0x93 || d == 0x94) put("-");
            else if (d == 0xA6) put("...");
            s += 3;
            continue;
        }
        ++s;                                   // skip the rest of any other character
        while (s < e && ((unsigned char)*s & 0xC0) == 0x80) ++s;
    }
    out[o] = 0;
    // Trim.
    while (o && out[o - 1] == ' ') out[--o] = 0;
    size_t lead = 0;
    while (out[lead] == ' ') ++lead;
    if (lead) memmove(out, out + lead, o - lead + 1);
}

// Fox encodes some text twice ("&amp;quot;" for a quote mark), so decode until it stops
// changing; twice is all any feed has needed.
void decode_text(const char *s, const char *e, char *out, size_t cap) {
    decode(s, e, out, cap);
    if (strchr(out, '&')) {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s", out);
        decode(tmp, tmp + strlen(tmp), out, cap);
    }
}

// Cut to `max` characters on a word boundary, with "..." when anything was cut.
void cut(char *s, int max) {
    if ((int)strlen(s) <= max) return;
    int at = max - 3;
    while (at > max / 2 && s[at] != ' ') --at;
    if (s[at] != ' ') at = max - 3;
    while (at > 0 && (s[at - 1] == ' ' || s[at - 1] == ',' || s[at - 1] == ':' || s[at - 1] == ';')) --at;
    strcpy(s + at, "...");
}

// The story's handle: 8 hex characters hashed from its full headline (FNV-1a), the same idea
// as the gateway's key, so a story keeps its key between fetches.
void key_of(const char *title, char out[INTEL_KEY_BYTES]) {
    uint32_t h = 2166136261u;
    for (const char *p = title; *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
    snprintf(out, INTEL_KEY_BYTES, "%08lx", (unsigned long)h);
}

bool is_promo(const char *from, const char *to) {
    const char *ls, *le;
    if (tag_text(from, to, "link", &ls, &le) && find(ls, le, "/outkick-betting/")) return true;
    return false;
}

// Walk the stories. `each` returns false to stop.
template <typename F>
void each_story(const uint8_t *buf, size_t len, F each) {
    const char *p = (const char *)buf, *end = p + len;
    while (p < end) {
        const char *a = find(p, end, "<item>");
        if (!a) break;
        const char *b = find(a, end, "</item>");
        if (!b) break;
        p = b + 7;
        if (is_promo(a, b)) continue;
        if (!each(a, b)) break;
    }
}

}  // namespace

bool news_fox_fetch(int want, IntelSnapshot &out) {
    // The theme's default is 3. Fox's feed has about 18 stories at a time, so take 5: the
    // most the News screen's automatic layout is built to show at once (it shows every
    // headline it holds, two lines each; past 5 they run into the bottom of the dial).
    if (want < 5) want = 5;
    if (want > INTEL_MAX_ITEMS) want = INTEL_MAX_ITEMS;
    uint8_t *buf = nullptr; size_t len = 0;
    if (!get_feed(&buf, &len)) { Serial.printf("[fox] fetch failed\n"); return false; }
    // Static: over 2 KB, and only the network task ever runs this.
    static IntelSnapshot snap;
    memset(&snap, 0, sizeof snap);
    int skipped = 0;
    each_story(buf, len, [&](const char *a, const char *b) {
        const char *ts, *te;
        if (!tag_text(a, b, "title", &ts, &te)) { ++skipped; return true; }
        char full[256];
        decode_text(ts, te, full, sizeof full);
        if (!full[0]) return true;
        IntelItem &it = snap.items[snap.count];
        key_of(full, it.key);
        cut(full, HEADLINE_MAX);
        snprintf(it.text, sizeof it.text, "%s", full);
        snprintf(it.source, sizeof it.source, "Fox News");
        return ++snap.count < want;
    });
    heap_caps_free(buf);
    if (snap.count == 0) { Serial.printf("[fox] no headlines in the feed\n"); return false; }
    snap.valid = true;
    snap.fetchedMs = millis();
    out = snap;
    Serial.printf("[fox] %d headlines (%u bytes)\n", snap.count, (unsigned)len);
    return true;
}

IntelBriefState news_fox_brief(const char *key, char *headlineOut, size_t headlineCap,
                               char *sourceOut, size_t sourceCap, char *bodyOut, size_t bodyCap) {
    if (headlineOut && headlineCap) headlineOut[0] = 0;
    if (sourceOut && sourceCap)     snprintf(sourceOut, sourceCap, "Fox News");
    if (bodyOut && bodyCap)         bodyOut[0] = 0;
    if (!key || !*key) return INTEL_BRIEF_GONE;
    uint8_t *buf = nullptr; size_t len = 0;
    if (!get_feed(&buf, &len)) return INTEL_BRIEF_FAILED;
    IntelBriefState st = INTEL_BRIEF_GONE;
    each_story(buf, len, [&](const char *a, const char *b) {
        const char *ts, *te;
        if (!tag_text(a, b, "title", &ts, &te)) return true;
        char full[256], k[INTEL_KEY_BYTES];
        decode_text(ts, te, full, sizeof full);
        key_of(full, k);
        if (strcmp(k, key)) return true;
        // The briefing has room for more of the headline than the list does.
        cut(full, headlineCap ? (int)headlineCap - 1 : 0);
        if (headlineOut && headlineCap) snprintf(headlineOut, headlineCap, "%s", full);
        const char *ds, *de;
        if (bodyOut && bodyCap && tag_text(a, b, "description", &ds, &de)) {
            decode_text(ds, de, bodyOut, bodyCap);
        }
        st = (bodyOut && bodyOut[0]) ? INTEL_BRIEF_READY : INTEL_BRIEF_EMPTY;
        return false;
    });
    heap_caps_free(buf);
    return st;
}
