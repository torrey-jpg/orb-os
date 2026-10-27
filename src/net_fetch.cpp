// Shared streamed-download helper (see net_fetch.h). Mirrors the proven pattern in
// photo_client.cpp: Content-Length -> stream into PSRAM; chunked -> getString decode.
#include "net_fetch.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <memory>

bool net_fetch_psram(const char *url, const char *userAgent,
                     uint8_t **out, size_t *outLen, size_t maxLen,
                     int connectTimeoutMs, int totalTimeoutMs) {
    *out = nullptr; *outLen = 0;
    if (WiFi.status() != WL_CONNECTED) return false;

    // The transport follows the URL, which it did not used to. This built a
    // WiFiClientSecure for every call, and a secure client handshakes on connect whatever
    // the scheme says, so an http:// URL still did TLS and still failed -32512 on a board
    // that cannot raise the two contiguous ~16 KB internal blocks a handshake needs.
    //
    // Every caller of this helper was therefore dead for the same reason and it looked
    // like several separate faults: the weather radar's tiles, the photo feed and the
    // cloud imagery were each written off as "that service forces HTTPS" when the force
    // was here. An https:// URL still gets a secure client and will still fail, which is
    // honest, and the log below says which one it was.
    //
    // ...and the secure client is only BUILT when the URL asks for one, which is the half of
    // that reasoning this originally missed. Declaring it here cost internal RAM on every
    // call, plain downloads included, which on this board is all of them: the constructor
    // does `new sslclient_context` and ssl_init() straight away, standing up an mbedTLS ssl
    // context, its config and a DRBG before anybody has asked to connect. A couple of KB,
    // taken and given back per fetch, out of the one heap the weather tiles and the cloud
    // imagery are already competing for, and whose largest free block has been measured in
    // single KB. Not what kills TLS, which is the 16 KB handshake buffers later, just a tax
    // nobody was getting anything for.
    //
    // Found by Techtobi83, in their fork, 2026-10.
    const bool secure = !strncmp(url, "https://", 8);
    WiFiClient plain;
    std::unique_ptr<WiFiClientSecure> tls(secure ? new WiFiClientSecure() : nullptr);
    if (tls) tls->setInsecure();              // hobby device (matches the other clients)
    WiFiClient &cli = tls ? static_cast<WiFiClient &>(*tls) : plain;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(connectTimeoutMs);
    http.setTimeout(totalTimeoutMs);
    if (!http.begin(cli, url)) return false;
    if (userAgent) http.setUserAgent(userAgent);

    const int code = http.GET();
    if (code != 200) {
        Serial.printf("[net] HTTP %d over %s: %s\n", code, secure ? "TLS" : "plain", url);
        http.end(); return false;
    }

    const int len = http.getSize();           // >0 = Content-Length; -1 = chunked/unknown
    uint8_t *buf = nullptr;
    size_t got = 0;

    if (len > 0) {
        // Known length: stream the body straight into a PSRAM buffer.
        const size_t cap = ((size_t)len <= maxLen) ? (size_t)len : maxLen;
        buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (!buf) { http.end(); return false; }
        WiFiClient *stream = http.getStreamPtr();
        uint32_t last = millis();
        while (got < cap && (millis() - last) < (uint32_t)totalTimeoutMs) {
            const size_t avail = stream->available();
            if (avail) {
                const size_t want = (cap - got < avail) ? (cap - got) : avail;
                const int r = stream->readBytes(buf + got, want);
                if (r > 0) { got += r; last = millis(); }
            } else if (!http.connected()) {
                break;
            } else {
                delay(5);
            }
        }
    } else {
        // Chunked/unknown: getString() performs the chunk decode; only small payloads
        // are expected here (image CDNs send Content-Length), so the String is cheap.
        String body = http.getString();
        got = body.length();
        if (got > maxLen) got = maxLen;
        if (got > 0) {
            buf = (uint8_t *)heap_caps_malloc(got, MALLOC_CAP_SPIRAM);
            if (buf) memcpy(buf, body.c_str(), got);
            else got = 0;
        }
    }
    http.end();
    if (got == 0) {
        // SAY SO. This failed in silence, and "the service is not answering" on the glass was
        // then the only clue, at the times when the service had answered perfectly well and
        // the Orb could not take the reply. A socket's buffers come out of internal heap, so
        // print what there was of it: the difference between a server problem and a memory
        // one is the first thing anybody diagnosing this needs and could not get.
        Serial.printf("[net] no body (len %d): internal heap %u free, largest block %u: %s\n",
                      len, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), url);
        if (buf) heap_caps_free(buf);
        return false;
    }
    *out = buf; *outLen = got;
    return true;
}

// ---- Kevin's Orb: POST (see net_fetch.h) -------------------------------------------------------
bool net_post_psram(const char *url, const char *const *headers, int nHeaders,
                    const uint8_t *body, size_t bodyLen,
                    uint8_t **out, size_t *outLen, size_t maxLen,
                    int connectTimeoutMs, int totalTimeoutMs, int *httpCode) {
    *out = nullptr; *outLen = 0;
    if (httpCode) *httpCode = 0;
    if (WiFi.status() != WL_CONNECTED) return false;
    const bool secure = !strncmp(url, "https://", 8);
    WiFiClient plain;
    std::unique_ptr<WiFiClientSecure> tls(secure ? new WiFiClientSecure() : nullptr);
    if (tls) tls->setInsecure();              // hobby device (matches the other clients)
    WiFiClient &cli = tls ? static_cast<WiFiClient &>(*tls) : plain;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(connectTimeoutMs);
    http.setTimeout(totalTimeoutMs);
    if (!http.begin(cli, url)) return false;
    for (int i = 0; i < nHeaders; ++i) {
        const char *colon = strchr(headers[i], ':');
        if (!colon) continue;
        String name(headers[i]); name.remove(colon - headers[i]);
        const char *v = colon + 1; while (*v == ' ') ++v;
        http.addHeader(name, v);
    }
    const char *keep[] = { "Transfer-Encoding" };
    http.collectHeaders(keep, 1);
    const int code = http.POST((uint8_t *)body, bodyLen);
    if (httpCode) *httpCode = code;
    if (code <= 0) {
        Serial.printf("[net] POST failed (%d) over %s: %s\n", code, secure ? "TLS" : "plain", url);
        http.end(); return false;
    }
    const bool chunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
    const int len = http.getSize();
    WiFiClient *stream = http.getStreamPtr();
    size_t cap = (len > 0 && (size_t)len < maxLen) ? (size_t)len : maxLen;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(cap ? cap : 1, MALLOC_CAP_SPIRAM);
    if (!buf) { http.end(); return false; }
    size_t got = 0;
    uint32_t last = millis();
    // Read exactly n bytes (or until the time runs out / the connection closes).
    auto readN = [&](size_t n) {
        while (n && got < cap && (millis() - last) < (uint32_t)totalTimeoutMs) {
            const size_t avail = stream->available();
            if (avail) {
                size_t want = n < avail ? n : avail;
                if (want > cap - got) want = cap - got;
                const int r = stream->readBytes(buf + got, want);
                if (r > 0) { got += r; n -= r; last = millis(); }
            } else if (!stream->connected()) {
                break;
            } else {
                delay(2);
            }
        }
        return n == 0;
    };
    if (chunked) {
        // HTTPClient leaves the chunk framing in the stream: hex size line, data, CRLF.
        for (;;) {
            String line = stream->readStringUntil('\n');
            line.trim();
            if (!line.length()) { if (!stream->connected() && !stream->available()) break; continue; }
            const size_t n = (size_t)strtoul(line.c_str(), nullptr, 16);
            if (n == 0) break;
            if (!readN(n)) break;
            stream->readStringUntil('\n');       // the CRLF after the data
            if (got >= cap) break;
        }
    } else if (len > 0) {
        readN(cap);
    } else {
        readN(cap);                               // no length: until the server closes
    }
    http.end();
    *out = buf; *outLen = got;
    if (code != 200) Serial.printf("[net] POST HTTP %d (%u bytes back): %s\n", code, (unsigned)got, url);
    return code == 200 && got > 0;
}

#else
// Desktop/native build: no PSRAM, no fragmented-heap concerns — a single libcurl GET
// straight into a malloc'd buffer covers it. The maxLen cap is preserved so a runaway
// response still aborts instead of growing unbounded.
#include <curl/curl.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace {
    struct NetBuf { uint8_t *data; size_t len, cap; };
    size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
        NetBuf *b = (NetBuf *)userdata;
        const size_t n = size * nmemb;
        if (b->len + n > b->cap) return 0;   // over budget -> libcurl aborts the transfer
        memcpy(b->data + b->len, ptr, n);
        b->len += n;
        return n;
    }
}

bool net_fetch_psram(const char *url, const char *userAgent,
                     uint8_t **out, size_t *outLen, size_t maxLen,
                     int connectTimeoutMs, int totalTimeoutMs) {
    *out = nullptr; *outLen = 0;
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    NetBuf buf = { (uint8_t *)malloc(maxLen), 0, maxLen };
    if (!buf.data) { curl_easy_cleanup(curl); return false; }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    if (userAgent) curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)connectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)totalTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    const CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || code != 200 || buf.len == 0) {
        fprintf(stderr, "[net] HTTP %ld (curl: %s) %s\n", code, curl_easy_strerror(res), url);
        free(buf.data);
        return false;
    }
    *out = buf.data; *outLen = buf.len;
    return true;
}

// Kevin's Orb: POST, the same contract as the device's (see net_fetch.h).
bool net_post_psram(const char *url, const char *const *headers, int nHeaders,
                    const uint8_t *body, size_t bodyLen,
                    uint8_t **out, size_t *outLen, size_t maxLen,
                    int connectTimeoutMs, int totalTimeoutMs, int *httpCode) {
    *out = nullptr; *outLen = 0;
    if (httpCode) *httpCode = 0;
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    NetBuf buf = { (uint8_t *)malloc(maxLen), 0, maxLen };
    if (!buf.data) { curl_easy_cleanup(curl); return false; }
    struct curl_slist *hl = nullptr;
    for (int i = 0; i < nHeaders; ++i) hl = curl_slist_append(hl, headers[i]);
    hl = curl_slist_append(hl, "Expect:");    // no 100-continue round trip
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (const char *)body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)bodyLen);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)connectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)totalTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    const CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    curl_slist_free_all(hl);
    if (httpCode) *httpCode = (int)code;
    if (res != CURLE_OK) {
        fprintf(stderr, "[net] POST failed (curl: %s) %s\n", curl_easy_strerror(res), url);
        free(buf.data);
        return false;
    }
    *out = buf.data; *outLen = buf.len;
    if (code != 200) fprintf(stderr, "[net] POST HTTP %ld (%u bytes back): %s\n", code, (unsigned)buf.len, url);
    return code == 200 && buf.len > 0;
}
#endif
