// Kevin's Orb — reading the pre-rendered teeth. See tooth_frames.h.
#include "tooth_frames.h"
#include "theme_sd.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include "sdcard.h"
#else
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
static struct { void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } } Serial;
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
static void heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif
#include <PNGdec.h>
#include <new>
#include <stdio.h>
#include <string.h>

namespace {

const char    *DIR             = "/teeth";
const int      MAX_FRAMES      = 48 * 48;
const uint32_t MAX_FRAME_BYTES = 256 * 1024;

uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// The line callback has no user pointer of ours, so the decode target lives here. Decodes
// only ever run on the LVGL thread, one at a time.
uint16_t *g_dst = nullptr;
int       g_w = 0;
bool      g_flip = false;
ToothFrames::Tint g_tint = ToothFrames::NATURAL;

inline uint16_t rgb565(int r, int g, int b) {
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

int line_cb(PNGDRAW *d) {
    const uint8_t *src = d->pPixels;
    const int bpp = (d->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA) ? 4 : 3;
    uint16_t *row = g_dst + (size_t)d->y * g_w;
    // Every fourth row darker in the hologram: the scan lines are part of the picture, so
    // they turn with it instead of sliding across it.
    const bool scanRow = (g_tint == ToothFrames::HOLOGRAM) && (d->y % 4 == 0);
    for (int x = 0; x < d->iWidth; ++x, src += bpp) {
        uint16_t v;
        if (g_tint == ToothFrames::HOLOGRAM) {
            int l = (src[0] * 3 + src[1] * 6 + src[2]) / 10;   // brightness
            if (scanRow) l = l * 2 / 5;
            // sky-blue glow. The blue lift fades out toward black (it used to be a flat +20,
            // which painted the empty background navy: a visible box on the AMOLED).
            v = rgb565(l * 48 / 100, l * 88 / 100, l * 118 / 100 + (l < 20 ? l : 20));
        } else {
            v = rgb565(src[0], src[1], src[2]);
        }
        row[g_flip ? (d->iWidth - 1 - x) : x] = v;
    }
    return 1;
}

}  // namespace

bool ToothFrames::readAt(uint32_t off, uint8_t *dst, uint32_t len) {
    if (!m_file) return false;
#ifdef ARDUINO
    File *f = (File *)m_file;
    theme_sd::lock();
    const bool ok = f->seek(off) && f->read(dst, len) == len;
    theme_sd::unlock();
    return ok;
#else
    FILE *f = (FILE *)m_file;
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(dst, 1, len, f) == len;
#endif
}

void ToothFrames::close() {
    if (m_file) {
#ifdef ARDUINO
        File *f = (File *)m_file;
        theme_sd::lock(); f->close(); theme_sd::unlock();
        delete f;
#else
        fclose((FILE *)m_file);
#endif
        m_file = nullptr;
    }
    if (m_table) { heap_caps_free(m_table); m_table = nullptr; }
    if (m_comp)  { heap_caps_free(m_comp);  m_comp = nullptr; m_compCap = 0; }
    if (m_png)   { ((PNG *)m_png)->~PNG(); heap_caps_free(m_png); m_png = nullptr; }
    m_open = false; m_type = -1; m_nyaw = m_npitch = 0;
}

bool ToothFrames::open(int type) {
    if (m_open && m_type == type) return true;
    close();
    char path[40];
    snprintf(path, sizeof path, "%s/t%02d.bin", DIR, type);
#ifdef ARDUINO
    if (!sdcard::mounted()) { Serial.printf("[tooth] no SD card\n"); return false; }
    theme_sd::lock();
    File f = SD.open(path, "r");
    theme_sd::unlock();
    if (!f) { Serial.printf("[tooth] %s: not found\n", path); return false; }
    m_file = new File(f);
#else
    FILE *f = fopen((std::string("sim/sdcard") + path).c_str(), "rb");
    if (!f) { Serial.printf("[tooth] %s: not found\n", path); return false; }
    m_file = f;
#endif
    uint8_t head[12];
    bool ok = readAt(0, head, sizeof head) && memcmp(head, "TTH2", 4) == 0;
    const int ny = ok ? rd16(head + 4) : 0, np = ok ? rd16(head + 6) : 0;
    const int frames = ny * np;
    ok = ok && ny > 0 && np > 0 && frames <= MAX_FRAMES;
    if (ok) {
        m_table = (uint32_t *)heap_caps_malloc((size_t)frames * 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ok = m_table && readAt(12, (uint8_t *)m_table, (uint32_t)frames * 8);
    }
    uint32_t biggest = 0;
    for (int i = 0; ok && i < frames; ++i) {
        const uint32_t off = rd32((const uint8_t *)&m_table[i * 2]);
        const uint32_t len = rd32((const uint8_t *)&m_table[i * 2 + 1]);
        m_table[i * 2] = off; m_table[i * 2 + 1] = len;
        ok = len > 0 && len <= MAX_FRAME_BYTES;
        if (len > biggest) biggest = len;
    }
    if (ok) {
        m_comp = (uint8_t *)heap_caps_malloc(biggest, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        m_compCap = biggest;
        ok = m_comp != nullptr;
    }
    if (!ok) { Serial.printf("[tooth] %s: bad file\n", path); close(); return false; }
    m_open = true; m_type = type; m_nyaw = ny; m_npitch = np;
    m_w = rd16(head + 8); m_h = rd16(head + 10);
    Serial.printf("[tooth] %s: %d x %d angles, %dx%d\n", path, ny, np, m_w, m_h);
    return true;
}

int ToothFrames::frameFor(int yaw, int pitch, bool flip) const {
    if (!m_open) return 0;
    const int y = ((yaw % m_nyaw) + m_nyaw) % m_nyaw;
    const int p = ((pitch % m_npitch) + m_npitch) % m_npitch;
    return p * m_nyaw + (flip ? (m_nyaw - y) % m_nyaw : y);
}

bool ToothFrames::decode(int frame, bool flip, uint16_t *dst, Tint tint) {
    if (!m_open || !dst || frame < 0 || frame >= m_nyaw * m_npitch) return false;
    if (!m_png) {
        void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!mem) return false;
        m_png = new (mem) PNG();
    }
    PNG *png = (PNG *)m_png;
    const uint32_t off = m_table[frame * 2], len = m_table[frame * 2 + 1];
    if (len > m_compCap || !readAt(off, m_comp, len)) return false;
    if (png->openRAM(m_comp, (int)len, line_cb) != PNG_SUCCESS) return false;
    if (png->getWidth() != m_w || png->getHeight() != m_h) { png->close(); return false; }
    g_dst = dst; g_w = m_w; g_flip = flip; g_tint = tint;
    const int r = png->decode(nullptr, 0);
    png->close();
    return r == PNG_SUCCESS;
}
