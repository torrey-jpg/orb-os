#pragma once
// HTTPS GET into a PSRAM buffer. When the server sends Content-Length the body is
// streamed straight into PSRAM, so large binaries (weather tiles, satellite JPEGs)
// never sit on the scarce internal heap — the contiguous block mbedTLS needs for the
// live-feed TLS handshake lives there, and a 100+ KB String can starve it.
// Chunked responses fall back to getString() (small payloads only reach that path).
// On success the caller owns *out and must release it with heap_caps_free().
#include <stddef.h>
#include <stdint.h>

bool net_fetch_psram(const char *url, const char *userAgent,
                     uint8_t **out, size_t *outLen, size_t maxLen,
                     int connectTimeoutMs, int totalTimeoutMs);

// Kevin's Orb: HTTPS POST of a body in PSRAM, with the reply streamed into PSRAM the same way
// (Content-Length or chunked). `headers` are full "Name: value" lines. *httpCode gets the
// status; a reply is returned for any status (OpenAI explains its errors in the body), and the
// function returns true only for 200. On return *out (if set) is the caller's to
// heap_caps_free().
bool net_post_psram(const char *url, const char *const *headers, int nHeaders,
                    const uint8_t *body, size_t bodyLen,
                    uint8_t **out, size_t *outLen, size_t maxLen,
                    int connectTimeoutMs, int totalTimeoutMs, int *httpCode);

