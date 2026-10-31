// Kevin's Orb — the voice Assistant's engine. See assistant.h.
#include "assistant.h"
#include "net_fetch.h"
#include "orb_time.h"
#include <ArduinoJson.h>
#include <atomic>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <vector>

#if __has_include("secrets.h")
#include "secrets.h"                 // OPENAI_API_KEY (gitignored)
#endif
#ifndef ASSIST_MODEL_STT
#define ASSIST_MODEL_STT  "gpt-transcribe"
#endif
#ifndef ASSIST_MODEL_CHAT
#define ASSIST_MODEL_CHAT "gpt-6-luna"
#endif
#ifndef ASSIST_MODEL_TTS
#define ASSIST_MODEL_TTS  "gpt-4o-mini-tts"
#endif
#ifndef ASSIST_VOICE
#define ASSIST_VOICE      "alloy"
#endif

#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "audio.h"
static void *ps_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void  ps_free(void *p) { heap_caps_free(p); }
#else
#include <SDL.h>
#include <chrono>
#include <cstdarg>
static struct { void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } } Serial;
static uint32_t millis() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
static void *ps_alloc(size_t n) { return malloc(n); }
static void  ps_free(void *p) { free(p); }
#endif

namespace {

const int    SR          = 16000;
const int    MAX_REC_S   = 15;                       // the longest question
const int    REC_CAP     = SR * MAX_REC_S;           // samples
const size_t TTS_MAX     = 1600u * 1024;            // ~34 s of 24 kHz speech (what is spoken is capped below)
const size_t SPEAK_MAX   = 450;                      // characters spoken (~30 s at ~15 chars/s, inside TTS_MAX); the screen shows it all
const int    HIST        = 3;                        // earlier exchanges kept for follow-ups

// ---- shared state ------------------------------------------------------------------------
std::atomic<AssistState> s_state{AS_IDLE};
std::atomic<int>  s_level{0};
std::atomic<bool> s_active{false};
std::atomic<bool> s_wantListen{false};
std::atomic<bool> s_stopRec{false};
std::atomic<bool> s_stopSpeak{false};
std::atomic<bool> s_tidy{false};          // the screen closed: the network task tidies up

std::mutex s_textLock;
struct Texts { char q[400]; char a[1200]; char err[160]; char rawA[1600]; };
Texts *s_t = nullptr;                      // PSRAM

// Working text for the network task, in PSRAM rather than on its stack: that task has 7 KB of
// stack, and an HTTPS handshake needs several of them, so nothing large may sit on the stack
// while a request is in flight.
struct Work { char q[400]; char shown[400]; char a[1600]; char head[1024]; char sys[900]; };
Work *s_w = nullptr;

struct Hist { char q[300]; char a[900]; };
Hist *s_hist = nullptr;                    // PSRAM, HIST entries, oldest first
int   s_histN = 0;

// Network task's own buffers.
int16_t *s_rec = nullptr;                  // 16 kHz mono, the question
std::atomic<int> s_recN{0};
int16_t *s_play = nullptr;                 // 16 kHz stereo, the answer
size_t   s_playBytes = 0;
uint32_t s_playStart = 0, s_playMs = 0;

// ---- the microphone's ear: when has somebody spoken, and stopped? -------------------------
struct Vad { uint32_t ms; float floor; bool speech; uint32_t speechMs, silentMs; } s_vad;

void vad_reset() { memset(&s_vad, 0, sizeof s_vad); s_vad.floor = 1e9f; }

// Feed a piece of recording. Returns true when the question looks finished.
bool vad_feed(const int16_t *p, int n) {
    if (n <= 0) return false;
    double sum = 0;
    for (int i = 0; i < n; ++i) sum += (double)p[i] * p[i];
    const float rms = (float)sqrt(sum / n);
    const uint32_t ms = (uint32_t)n * 1000 / SR;
    s_vad.ms += ms;
    if (s_vad.ms <= 300 && rms < s_vad.floor) s_vad.floor = rms;   // the room, before speaking
    const float floor = s_vad.floor < 1e8f ? s_vad.floor : rms;
    const float thr = floor * 3.0f > 250.0f ? floor * 3.0f : 250.0f;   // three times the room, at least 250
    int lvl = (int)(rms / 40.0f);
    s_level = lvl > 100 ? 100 : lvl;
    if (rms > thr) { s_vad.speech = true; s_vad.speechMs += ms; s_vad.silentMs = 0; }
    else if (s_vad.speech) s_vad.silentMs += ms;
    if (s_vad.speech && s_vad.silentMs >= 1300 && s_vad.speechMs >= 300) return true;  // a pause after speaking
    if (s_vad.ms >= (uint32_t)MAX_REC_S * 1000) return true;                           // long enough
    if (!s_vad.speech && s_vad.ms >= 7000) return true;                                // nobody spoke
    return false;
}

// ---- small helpers -------------------------------------------------------------------------
void copy_str(char *d, size_t cap, const char *s) {
    if (!cap) return;
    size_t i = 0;
    for (; s && s[i] && i + 1 < cap; ++i) d[i] = s[i];
    d[i] = 0;
}

// UTF-8 to what the screen's fonts have: ASCII, with typographic punctuation made plain.
void to_ascii(char *dst, size_t cap, const char *s) {
    size_t o = 0;
    auto put = [&](const char *t) { while (*t && o + 1 < cap) dst[o++] = *t++; };
    while (s && *s && o + 1 < cap) {
        const unsigned char c = (unsigned char)*s;
        if (c < 0x80) { dst[o++] = (c == '\n' || c == '\t' || c >= 32) ? (c == '\t' ? ' ' : (char)c) : ' '; ++s; continue; }
        if (c == 0xE2 && (unsigned char)s[1] == 0x80) {
            const unsigned char d = (unsigned char)s[2];
            if (d == 0x98 || d == 0x99) put("'");
            else if (d == 0x9C || d == 0x9D) put("\"");
            else if (d == 0x93 || d == 0x94) put("-");
            else if (d == 0xA6) put("...");
            else if (d == 0xA2) put("*");
            s += 3; continue;
        }
        if (c == 0xC2 && (unsigned char)s[1] == 0xB0) { put("\xC2\xB0"); s += 2; continue; }   // degree: the fonts have it
        if (c == 0xC2 && (unsigned char)s[1] == 0xA0) { put(" "); s += 2; continue; }
        ++s;
        while (*s && ((unsigned char)*s & 0xC0) == 0x80) ++s;
    }
    dst[o] = 0;
}

void set_error(const char *msg) {
    {
        std::lock_guard<std::mutex> l(s_textLock);
        if (s_t) copy_str(s_t->err, sizeof s_t->err, msg);
    }
    Serial.printf("[assist] %s\n", msg);
    s_state = AS_ERROR;
}

const char *api_key() {
#ifdef OPENAI_API_KEY
    return OPENAI_API_KEY;
#else
    return "";
#endif
}

bool ensure_buffers() {
    if (!s_t)    { s_t = (Texts *)ps_alloc(sizeof(Texts)); if (s_t) memset(s_t, 0, sizeof(Texts)); }
    if (!s_hist) { s_hist = (Hist *)ps_alloc(sizeof(Hist) * HIST); s_histN = 0; }
    if (!s_w)    s_w = (Work *)ps_alloc(sizeof(Work));
    if (!s_rec)  s_rec = (int16_t *)ps_alloc(sizeof(int16_t) * REC_CAP);
    return s_t && s_hist && s_w && s_rec;
}

// OpenAI's own words for what went wrong, in a sentence somebody can act on.
void api_error(int code, const uint8_t *body, size_t len, const char *what) {
    char msg[160];
    if (code == 0) { set_error("Can't reach OpenAI. Check the Wi-Fi."); return; }
    if (code == 401) { set_error("OpenAI didn't accept the key in secrets.h."); return; }
    JsonDocument d;
    const char *m = "", *c = "";
    if (body && len && !deserializeJson(d, (const char *)body, len)) {
        m = d["error"]["message"] | "";
        c = d["error"]["code"] | "";
    }
    if (!strcmp(c, "insufficient_quota") || code == 402) {
        set_error("Out of OpenAI credit. Add some at platform.openai.com.");
        return;
    }
    if (code == 429) { set_error("OpenAI is busy. Try again in a moment."); return; }
    char clean[100];
    to_ascii(clean, sizeof clean, m);
    snprintf(msg, sizeof msg, "OpenAI %s error %d. %s", what, code, clean);
    set_error(msg);
}

bool post(const char *url, const char *ctype, const uint8_t *body, size_t len,
          uint8_t **out, size_t *outLen, size_t maxLen, int timeoutMs, int *code) {
    char auth[256], ct[160];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", api_key());
    snprintf(ct, sizeof ct, "Content-Type: %s", ctype);
    const char *h[] = { auth, ct };
    return net_post_psram(url, h, 2, body, len, out, outLen, maxLen, 6000, timeoutMs, code);
}

// ---- the three calls -----------------------------------------------------------------------
#if !defined(ARDUINO)
bool mock() { return getenv("SIM_ASSIST_MOCK") != nullptr; }
#endif

// 1. The recording, as a WAV, to text.
bool transcribe(char *text, size_t cap) {
#if !defined(ARDUINO)
    if (mock()) { copy_str(text, cap, "What does a molar do?"); return true; }
#endif
    const int n = s_recN;
    const char *B = "----KevinOrbAssistantBoundary";
    char *head = s_w->head;
    const size_t HEAD = sizeof s_w->head;
    // The prompt gives the transcriber the names it would otherwise spell its own way
    // ("Dr. Holler"). Sent once; if a model ever refuses it, the request is repeated without.
    for (int attempt = 0; attempt < 2; ++attempt) {
        int h = 0;
        h += snprintf(head + h, HEAD - h, "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n", B, ASSIST_MODEL_STT);
        if (attempt == 0) {
            h += snprintf(head + h, HEAD - h, "--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nen\r\n", B);
            h += snprintf(head + h, HEAD - h, "--%s\r\nContent-Disposition: form-data; name=\"prompt\"\r\n\r\n"
                          "Dr. Kevin Hollar, dentist, Bateman Dental, Nacogdoches, Lufkin, Cayleigh, Razorbacks, Longhorns.\r\n", B);
        }
        h += snprintf(head + h, HEAD - h, "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"question.wav\"\r\n"
                      "Content-Type: audio/wav\r\n\r\n", B);
        char tail[64];
        const int t = snprintf(tail, sizeof tail, "\r\n--%s--\r\n", B);
        const size_t pcmBytes = (size_t)n * 2;
        const size_t total = (size_t)h + 44 + pcmBytes + (size_t)t;
        uint8_t *body = (uint8_t *)ps_alloc(total);
        if (!body) { set_error("Out of memory for the recording."); return false; }
        uint8_t *p = body;
        memcpy(p, head, h); p += h;
        // A 44-byte WAV header: 16 kHz, mono, 16-bit.
        auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) *p++ = (uint8_t)(v >> (8 * i)); };
        auto u16 = [&](uint16_t v) { *p++ = (uint8_t)v; *p++ = (uint8_t)(v >> 8); };
        memcpy(p, "RIFF", 4); p += 4; u32(36 + (uint32_t)pcmBytes); memcpy(p, "WAVEfmt ", 8); p += 8;
        u32(16); u16(1); u16(1); u32(SR); u32(SR * 2); u16(2); u16(16);
        memcpy(p, "data", 4); p += 4; u32((uint32_t)pcmBytes);
        memcpy(p, s_rec, pcmBytes); p += pcmBytes;
        memcpy(p, tail, t);
        char ctype[96];
        snprintf(ctype, sizeof ctype, "multipart/form-data; boundary=%s", B);
        uint8_t *out = nullptr; size_t outLen = 0; int code = 0;
        const bool ok = post("https://api.openai.com/v1/audio/transcriptions", ctype, body, total,
                             &out, &outLen, 64 * 1024, 30000, &code);
        ps_free(body);
        if (!ok && code == 400 && attempt == 0) { if (out) ps_free(out); continue; }
        if (!ok) { api_error(code, out, outLen, "listening"); if (out) ps_free(out); return false; }
        JsonDocument d;
        const bool parsed = !deserializeJson(d, (const char *)out, outLen);
        copy_str(text, cap, parsed ? (d["text"] | "") : "");
        ps_free(out);
        return true;
    }
    return false;
}

// 2. The answer, with the last few exchanges for follow-up questions.
bool answer(const char *question, char *out, size_t cap) {
#if !defined(ARDUINO)
    if (mock()) { copy_str(out, cap, "A molar grinds and chews food. They are the broad, flat teeth at the back of your mouth."); return true; }
#endif
    char *sys = s_w->sys, today[64] = "";
    struct tm t;
    if (orb_local_time(&t)) strftime(today, sizeof today, " Today is %A, %B %d, %Y.", &t);
    snprintf(sys, sizeof s_w->sys,
             "You are the voice assistant on a desk orb belonging to Dr. Kevin Hollar, a dentist at "
             "Bateman Dental in Nacogdoches, Texas, near Lufkin. Your answer will be read aloud and shown "
             "on a small round screen, so answer in plain, friendly spoken English: at most three short "
             "sentences, no lists, no markdown, no emojis, no links. If a question needs live information "
             "you do not have, such as today's news, scores or weather, say so briefly.%s", today);
    JsonDocument d;
    d["model"] = ASSIST_MODEL_CHAT;
    JsonArray m = d["messages"].to<JsonArray>();
    JsonObject s0 = m.add<JsonObject>(); s0["role"] = "system"; s0["content"] = sys;
    for (int i = 0; i < s_histN; ++i) {
        JsonObject u = m.add<JsonObject>(); u["role"] = "user"; u["content"] = s_hist[i].q;
        JsonObject a = m.add<JsonObject>(); a["role"] = "assistant"; a["content"] = s_hist[i].a;
    }
    JsonObject u = m.add<JsonObject>(); u["role"] = "user"; u["content"] = question;
    const size_t len = measureJson(d);
    char *body = (char *)ps_alloc(len + 1);
    if (!body) { set_error("Out of memory for the question."); return false; }
    serializeJson(d, body, len + 1);
    uint8_t *resp = nullptr; size_t respLen = 0; int code = 0;
    const bool ok = post("https://api.openai.com/v1/chat/completions", "application/json",
                         (const uint8_t *)body, len, &resp, &respLen, 128 * 1024, 45000, &code);
    ps_free(body);
    if (!ok) { api_error(code, resp, respLen, "answer"); if (resp) ps_free(resp); return false; }
    JsonDocument r;
    const bool parsed = !deserializeJson(r, (const char *)resp, respLen);
    copy_str(out, cap, parsed ? (r["choices"][0]["message"]["content"] | "") : "");
    ps_free(resp);
    if (!out[0]) { set_error("ChatGPT sent back an empty answer."); return false; }
    // Remember it for a follow-up.
    if (s_histN == HIST) { memmove(&s_hist[0], &s_hist[1], sizeof(Hist) * (HIST - 1)); --s_histN; }
    copy_str(s_hist[s_histN].q, sizeof s_hist[0].q, question);
    copy_str(s_hist[s_histN].a, sizeof s_hist[0].a, out);
    ++s_histN;
    return true;
}

// 3. The answer, spoken: 24 kHz mono from OpenAI, made 16 kHz stereo for the Orb's speaker.
bool speak(const char *text) {
    uint8_t *pcm = nullptr; size_t pcmLen = 0;
#if !defined(ARDUINO)
    if (mock()) {
        pcmLen = 24000 * 2;                                // a second of a soft tone
        pcm = (uint8_t *)malloc(pcmLen);
        for (int i = 0; i < 24000; ++i) ((int16_t *)pcm)[i] = (int16_t)(3000 * sin(2 * M_PI * 440 * i / 24000.0));
    } else
#endif
    {
        JsonDocument d;
        d["model"] = ASSIST_MODEL_TTS;
        d["voice"] = ASSIST_VOICE;
        // Long answers are spoken up to a sentence end near SPEAK_MAX; the screen shows it all.
        char *spoken = s_w->a;
        if (text != spoken) copy_str(spoken, sizeof s_w->a, text);
        if (strlen(spoken) > SPEAK_MAX) {
            size_t cut = SPEAK_MAX;
            while (cut > SPEAK_MAX / 2 && !(strchr(".!?", spoken[cut - 1]) && spoken[cut] == ' ')) --cut;
            if (cut <= SPEAK_MAX / 2) cut = SPEAK_MAX;
            spoken[cut] = 0;
        }
        d["input"] = spoken;
        d["response_format"] = "pcm";                     // raw 24 kHz, 16-bit, mono
        const size_t len = measureJson(d);
        char *body = (char *)ps_alloc(len + 1);
        if (!body) { set_error("Out of memory for the voice."); return false; }
        serializeJson(d, body, len + 1);
        int code = 0;
        const bool ok = post("https://api.openai.com/v1/audio/speech", "application/json",
                             (const uint8_t *)body, len, &pcm, &pcmLen, TTS_MAX, 60000, &code);
        ps_free(body);
        if (!ok) { api_error(code, pcm, pcmLen, "voice"); if (pcm) ps_free(pcm); return false; }
    }
    const size_t in = pcmLen / 2;
    const size_t outFrames = in * 2 / 3;                   // 24 kHz -> 16 kHz
    s_play = (int16_t *)ps_alloc(outFrames * 4 + 4);
    if (!s_play) { ps_free(pcm); set_error("Out of memory for the voice."); return false; }
    const int16_t *src = (const int16_t *)pcm;
    for (size_t j = 0; j < outFrames; ++j) {
        const size_t i = j * 3 / 2;                        // position 1.5 j
        const int a = src[i], b = (i + 1 < in) ? src[i + 1] : a;
        const int v = (j & 1) ? (a + b) / 2 : a;
        s_play[j * 2] = s_play[j * 2 + 1] = (int16_t)v;
    }
    ps_free(pcm);
    s_playBytes = outFrames * 4;
    s_playMs = (uint32_t)(outFrames * 1000 / SR);
    return true;
}

// ---- playing it ------------------------------------------------------------------------------
#if !defined(ARDUINO)
SDL_AudioDeviceID s_out = 0, s_in = 0;

void sim_capture_cb(void *, Uint8 *stream, int len) { assist_sim_capture((const int16_t *)stream, len / 2); }

bool sim_audio_init() {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    if (!s_out) {
        SDL_AudioSpec w = {}; w.freq = SR; w.format = AUDIO_S16SYS; w.channels = 2; w.samples = 1024;
        s_out = SDL_OpenAudioDevice(nullptr, 0, &w, nullptr, 0);
        if (s_out) SDL_PauseAudioDevice(s_out, 0);
    }
    if (!s_in) {
        SDL_AudioSpec w = {}; w.freq = SR; w.format = AUDIO_S16SYS; w.channels = 1; w.samples = 512;
        w.callback = sim_capture_cb;
        s_in = SDL_OpenAudioDevice(nullptr, 1, &w, nullptr, 0);   // the Mac's microphone
        if (!s_in) Serial.printf("[assist] no microphone: %s\n", SDL_GetError());
    }
    return true;
}
#endif

void play_start() {
    s_playStart = millis();
#ifdef ARDUINO
    audio_play_pcm((const uint8_t *)s_play, s_playBytes, true, 100);
#else
    if (s_out) { SDL_ClearQueuedAudio(s_out); SDL_QueueAudio(s_out, s_play, (Uint32)s_playBytes); }
#endif
}

void play_stop() {
#ifdef ARDUINO
    if (s_play) audio_release_pcm((const uint8_t *)s_play);
#else
    if (s_out) SDL_ClearQueuedAudio(s_out);
#endif
}

void free_play() { play_stop(); if (s_play) { ps_free(s_play); s_play = nullptr; } s_playBytes = 0; }

// The screen closed: stop and give everything back. Network task only.
void tidy() {
    free_play();
#ifdef ARDUINO
    audio_mic_enable(false);
#else
    if (s_in) SDL_PauseAudioDevice(s_in, 1);
#endif
    if (s_rec) { ps_free(s_rec); s_rec = nullptr; }
    s_recN = 0;
    s_level = 0;
    Serial.printf("[assist] closed\n");
}

#if !defined(ARDUINO)
// SIM_ASK_WAV=file.wav: use a recorded question instead of the microphone (any rate, 16-bit
// mono or stereo; resampled to 16 kHz mono).
bool sim_load_wav(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fclose(f); return false; }
    int rate = 16000, ch = 1, bits = 16;
    std::vector<int16_t> pcm;
    for (;;) {
        uint8_t ck[8];
        if (fread(ck, 1, 8, f) != 8) break;
        uint32_t sz = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
        if (!memcmp(ck, "fmt ", 4)) {
            std::vector<uint8_t> b(sz); if (fread(b.data(), 1, sz, f) != sz) break;
            ch = b[2] | b[3] << 8; rate = b[4] | b[5] << 8 | b[6] << 16 | b[7] << 24; bits = b[14] | b[15] << 8;
        } else if (!memcmp(ck, "data", 4)) {
            if (sz == 0xFFFFFFFFu || sz == 0) sz = 64u << 20;          // streamed WAVs leave it unknown
            std::vector<uint8_t> b(sz); size_t got = fread(b.data(), 1, sz, f);
            pcm.assign((int16_t *)b.data(), (int16_t *)(b.data() + got - got % 2));
            break;
        } else fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    fclose(f);
    if (bits != 16 || pcm.empty()) return false;
    const size_t frames = pcm.size() / ch;
    int n = 0;
    for (double pos = 0; n < REC_CAP; pos += (double)rate / SR) {
        const size_t i = (size_t)pos;
        if (i >= frames) break;
        s_rec[n++] = pcm[i * ch];
    }
    s_recN = n;
    printf("[assist] question from %s: %.1f s\n", path, n / (double)SR);
    return true;
}
#endif

}  // namespace

// ---- public ---------------------------------------------------------------------------------
bool assist_has_key() {
    const char *k = api_key();
    return k[0] && strcmp(k, "paste-your-key-here") != 0;
}

AssistState assist_state() { return s_state; }
int assist_level() { return s_level; }

void assist_texts(char *q, size_t qCap, char *a, size_t aCap, char *err, size_t errCap) {
    std::lock_guard<std::mutex> l(s_textLock);
    copy_str(q, qCap, s_t ? s_t->q : "");
    copy_str(a, aCap, s_t ? s_t->a : "");
    copy_str(err, errCap, s_t ? s_t->err : "");
}

void assist_push() {
    switch ((AssistState)s_state) {
        case AS_IDLE: case AS_ANSWERED: case AS_ERROR: s_wantListen = true; break;
        case AS_LISTENING: s_stopRec = true; break;
        case AS_SPEAKING:  s_stopSpeak = true; break;
        default: break;
    }
}

void assist_open() {
    s_active = true;                                       // (a pending tidy still runs first; see assist_step)
    s_wantListen = s_stopRec = s_stopSpeak = false;
    s_histN = 0;                                           // a fresh conversation
    {
        std::lock_guard<std::mutex> l(s_textLock);
        if (s_t) { s_t->q[0] = s_t->a[0] = s_t->err[0] = 0; }
    }
    s_state = assist_has_key() ? AS_IDLE : AS_NOKEY;
}

void assist_close() {
    s_active = false;
    s_tidy = true;
    s_stopRec = true;
}

#if !defined(ARDUINO)
void assist_sim_capture(const int16_t *p, int n) {
    if (s_state != AS_LISTENING || !s_rec) return;
    int at = s_recN;
    if (at + n > REC_CAP) n = REC_CAP - at;
    if (n <= 0) { s_stopRec = true; return; }
    memcpy(s_rec + at, p, (size_t)n * 2);
    s_recN = at + n;
    if (vad_feed(p, n)) s_stopRec = true;
}
#endif

bool assist_step() {
    // A close always gets its tidy, even if the screen was opened again before this ran.
    if (s_tidy.exchange(false)) tidy();
    if (!s_active) return false;
    if (s_state == AS_NOKEY) return false;

    // Start listening.
    if (s_wantListen.exchange(false)) {
        free_play();
        if (!ensure_buffers()) { set_error("Out of memory for the Assistant."); return true; }
        s_recN = 0;
        s_stopRec = false;
        vad_reset();
#ifdef ARDUINO
        if (!audio_mic_enable(true)) { set_error("The microphone didn't start."); return true; }
        int16_t junk[256];
        audio_mic_read(junk, 256, 50);                    // the first few ms after a restart are clicks
#else
        sim_audio_init();
        if (const char *w = getenv("SIM_ASK_WAV")) {
            if (!sim_load_wav(w)) { set_error("Couldn't read SIM_ASK_WAV."); return true; }
            s_state = AS_HEARD;
            return true;
        }
        if (getenv("SIM_ASSIST_MOCK")) { s_recN = SR; s_state = AS_HEARD; return true; }
        if (!s_in) { set_error("No microphone on this computer."); return true; }
        SDL_PauseAudioDevice(s_in, 0);
#endif
        {
            std::lock_guard<std::mutex> l(s_textLock);
            s_t->q[0] = s_t->a[0] = s_t->err[0] = 0;
        }
        s_state = AS_LISTENING;
        return true;
    }

    switch ((AssistState)s_state) {
        case AS_LISTENING: {
#ifdef ARDUINO
            // Record here, on the network task, until a pause, a push, or the time limit.
            static int16_t chunk[480];                    // 30 ms
            while (s_active && !s_stopRec) {
                const int n = audio_mic_read(chunk, 480, 200);
                if (n <= 0) { set_error("The microphone stopped."); return true; }
                const int at = s_recN;
                const int take = at + n > REC_CAP ? REC_CAP - at : n;
                memcpy(s_rec + at, chunk, (size_t)take * 2);
                s_recN = at + take;
                if (vad_feed(chunk, n) || take < n) break;
            }
            if (!s_active) return false;
#else
            if (!s_stopRec) return false;
            SDL_PauseAudioDevice(s_in, 1);
#endif
            s_level = 0;
            if (!s_vad.speech || s_recN < SR / 3) {
                set_error("I didn't hear a question. Push and try again.");
                return true;
            }
            s_state = AS_HEARD;
            return true;
        }
        case AS_HEARD: {
            char *q = s_w->q, *shown = s_w->shown;
            if (!transcribe(q, sizeof s_w->q)) return true;
            to_ascii(shown, sizeof s_w->shown, q);
            if (!shown[0] || strspn(shown, " .") == strlen(shown)) {
                set_error("Sorry, I couldn't make that out. Push and try again.");
                return true;
            }
            {
                std::lock_guard<std::mutex> l(s_textLock);
                copy_str(s_t->q, sizeof s_t->q, shown);
                copy_str(s_t->rawA, sizeof s_t->rawA, q);     // the raw question, for the answer step
            }
            Serial.printf("[assist] heard: %s\n", shown);
            s_state = AS_THINKING;
            return true;
        }
        case AS_THINKING: {
            char *q = s_w->q, *a = s_w->a;
            {
                std::lock_guard<std::mutex> l(s_textLock);
                copy_str(q, sizeof s_w->q, s_t->rawA);
            }
            if (!answer(q, a, sizeof s_w->a)) return true;
            {
                std::lock_guard<std::mutex> l(s_textLock);
                copy_str(s_t->rawA, sizeof s_t->rawA, a);
                to_ascii(s_t->a, sizeof s_t->a, a);
            }
            Serial.printf("[assist] answer: %s\n", a);
            s_state = AS_VOICING;
            return true;
        }
        case AS_VOICING: {
            char *a = s_w->a;
            {
                std::lock_guard<std::mutex> l(s_textLock);
                copy_str(a, sizeof s_w->a, s_t->rawA);
            }
            free_play();
            if (!speak(a)) { s_state = AS_ERROR; return true; }
#if !defined(ARDUINO)
            sim_audio_init();
#endif
            s_stopSpeak = false;
            play_start();
            s_state = AS_SPEAKING;
            return true;
        }
        case AS_SPEAKING:
            if (s_stopSpeak.exchange(false) || millis() - s_playStart > s_playMs + 400) {
                play_stop();
                s_state = AS_ANSWERED;
                return true;
            }
            return false;
        default:
            return false;
    }
}
