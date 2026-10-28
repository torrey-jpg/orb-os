// Kevin's Orb — screensaver picture scenes. See saver_scene.h.
#include "saver_scene.h"
#include "saver_glow_spots.h"
#include "saver_scenes.h"
#include "theme_sd.h"
#include <PNGdec.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#else
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
static void heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif

namespace {

const int    W = 466, H = 466;
const size_t PNG_MAX = 900 * 1024;           // the pictures are about 200 KB
const uint32_t DRIFT_STEP_MS = 4000;         // the picture moves one pixel this often (AMOLED care)
const int    DRIFT_PX = 5;

enum Kind { COSMIC, OCEAN, GLOW, AURORA, KIND_COUNT };
const char *NAMES[KIND_COUNT] = { "cosmic", "ocean", "glow", "aurora" };

// ---- sprites: white light with a soft alpha edge, tinted per use with img_recolor -------
struct Sprite { uint8_t *px = nullptr; lv_img_dsc_t dsc; int w = 0, h = 0; };
enum { SP_DOT, SP_STAR, SP_GLOW, SP_BIG, SP_WASH, SP_COUNT };
Sprite s_sp[SP_COUNT];

bool make_sprite(Sprite &s, int w, int h, int kind) {
    s.w = w; s.h = h;
    s.px = (uint8_t *)heap_caps_malloc((size_t)w * h * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.px) return false;
    const float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float dx = (x - cx) / (w * 0.5f), dy = (y - cy) / (h * 0.5f);
            const float r = sqrtf(dx * dx + dy * dy);
            float a;
            if (kind == SP_STAR) {
                // bright core, soft halo and a faint four-point cross
                a = expf(-r * r * 40.f) + 0.35f * expf(-r * r * 6.f)
                  + 0.45f * (expf(-fabsf(dy) * 28.f) * fmaxf(0.f, 1.f - fabsf(dx)) +
                             expf(-fabsf(dx) * 28.f) * fmaxf(0.f, 1.f - fabsf(dy)));
            } else if (kind == SP_DOT) {
                a = expf(-r * r * 9.f);
            } else {
                a = expf(-r * r * 4.5f) * fmaxf(0.f, 1.f - r);   // soft glow, zero at the edge
            }
            if (a > 1.f) a = 1.f;
            uint8_t *p = s.px + ((size_t)y * w + x) * 3;
            p[0] = 0xFF; p[1] = 0xFF;                 // white in RGB565 (byte order does not matter)
            p[2] = (uint8_t)(a * 255.f + 0.5f);
        }
    }
    memset(&s.dsc, 0, sizeof s.dsc);
    s.dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s.dsc.header.w = w;
    s.dsc.header.h = h;
    s.dsc.data_size = (uint32_t)w * h * 3;
    s.dsc.data = s.px;
    return true;
}

void free_sprites() {
    for (auto &s : s_sp) {
        if (s.px) { lv_img_cache_invalidate_src(&s.dsc); heap_caps_free(s.px); s.px = nullptr; }
    }
}

// ---- the picture ----------------------------------------------------------------------
uint16_t    *s_bgPix = nullptr;
lv_img_dsc_t s_bgDsc;

int bg_line(PNGDRAW *d) {
    if (d->y >= H) return 1;
    const uint8_t *src = d->pPixels;
    uint16_t *dst = s_bgPix + (size_t)d->y * W;
    const int step = d->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA ? 4 : 3;
    const int n = d->iWidth < W ? d->iWidth : W;
    for (int x = 0; x < n; ++x, src += step)
        dst[x] = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3));
    return 1;
}

bool load_picture(const char *name) {
    char path[48];
    snprintf(path, sizeof path, "/saver/%s.png", name);
    size_t len = 0;
    uint8_t *file = theme_sd::read_whole(path, len, PNG_MAX);
    if (!file) { printf("[saver] no %s\n", path); return false; }
    bool ok = false;
    void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem) {
        PNG *png = new (mem) PNG();
        if (png->openRAM(file, (int)len, bg_line) == PNG_SUCCESS) {
            const int pt = png->getPixelType();
            if (png->getWidth() == W && png->getHeight() == H &&
                (pt == PNG_PIXEL_TRUECOLOR || pt == PNG_PIXEL_TRUECOLOR_ALPHA)) {
                s_bgPix = (uint16_t *)heap_caps_malloc((size_t)W * H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (s_bgPix) {
                    ok = png->decode(nullptr, 0) == PNG_SUCCESS;
                    if (!ok) { heap_caps_free(s_bgPix); s_bgPix = nullptr; }
                }
            } else {
                printf("[saver] %s: must be a %dx%d RGB PNG\n", path, W, H);
            }
            png->close();
        }
        png->~PNG();
        heap_caps_free(mem);
    }
    theme_sd::free(file);
    if (!ok) return false;
    memset(&s_bgDsc, 0, sizeof s_bgDsc);
    s_bgDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_bgDsc.header.w = W;
    s_bgDsc.header.h = H;
    s_bgDsc.data_size = (uint32_t)W * H * 2;
    s_bgDsc.data = (const uint8_t *)s_bgPix;
    return true;
}

// ---- the moving lights ------------------------------------------------------------------
enum PKind { P_TWINKLE, P_PULSE, P_BUBBLE, P_MOTE, P_SPARK, P_WASH, P_TRAIL };
struct Part {
    lv_obj_t *o = nullptr;
    PKind kind;
    int   sp;                 // sprite (or -1 for a drawn bubble)
    float x, y, vx, vy;       // centre and speed (px per second)
    float phase, period;      // seconds
    float lo, hi;             // opacity range
    float x0, y0;             // home / spawn point
};
const int MAX_PARTS = 64;
Part     s_p[MAX_PARTS];
int      s_n = 0;

lv_obj_t *s_layer = nullptr;  // the picture and its lights; drifts as one
lv_obj_t *s_bg = nullptr;
int       s_kind = -1;
uint32_t  s_t0 = 0, s_last = 0, s_lastDrift = 0;
int       s_driftStep = 0;
// shooting star
uint32_t  s_nextShoot = 0, s_shootT0 = 0;
bool      s_shooting = false;
float     s_sx, s_sy, s_svx, s_svy;
const int TRAIL = 8;
int       s_trail0 = -1;      // index of the first trail part

float frand() { return (float)rand() / (float)RAND_MAX; }

int add(PKind k, int sp, float x, float y, uint32_t color) {
    if (s_n >= MAX_PARTS) return -1;
    Part &p = s_p[s_n];
    p = Part();
    p.kind = k; p.sp = sp; p.x = p.x0 = x; p.y = p.y0 = y;
    p.vx = p.vy = 0; p.phase = frand() * 6.f; p.period = 2.f; p.lo = 0; p.hi = 255;
    if (sp >= 0) {
        p.o = lv_img_create(s_layer);
        lv_img_set_src(p.o, &s_sp[sp].dsc);
        lv_obj_set_style_img_recolor(p.o, lv_color_hex(color), 0);
        lv_obj_set_style_img_recolor_opa(p.o, LV_OPA_COVER, 0);
        lv_obj_set_style_img_opa(p.o, 0, 0);
    } else {                                   // a bubble: a thin bright ring
        p.o = lv_obj_create(s_layer);
        lv_obj_remove_style_all(p.o);
        lv_obj_set_style_radius(p.o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(p.o, lv_color_hex(color), 0);
        lv_obj_set_style_border_width(p.o, 1, 0);
        lv_obj_set_style_border_opa(p.o, LV_OPA_70, 0);
        lv_obj_set_style_bg_color(p.o, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(p.o, LV_OPA_10, 0);
    }
    lv_obj_clear_flag(p.o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return s_n++;
}

void place(Part &p) {
    int w, h;
    if (p.sp >= 0) { w = s_sp[p.sp].w; h = s_sp[p.sp].h; }
    else { w = lv_obj_get_width(p.o); h = lv_obj_get_height(p.o); }
    lv_obj_set_pos(p.o, (lv_coord_t)lrintf(p.x - w * 0.5f), (lv_coord_t)lrintf(p.y - h * 0.5f));
}

void set_opa(Part &p, float o) {
    if (o < 0) o = 0;
    if (o > 255) o = 255;
    if (p.sp >= 0) lv_obj_set_style_img_opa(p.o, (lv_opa_t)o, 0);
    else lv_obj_set_style_opa(p.o, (lv_opa_t)o, 0);
}

bool in_circle(float x, float y, float margin) {
    const float dx = x - W * 0.5f, dy = y - H * 0.5f;
    return dx * dx + dy * dy < (W * 0.5f - margin) * (W * 0.5f - margin);
}

const uint32_t STAR_TINTS[] = { 0xFFFFFF, 0xCFE0FF, 0xFFF0D8, 0xE6D8FF };

void add_twinkles(const SaverPoint *pts, int n) {
    for (int i = 0; i < n; ++i) {
        const int k = add(P_TWINKLE, SP_STAR, pts[i].x, pts[i].y, STAR_TINTS[i % 4]);
        if (k < 0) return;
        s_p[k].period = 1.6f + frand() * 2.8f;
        s_p[k].lo = 10; s_p[k].hi = 235;
        place(s_p[k]);
    }
}

void add_trail() {
    s_trail0 = s_n;
    for (int i = 0; i < TRAIL; ++i) {
        const int k = add(P_TRAIL, SP_DOT, -50, -50, i == 0 ? 0xFFFFFF : 0xBFD8FF);
        if (k < 0) { s_trail0 = -1; return; }
        place(s_p[k]);
    }
    s_nextShoot = lv_tick_get() + 3000 + rand() % 5000;
}

void build(int kind) {
    switch (kind) {
    case COSMIC:
        add_twinkles(SAVER_COSMIC_POINTS, sizeof SAVER_COSMIC_POINTS / sizeof SAVER_COSMIC_POINTS[0]);
        // the nebula breathes: two slow, faint washes of light over its brightest parts
        for (int i = 0; i < 2; ++i) {
            const int k = add(P_WASH, SP_WASH, 250 + i * 60, 140 + i * 70, i ? 0xC060FF : 0x60A0FF);
            if (k < 0) break;
            s_p[k].period = 7.f + i * 3.f; s_p[k].lo = 0; s_p[k].hi = 70;
            s_p[k].vx = i ? -3.f : 3.f;
            place(s_p[k]);
        }
        add_trail();
        break;
    case AURORA: {
        add_twinkles(SAVER_AURORA_POINTS, sizeof SAVER_AURORA_POINTS / sizeof SAVER_AURORA_POINTS[0]);
        // the curtains ripple: washes of cyan, violet and pink sliding along the band
        const uint32_t cols[] = { 0x70E8FF, 0x9070FF, 0xD070E0, 0x70C0FF };
        for (int i = 0; i < 4; ++i) {
            const int k = add(P_WASH, SP_WASH, 60 + i * 110, 120 + (i % 2) * 40, cols[i]);
            if (k < 0) break;
            s_p[k].period = 4.f + frand() * 3.f; s_p[k].lo = 10; s_p[k].hi = 95;
            s_p[k].vx = (i % 2 ? -1.f : 1.f) * (8.f + frand() * 6.f);
            place(s_p[k]);
        }
        add_trail();
        break;
    }
    case OCEAN: {
        const int nj = sizeof SAVER_OCEAN_POINTS / sizeof SAVER_OCEAN_POINTS[0];
        const uint32_t jcol[] = { 0xFF80D8, 0xA090FF, 0x70D8FF };
        for (int i = 0; i < nj; ++i) {
            const int sp = SAVER_OCEAN_POINTS[i].r >= 20 ? SP_BIG : SP_GLOW;   // a small jellyfish gets a small glow
            const int k = add(P_PULSE, sp, SAVER_OCEAN_POINTS[i].x, SAVER_OCEAN_POINTS[i].y, jcol[i % 3]);
            if (k < 0) break;
            s_p[k].period = 2.6f + i * 0.7f; s_p[k].lo = 30; s_p[k].hi = 150;
            place(s_p[k]);
        }
        for (int i = 0; i < 14; ++i) {
            const int k = add(P_BUBBLE, -1, 0, 0, 0xCFF0FF);
            if (k < 0) break;
            const int d = 5 + rand() % 9;
            lv_obj_set_size(s_p[k].o, d, d);
            s_p[k].x0 = 110 + frand() * 250;
            s_p[k].x = s_p[k].x0;
            s_p[k].y = 120 + frand() * 260;                // already on their way up
            s_p[k].vy = -(18.f + frand() * 22.f);
            s_p[k].period = 1.5f + frand();
            place(s_p[k]);
        }
        for (int i = 0; i < 10; ++i) {                     // drifting specks catching the light
            const int k = add(P_MOTE, SP_DOT, 60 + frand() * 340, 60 + frand() * 300, 0xBFE0FF);
            if (k < 0) break;
            s_p[k].vx = (frand() - 0.5f) * 6.f; s_p[k].vy = 3.f + frand() * 5.f;
            s_p[k].lo = 40; s_p[k].hi = 140; s_p[k].period = 3.f + frand() * 3.f;
            place(s_p[k]);
        }
        break;
    }
    case GLOW: {
        const uint32_t cols[] = { 0x60D8FF, 0x6090FF, 0xB080FF, 0x60D8FF, 0xFF80D8, 0xFFC860 };
        for (int i = 0; i < SAVER_GLOW_SPOT_COUNT; ++i) {
            const int k = add(P_PULSE, SP_GLOW, SAVER_GLOW_SPOTS[i].x, SAVER_GLOW_SPOTS[i].y, cols[i % 6]);
            if (k < 0) break;
            s_p[k].period = 2.2f + frand() * 2.5f; s_p[k].lo = 0; s_p[k].hi = 170;
            place(s_p[k]);
        }
        const uint32_t bok[] = { 0xFFA040, 0x6090FF, 0xD060C0, 0x60D8FF, 0xB080FF };
        for (int i = 0; i < 5; ++i) {                      // soft lights drifting behind
            const int k = add(P_MOTE, SP_BIG, 60 + frand() * 340, 60 + frand() * 200, bok[i]);
            if (k < 0) break;
            s_p[k].vx = (frand() - 0.5f) * 8.f; s_p[k].vy = (frand() - 0.5f) * 5.f;
            s_p[k].lo = 20; s_p[k].hi = 70; s_p[k].period = 6.f + frand() * 4.f;
            place(s_p[k]);
        }
        for (int i = 0; i < 12; ++i) {                     // sparks rising from the floor
            const int k = add(P_SPARK, SP_DOT, 0, 0, i % 2 ? 0xFFD080 : 0x80E0FF);
            if (k < 0) break;
            s_p[k].x0 = 150 + frand() * 170; s_p[k].x = s_p[k].x0;
            s_p[k].y = 200 + frand() * 190;
            s_p[k].vy = -(10.f + frand() * 16.f);
            s_p[k].period = 2.f + frand() * 2.f;
            place(s_p[k]);
        }
        break;
    }
    }
}

void shoot_start(uint32_t now) {
    // from somewhere along the top, across and down at a shallow angle
    const bool left = rand() % 2;
    s_sx = left ? 120 + frand() * 140 : 200 + frand() * 140;
    s_sy = 60 + frand() * 70;
    const float sp = 330.f + frand() * 120.f;
    const float ang = (20.f + frand() * 18.f) * 0.0174533f;
    s_svx = (left ? -1.f : 1.f) * sp * cosf(ang);
    s_svy = sp * sinf(ang);
    s_shootT0 = now;
    s_shooting = true;
}

}  // namespace

int saver_scene::count() { return KIND_COUNT; }
const char *saver_scene::name(int idx) { return (idx >= 0 && idx < KIND_COUNT) ? NAMES[idx] : ""; }
bool saver_scene::active() { return s_layer != nullptr; }

bool saver_scene::show(lv_obj_t *parent, int idx) {
    hide();
    if (idx < 0 || idx >= KIND_COUNT) return false;
    if (!load_picture(NAMES[idx])) return false;
    const bool spritesOk = make_sprite(s_sp[SP_DOT], 14, 14, SP_DOT) &&
                           make_sprite(s_sp[SP_STAR], 23, 23, SP_STAR) &&
                           make_sprite(s_sp[SP_GLOW], 34, 34, SP_GLOW) &&
                           make_sprite(s_sp[SP_BIG], 110, 110, SP_BIG) &&
                           make_sprite(s_sp[SP_WASH], 180, 90, SP_WASH);
    if (!spritesOk) {
        free_sprites();
        heap_caps_free(s_bgPix); s_bgPix = nullptr;
        return false;
    }
    s_kind = idx;
    s_layer = lv_obj_create(parent);
    lv_obj_remove_style_all(s_layer);
    lv_obj_set_size(s_layer, W, H);
    lv_obj_set_pos(s_layer, 0, 0);
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_bg = lv_img_create(s_layer);
    lv_img_set_src(s_bg, &s_bgDsc);
    lv_obj_set_pos(s_bg, 0, 0);
    lv_obj_clear_flag(s_bg, LV_OBJ_FLAG_CLICKABLE);
    s_n = 0; s_trail0 = -1; s_shooting = false;
    build(idx);
    s_t0 = s_last = s_lastDrift = lv_tick_get();
    s_driftStep = rand() % 64;
    printf("[saver] scene %s (%d lights)\n", NAMES[idx], s_n);
    return true;
}

void saver_scene::tick() {
    if (!s_layer) return;
    const uint32_t now = lv_tick_get();
    float dt = (now - s_last) / 1000.f;
    if (dt > 0.2f) dt = 0.2f;
    s_last = now;
    const float t = (now - s_t0) / 1000.f;

    for (int i = 0; i < s_n; ++i) {
        Part &p = s_p[i];
        switch (p.kind) {
        case P_TWINKLE: {
            // mostly a gentle shimmer; now and then a star flares
            const float s = 0.5f + 0.5f * sinf(t * 6.2832f / p.period + p.phase);
            set_opa(p, p.lo + (p.hi - p.lo) * s * s);
            break;
        }
        case P_PULSE: {
            const float s = 0.5f + 0.5f * sinf(t * 6.2832f / p.period + p.phase);
            set_opa(p, p.lo + (p.hi - p.lo) * s);
            break;
        }
        case P_WASH: {
            p.x += p.vx * dt;
            if (p.x < 40 || p.x > W - 40) p.vx = -p.vx;
            const float s = 0.5f + 0.5f * sinf(t * 6.2832f / p.period + p.phase);
            set_opa(p, p.lo + (p.hi - p.lo) * s);
            place(p);
            break;
        }
        case P_MOTE: {
            p.x += p.vx * dt; p.y += p.vy * dt;
            if (!in_circle(p.x, p.y, 30)) {               // wrapped back in on the far side
                p.x = W - p.x; p.y = H - p.y;
                if (!in_circle(p.x, p.y, 30)) { p.x = p.x0; p.y = p.y0; }
            }
            const float s = 0.5f + 0.5f * sinf(t * 6.2832f / p.period + p.phase);
            set_opa(p, p.lo + (p.hi - p.lo) * s);
            place(p);
            break;
        }
        case P_BUBBLE: {
            p.y += p.vy * dt;
            p.x = p.x0 + 4.f * sinf(t * 6.2832f / p.period + p.phase);
            if (p.y < 40) {                               // reached the light: start again below
                p.y = 360 + frand() * 30;
                p.x0 = 110 + frand() * 250;
            }
            // fade in from the floor, out toward the surface
            const float o = fminf(1.f, (380.f - p.y) / 60.f) * fminf(1.f, (p.y - 40.f) / 80.f);
            set_opa(p, 255.f * o);
            place(p);
            break;
        }
        case P_SPARK: {
            p.y += p.vy * dt;
            p.x = p.x0 + 6.f * sinf(t * 6.2832f / p.period + p.phase);
            if (p.y < 120) { p.y = 380 + frand() * 20; p.x0 = 150 + frand() * 170; }
            const float o = fminf(1.f, (395.f - p.y) / 50.f) * fminf(1.f, (p.y - 120.f) / 90.f);
            set_opa(p, 220.f * o);
            place(p);
            break;
        }
        case P_TRAIL:
            break;                                         // handled below
        }
    }

    // a shooting star now and then
    if (s_trail0 >= 0) {
        if (!s_shooting && (int32_t)(now - s_nextShoot) >= 0) shoot_start(now);
        if (s_shooting) {
            const float st = (now - s_shootT0) / 1000.f;
            const float life = 0.95f;
            const float fade = st < 0.15f ? st / 0.15f : (st > life - 0.3f ? (life - st) / 0.3f : 1.f);
            for (int k = 0; k < TRAIL; ++k) {
                Part &p = s_p[s_trail0 + k];
                const float lag = k * 0.018f;
                const float tt = st - lag;
                p.x = s_sx + s_svx * tt;
                p.y = s_sy + s_svy * tt;
                const float o = (tt < 0 || !in_circle(p.x, p.y, 20)) ? 0.f : 255.f * fade * (1.f - (float)k / TRAIL);
                set_opa(p, o);
                place(p);
            }
            if (st >= life) {
                s_shooting = false;
                for (int k = 0; k < TRAIL; ++k) set_opa(s_p[s_trail0 + k], 0);
                s_nextShoot = now + (s_kind == COSMIC ? 6000 + rand() % 8000 : 9000 + rand() % 12000);
            }
        }
    }

    // the whole picture wanders a few pixels, slowly, so no pixel holds one colour for hours
    if (now - s_lastDrift >= DRIFT_STEP_MS) {
        s_lastDrift = now;
        ++s_driftStep;
        const float a = s_driftStep * 0.098f;               // a full loop in ~64 steps (~4 min)
        lv_obj_set_pos(s_layer, (lv_coord_t)lrintf(DRIFT_PX * sinf(a)), (lv_coord_t)lrintf(DRIFT_PX * sinf(2 * a) * 0.6f));
    }
}

void saver_scene::hide() {
    if (s_layer) { lv_obj_del(s_layer); s_layer = nullptr; }
    s_bg = nullptr;
    s_n = 0; s_trail0 = -1; s_shooting = false; s_kind = -1;
    free_sprites();
    if (s_bgPix) { lv_img_cache_invalidate_src(&s_bgDsc); heap_caps_free(s_bgPix); s_bgPix = nullptr; }
}
