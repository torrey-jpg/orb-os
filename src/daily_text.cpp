// Kevin's Orb — see daily_text.h.
#include "daily_text.h"
#include "theme_sd.h"
#include <string.h>

namespace {
bool is_leap(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }
const size_t FILE_MAX = 128 * 1024;
}

int daily_text::indexFor(const struct tm &t) {
    int i = t.tm_yday;
    if (!is_leap(t.tm_year + 1900) && t.tm_yday >= 59) ++i;
    return i < 0 ? 0 : (i >= DAYS ? DAYS - 1 : i);
}

bool daily_text::lineFor(const char *path, int index, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    size_t len = 0;
    uint8_t *buf = theme_sd::read_whole(path, len, FILE_MAX);
    if (!buf) return false;
    const char *p = (const char *)buf, *end = p + len;
    int n = 0;
    bool found = false;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *stop = nl ? nl : end;
        const char *e = stop;
        if (e > p && e[-1] == '\r') --e;
        if (e > p && *p != '#') {
            if (n == index) {
                size_t k = (size_t)(e - p);
                if (k >= cap) k = cap - 1;
                memcpy(out, p, k);
                out[k] = '\0';
                found = true;
                break;
            }
            ++n;
        }
        p = nl ? nl + 1 : end;
    }
    theme_sd::free(buf);
    return found;
}
