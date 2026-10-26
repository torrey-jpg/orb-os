// Kevin's Orb — today's weather. See kwx.h.
#include "kwx.h"
#include "net_fetch.h"
#include "config.h"
#include <ArduinoJson.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static void heap_caps_free(void *p) { free(p); }
#endif

namespace {

// Plain HTTP on the device, as in Zion's weather_client.cpp: no TLS handshake to find
// internal memory for, no credentials sent. The simulator asks over HTTPS.
#ifdef ARDUINO
#define KWX_SCHEME "http"
#else
#define KWX_SCHEME "https"
#endif
const size_t BODY_MAX = 64 * 1024;   // the reply is about 3 KB

std::mutex  s_mutex;
KwxSnapshot s_snap = {};
KwxStatus   s_status = KWX_NEVER;

void store(const KwxSnapshot *snap, KwxStatus st) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (snap) s_snap = *snap;
    s_status = st;
}

// "2026-10-25T07:29" -> "07:29"
void hhmm(char *dst, size_t cap, const char *iso) {
    const char *t = iso ? strchr(iso, 'T') : nullptr;
    snprintf(dst, cap, "%.5s", t ? t + 1 : "");
}

int round_i(float f) { return (int)(f < 0 ? f - 0.5f : f + 0.5f); }

bool parse(const uint8_t *body, size_t len, KwxSnapshot &s) {
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, (const char *)body, len);
    if (err) { Serial.printf("[kwx] can't read the reply: %s\n", err.c_str()); return false; }
    JsonObjectConst cur = doc["current"], daily = doc["daily"], hourly = doc["hourly"];
    if (cur.isNull() || daily.isNull() || hourly.isNull()) {
        Serial.printf("[kwx] reply is missing current/daily/hourly\n");
        return false;
    }
    memset(&s, 0, sizeof s);
    s.tempF    = (int16_t)round_i(cur["temperature_2m"] | 0.0f);
    s.feelsF   = (int16_t)round_i(cur["apparent_temperature"] | (float)s.tempF);
    s.code     = (uint8_t)(cur["weather_code"] | 0);
    s.isDay    = (cur["is_day"] | 1) != 0;
    s.humidity = (uint8_t)(cur["relative_humidity_2m"] | 0);
    s.windMph  = (int16_t)round_i(cur["wind_speed_10m"] | 0.0f);
    s.windDeg  = (int16_t)(cur["wind_direction_10m"] | 0);
    s.hiF      = (int16_t)round_i(daily["temperature_2m_max"][0] | 0.0f);
    s.loF      = (int16_t)round_i(daily["temperature_2m_min"][0] | 0.0f);
    s.rainMax  = (uint8_t)(daily["precipitation_probability_max"][0] | 0);
    hhmm(s.sunrise, sizeof s.sunrise, daily["sunrise"][0] | "");
    hhmm(s.sunset,  sizeof s.sunset,  daily["sunset"][0] | "");
    hhmm(s.sunriseNext, sizeof s.sunriseNext, daily["sunrise"][1] | (daily["sunrise"][0] | ""));
    JsonArrayConst ht = hourly["time"];
    // From the current hour on. forecast_hours should start there already; the check keeps
    // a past hour off the strip if it ever does not. Both are local "YYYY-MM-DDTHH:MM", so
    // comparing the first 13 characters compares date and hour.
    const char *nowIso = cur["time"] | "";
    for (size_t i = 0; i < ht.size() && s.nHours < KWX_HOURS; ++i) {
        const char *t = ht[i] | "";
        if (strlen(nowIso) >= 13 && strncmp(t, nowIso, 13) < 0) continue;
        KwxHour &h = s.hours[s.nHours++];
        const char *T = strchr(t, 'T');
        h.hour = (int8_t)(T ? atoi(T + 1) : 0);
        h.tempF = (int16_t)round_i(hourly["temperature_2m"][i] | 0.0f);
        h.rain  = (uint8_t)(hourly["precipitation_probability"][i] | 0);
        h.code  = (uint8_t)(hourly["weather_code"][i] | 0);
        h.day   = (hourly["is_day"][i] | 1) != 0;
    }
    s.valid = true;
    return true;
}

}  // namespace

void kwx_get(KwxSnapshot &out, KwxStatus &status) {
    std::lock_guard<std::mutex> lock(s_mutex);
    out = s_snap;
    status = s_status;
}

uint32_t kwx_now_ms() { return millis(); }

bool kwx_fetch_now() {
#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) { store(nullptr, KWX_NO_WIFI); return true; }
#endif
    uint8_t *body = nullptr; size_t len = 0;
    bool got = false;
#ifndef ARDUINO
    if (const char *path = getenv("SIM_KWX_FILE")) {
        FILE *f = fopen(path, "rb");
        if (f) {
            body = (uint8_t *)malloc(BODY_MAX);
            len = fread(body, 1, BODY_MAX, f);
            fclose(f);
            got = len > 0;
        }
        if (!got) { free(body); body = nullptr; Serial.printf("[kwx] can't read %s\n", path); }
    } else
#endif
    {
        char url[640];
        snprintf(url, sizeof url,
                 KWX_SCHEME "://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                 "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,is_day,wind_speed_10m,wind_direction_10m"
                 "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
                 "&hourly=temperature_2m,precipitation_probability,weather_code,is_day"
                 "&forecast_days=2&forecast_hours=%d&temperature_unit=fahrenheit&wind_speed_unit=mph&timezone=auto",
                 (double)KEVIN_WX_LAT, (double)KEVIN_WX_LON, KWX_HOURS);
        got = net_fetch_psram(url, ORB_USER_AGENT, &body, &len, BODY_MAX, 4000, 10000);
    }
    if (!got) { store(nullptr, KWX_FAILED); return true; }
    KwxSnapshot s;
    const bool ok = parse(body, len, s);
    heap_caps_free(body);
    if (!ok) { store(nullptr, KWX_UNREADABLE); return true; }
    s.fetchedMs = millis();
    if (!s.fetchedMs) s.fetchedMs = 1;
    store(&s, KWX_OK);
    Serial.printf("[kwx] %dF %s, high %d low %d, rain %d%%, %d hours\n",
                  s.tempF, kwx_condition(s.code), s.hiF, s.loF, s.rainMax, s.nHours);
    return true;
}

// WMO weather interpretation codes, as Open-Meteo documents them.
const char *kwx_condition(uint8_t c) {
    switch (c) {
        case 0:  return "Clear";
        case 1:  return "Mostly clear";
        case 2:  return "Partly cloudy";
        case 3:  return "Cloudy";
        case 45: case 48: return "Fog";
        case 51: case 53: case 55: return "Drizzle";
        case 56: case 57: return "Freezing drizzle";
        case 61: return "Light rain";
        case 63: return "Rain";
        case 65: return "Heavy rain";
        case 66: case 67: return "Freezing rain";
        case 71: return "Light snow";
        case 73: return "Snow";
        case 75: return "Heavy snow";
        case 77: return "Snow grains";
        case 80: return "Light showers";
        case 81: return "Showers";
        case 82: return "Heavy showers";
        case 85: case 86: return "Snow showers";
        case 95: return "Thunderstorms";
        case 96: case 99: return "Storms with hail";
        default: return "";
    }
}

KwxIcon kwx_icon(uint8_t c, bool day) {
    if (c <= 1)  return day ? ICON_SUN : ICON_MOON;
    if (c == 2)  return day ? ICON_PART_DAY : ICON_PART_NIGHT;
    if (c == 3)  return ICON_CLOUD;
    if (c == 45 || c == 48) return ICON_FOG;
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) return ICON_SNOW;
    if (c >= 95) return ICON_STORM;
    if (c >= 51) return ICON_RAIN;
    return ICON_CLOUD;
}
