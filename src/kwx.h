#pragma once
// Kevin's Orb — today's weather for one town (KEVIN_WX_* in config.h): the data, the fetch,
// and the hand-off from the network task (core 0) to the screen (core 1).
//
// Source: Open-Meteo (api.open-meteo.com), free, no key, the same service Zion's own
// weather code uses, and over plain HTTP for the same reason: no TLS handshake to find
// memory for. Fahrenheit and mph, in the town's own time zone. A few KB per fetch.
//
// Named kwx so it never collides with Zion's weather.* / weather_client.*.
#include <stdint.h>
#include <stddef.h>

#define KWX_HOURS 24

struct KwxHour {
    int8_t  hour;      // 0..23, local
    int16_t tempF;
    uint8_t rain;      // chance of rain, %
    uint8_t code;      // WMO weather code
    bool    day;       // daylight (for sun vs moon)
};

struct KwxSnapshot {
    bool     valid;
    int16_t  tempF, feelsF;
    uint8_t  code;          // WMO weather code, now
    bool     isDay;
    uint8_t  humidity;      // %
    int16_t  windMph;
    int16_t  windDeg;       // where the wind comes FROM, degrees
    int16_t  hiF, loF;      // today
    uint8_t  rainMax;       // today's highest chance of rain, %
    char     sunrise[6];    // "07:29" (24 h, local)
    char     sunset[6];
    char     sunriseNext[6];   // tomorrow's, for after dark
    KwxHour  hours[KWX_HOURS];   // from this hour on
    int      nHours;
    uint32_t fetchedMs;
};

enum KwxStatus : uint8_t { KWX_NEVER = 0, KWX_OK, KWX_NO_WIFI, KWX_FAILED, KWX_UNREADABLE };

// Any thread: the last good snapshot (valid=false if none) and what the last try did.
void kwx_get(KwxSnapshot &out, KwxStatus &status);
uint32_t kwx_now_ms();

// Network task only. Fetches and stores; returns true when the store changed.
// The simulator can read a saved reply instead: SIM_KWX_FILE=/path/reply.json
bool kwx_fetch_now();

// Words and pictures for a WMO weather code.
const char *kwx_condition(uint8_t code);           // "Partly cloudy"
enum KwxIcon : uint8_t { ICON_SUN, ICON_MOON, ICON_PART_DAY, ICON_PART_NIGHT, ICON_CLOUD,
                         ICON_FOG, ICON_RAIN, ICON_STORM, ICON_SNOW, ICON_COUNT };
KwxIcon kwx_icon(uint8_t code, bool day);
