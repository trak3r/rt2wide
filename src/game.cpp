#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Railroad Tycoon II Platinum (GOG build, RT2_PLAT.EXE). The exe has no ASLR and always
// loads at 0x400000; addresses below come from its disassembly and are only touched after
// the code bytes around them match.

static const uintptr_t kImageBase = 0x400000;
static const uintptr_t kSettingsPtr = 0x57b400;  // -> struct: +0x10/+0x14 window client size, +0x18 display mode

struct Signature {
    uintptr_t va;
    uint8_t bytes[10];
};

static const Signature kSignatures[] = {
    // mov ecx,[settings]; mov [ecx+0x18],eax  (default display mode)
    {0x4efd8f, {0x8B, 0x0D, 0x00, 0xB4, 0x57, 0x00, 0x89, 0x41, 0x18, 0x8B}},
    // mov [edx+0x10],0x400  (default window width)
    {0x4efdcf, {0xC7, 0x42, 0x10, 0x00, 0x04, 0x00, 0x00, 0xA1, 0x00, 0xB4}},
    // mov [eax+0x14],0x300  (default window height)
    {0x4efddb, {0xC7, 0x40, 0x14, 0x00, 0x03, 0x00, 0x00, 0x8B, 0x0D, 0x00}},
    // resize handler storing the windowed-mode size into resolution slot 0
    {0x4f1090, {0x89, 0x3D, 0x2C, 0xD2, 0x55, 0x00, 0x89, 0x35, 0x30, 0xD2}},
    // ui_window_load prologue: sub esp,0x50; mov eax,[0x57fee0]; push ebx; push ebp
    {0x507600, {0x83, 0xEC, 0x50, 0xA1, 0xE0, 0xFE, 0x57, 0x00, 0x53, 0x55}},
    // ui_window_command prologue: mov eax,[0x57feb8]; sub esp,0x4c; test eax,eax
    {0x507400, {0xA1, 0xB8, 0xFE, 0x57, 0x00, 0x83, 0xEC, 0x4C, 0x85, 0xC0}},
};

static const uintptr_t kScreenWidth = 0x57ab99;
static const uintptr_t kScreenHeight = 0x57ab9d;
static const uintptr_t kUiWindowLoad = 0x507600;
static const int kUiWindowLoadPrologue = 8;  // whole instructions copied to the trampoline
static const uintptr_t kUiWindowCommand = 0x507400;
static const int kUiWindowCommandPrologue = 8;

bool game_verified() {
    static int state = -1;
    if (state >= 0) return state == 1;
    state = 0;
    if ((uintptr_t)GetModuleHandleA(nullptr) != kImageBase) {
        log_line("Game: exe not at 0x%x, game-specific tweaks disabled", (unsigned)kImageBase);
        return false;
    }
    for (const Signature& s : kSignatures) {
        if (IsBadReadPtr((void*)s.va, sizeof(s.bytes)) || memcmp((void*)s.va, s.bytes, sizeof(s.bytes)) != 0) {
            log_line("Game: unknown exe build (code at %08X differs), game-specific tweaks disabled", (unsigned)s.va);
            return false;
        }
    }
    state = 1;
    log_line("Game: known RT2_PLAT.EXE build");
    return true;
}

static uint8_t* settings() {
    uint8_t* p = *(uint8_t**)kSettingsPtr;
    if (!p || IsBadWritePtr(p, 0x48)) return nullptr;
    return p;
}

void game_log_settings(const char* when) {
    if (!game_verified()) return;
    uint8_t* s = settings();
    if (!s) { log_line("Game settings (%s): not allocated", when); return; }
    log_line("Game settings (%s): mode=%ld frame=%ld window=(%ld,%ld) %ldx%ld depth=%ld", when, *(LONG*)(s + 0x18),
             *(LONG*)(s + 0x4), *(LONG*)(s + 0x8), *(LONG*)(s + 0xc), *(LONG*)(s + 0x10), *(LONG*)(s + 0x14),
             *(LONG*)(s + 0x1c));
}

// ---------------------------------------------------------------------------
// In-game screen layout. mainwin.win is designed for 1024x768: the map view (control 253)
// is 1024x508 with the control panel below it. On a wider screen the map fills the whole
// screen, the panel is split into sections spread across the bottom and the items pinned
// to the right edge move with it.
//
// Window object: +0x09 width, +0x19/+0x55 copies of it, +0x61 first control.
// Control object: +0x04 id (u16), +0x06 type (u16), +0x0b/+0x0d/+0x0f/+0x11 x/y/w/h (i16),
// +0x57 next control, +0x5b sprite bank. Map view (type 0x10): +0x8b/+0x8f viewport
// width/height. Image (type 4): +0x7b sprite record.

static int16_t& ctl16(uint8_t* c, int off) { return *(int16_t*)(c + off); }

static uint8_t* g_mainwin;

// Sprite banks (.imb, see rrt2-decomp docs/formats/lad.md). Bank: +0 count (u16), +2 records
// (0x44 bytes each). Record: +0x0c group index table, +0x3c groups (14 bytes each). Group:
// +0x0a images (0x1d bytes each). Image: +0x05 encoding, +0x09/+0x0d/+0x11/+0x15 x/y/w/h,
// +0x19 pixel data from the game heap. Encoding 3 stores each row as [length][tokens]00,
// the length counting itself (two bytes, big-endian, when bit 7 is set). Tokens (drawer at
// 0x4ff4b1): 1..0x7F n opaque palette indices follow; 0x80|t<<3|n shades n pixels with
// table t; 0xA0|n n blended indices follow; 0xC0|n skips n pixels. A zero count in a shade
// or blend token means the count is in the next byte. The drawer stops at 0xC0 (skip 0),
// which ends the last row in place of 00 and is followed by C0 00.
static uint8_t* control_image(uint8_t* c) {
    uint8_t* bank = *(uint8_t**)(c + 0x5b);
    if (!bank || IsBadReadPtr(bank, 6)) return nullptr;
    uint16_t idx = *(uint16_t*)(c + 0x7b);
    if (idx >= *(uint16_t*)bank) return nullptr;
    uint8_t* rec = *(uint8_t**)(bank + 2) + 0x44 * idx;
    if (IsBadReadPtr(rec, 0x44)) return nullptr;
    uint8_t* grp = *(uint8_t**)(rec + 0x3c) + 14 * *(uint32_t*)(rec + 0x0c);
    if (IsBadReadPtr(grp, 14)) return nullptr;
    uint8_t* img = *(uint8_t**)(grp + 0x0a);
    if (IsBadReadPtr(img, 0x1d) || *(uint32_t*)(img + 5) != 3) return nullptr;
    return img;
}

// Decoded pixel: -1 transparent, otherwise kind << 8 | value.
enum { kOpaque = 0, kShade = 1, kBlend = 2 };

static bool sprite_decode(uint8_t* img, int16_t* out) {
    int w = *(int*)(img + 0x11), h = *(int*)(img + 0x15);
    const uint8_t* p = *(const uint8_t**)(img + 0x19);
    for (int i = 0; i < w * h; i++) out[i] = -1;
    for (int y = 0; y < h; y++) {
        int len = p[0] & 0x80 ? ((p[0] & 0x7f) << 8) | p[1] : p[0];
        const uint8_t* q = p + (p[0] & 0x80 ? 2 : 1);
        const uint8_t* end = p + len;
        int16_t* row = out + y * w;
        int x = 0;
        while (q < end) {
            uint8_t t = *q++;
            if (t == 0) break;
            if (t >= 0xC0) { x += t - 0xC0; continue; }
            int n, kind, shade = 0;
            if (t < 0x80) n = t, kind = kOpaque;
            else if (t < 0xA0) { n = t & 7; shade = (t >> 3) & 3; kind = kShade; if (!n) n = *q++; }
            else { n = t & 0x1f; kind = kBlend; if (!n) n = *q++; }
            for (int i = 0; i < n; i++, x++) {
                int16_t v = (int16_t)(kind << 8 | (kind == kShade ? shade : *q++));
                if (x < w) row[x] = v;
            }
        }
        if (q != end) { log_line("Game: sprite row %d length mismatch", y); return false; }
        p = end;
    }
    return true;
}

// Encodes w x h pixels in the game's format into a buffer from the game heap.
typedef void*(__cdecl* GameMallocFn)(size_t);
typedef void(__cdecl* GameFreeFn)(void*);
static const GameMallocFn game_malloc = (GameMallocFn)0x51b6b7;
static const GameFreeFn game_free = (GameFreeFn)0x51b64e;

static uint8_t* sprite_encode(const int16_t* px, int w, int h) {
    size_t cap = 2;
    for (int y = 0; y < h; y++) cap += 3 + w * 2;
    uint8_t* tmp = (uint8_t*)malloc(cap);
    uint8_t* o = tmp;
    for (int y = 0; y < h; y++) {
        const int16_t* row = px + y * w;
        int last = w;
        while (last > 0 && row[last - 1] < 0) last--;
        bool final_row = y == h - 1;
        if (last == 0 && !final_row) { *o++ = 1; continue; }
        uint8_t* start = o;
        o += 2;
        for (int x = 0; x < last;) {
            int16_t v = row[x];
            int kind = v < 0 ? -1 : v >> 8;
            int n = 1;
            while (x + n < last && (kind == kShade ? row[x + n] == v : (row[x + n] < 0 ? -1 : row[x + n] >> 8) == kind)) n++;
            if (kind < 0) {
                if (n > 63) n = 63;
                *o++ = (uint8_t)(0xC0 | n);
            } else if (kind == kOpaque) {
                if (n > 0x7f) n = 0x7f;
                *o++ = (uint8_t)n;
                for (int i = 0; i < n; i++) *o++ = (uint8_t)row[x + i];
            } else if (kind == kShade) {
                if (n > 255) n = 255;
                uint8_t t = (uint8_t)(0x80 | (v & 3) << 3);
                if (n <= 7) *o++ = (uint8_t)(t | n);
                else { *o++ = t; *o++ = (uint8_t)n; }
            } else {
                if (n > 255) n = 255;
                if (n <= 31) *o++ = (uint8_t)(0xA0 | n);
                else { *o++ = 0xA0; *o++ = (uint8_t)n; }
                for (int i = 0; i < n; i++) *o++ = (uint8_t)row[x + i];
            }
            x += n;
        }
        *o++ = final_row ? 0xC0 : 0;
        int len = (int)(o - start);
        if (len - 1 < 0x80) {
            memmove(start + 1, start + 2, len - 2);
            o--;
            start[0] = (uint8_t)(len - 1);
        } else {
            start[0] = (uint8_t)(0x80 | len >> 8);
            start[1] = (uint8_t)len;
        }
    }
    *o++ = 0xC0;
    *o++ = 0;
    size_t size = o - tmp;
    uint8_t* out = (uint8_t*)game_malloc(size);
    if (out) memcpy(out, tmp, size);
    free(tmp);
    return out;
}

static void write_mask_bmp(const char* path, const int16_t* px, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int stride = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = {0x4D42, (DWORD)(54 + stride * h), 0, 0, 54};
    BITMAPINFOHEADER ih = {sizeof(ih), w, h, 1, 24};
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    uint8_t* line = (uint8_t*)calloc(stride, 1);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            int v = px[y * w + x];
            uint8_t* o = line + x * 3;
            if (v < 0) { o[0] = 255; o[1] = 0; o[2] = 255; }
            else if (v >> 8 == kShade) { o[0] = 160; o[1] = 0; o[2] = 0; }
            else if (v >> 8 == kBlend) { o[0] = 0; o[1] = 160; o[2] = 0; }
            else o[0] = o[1] = o[2] = (uint8_t)v;
        }
        fwrite(line, stride, 1, f);
    }
    free(line);
    fclose(f);
}

void game_dump_layout() {
    uint8_t* win = g_mainwin;
    if (!win || IsBadReadPtr(win, 0x69)) { log_line("Layout: mainwin not loaded"); return; }
    log_line("Layout: mainwin at (%d,%d) %dx%d", *(int*)(win + 1), *(int*)(win + 5), *(int*)(win + 9), *(int*)(win + 13));
    int n = 0;
    for (uint8_t* c = *(uint8_t**)(win + 0x61); c && n < 500; c = *(uint8_t**)(c + 0x57), n++) {
        if (IsBadReadPtr(c, 0xa0)) break;
        const char* bank = *(const char**)(c + 0x31);
        if (!bank || IsBadReadPtr(bank, 1)) bank = "";
        uint16_t type = *(uint16_t*)(c + 6);
        char extra[64] = "";
        if (type == 4) snprintf(extra, sizeof(extra), " rec=%u a=%08lX b=%08lX", *(uint16_t*)(c + 0x7b), *(DWORD*)(c + 0x7d), *(DWORD*)(c + 0x81));
        log_line("  id=%-5u type=%-4X z=%-3u rect=(%d,%d %dx%d) flags=%08lX bank=%s%s", *(uint16_t*)(c + 4), type, c[8],
                 ctl16(c, 0x0b), ctl16(c, 0x0d), ctl16(c, 0x0f), ctl16(c, 0x11), *(DWORD*)(c + 0x23), bank, extra);
        uint8_t* b = *(uint8_t**)(c + 0x5b);
        if (type == 4 && b && !IsBadReadPtr(b, 6)) {
            uint8_t* rec = *(uint8_t**)(b + 2) + 0x44 * *(uint16_t*)(c + 0x7b);
            if (IsBadReadPtr(rec, 0x44)) continue;
            uint8_t* grp = *(uint8_t**)(rec + 0x3c) + 14 * *(uint32_t*)(rec + 0x0c);
            if (IsBadReadPtr(grp, 14)) continue;
            uint8_t* img = *(uint8_t**)(grp + 0x0a);
            if (IsBadReadPtr(img, 0x1d)) continue;
            uint8_t* d = *(uint8_t**)(img + 0x19);
            char hex[100] = "";
            for (int i = 0; i < 24 && d && !IsBadReadPtr(d + i, 1); i++) snprintf(hex + i * 3, 4, "%02X ", d[i]);
            log_line("     img groups=%u k=%u cnt=%u flags=%02X enc=%lu x=%d y=%d w=%d h=%d data=%p: %s", *(uint32_t*)(rec + 0x40),
                     *(uint32_t*)grp, *(uint16_t*)(grp + 4), img[0], *(DWORD*)(img + 5), *(int*)(img + 9), *(int*)(img + 13),
                     *(int*)(img + 17), *(int*)(img + 21), d, hex);
            uint8_t* im = control_image(c);
            if (im && g_cfg.dump_dir[0] && !strcmp(bank, "mwfrmm.imb")) {
                int w = *(int*)(im + 0x11), h = *(int*)(im + 0x15);
                int16_t* px = (int16_t*)malloc(sizeof(int16_t) * w * h);
                if (sprite_decode(im, px)) {
                    uint8_t copy[0x1d];
                    memcpy(copy, im, sizeof(copy));
                    uint8_t* enc = sprite_encode(px, w, h);
                    *(uint8_t**)(copy + 0x19) = enc;
                    int16_t* px2 = (int16_t*)malloc(sizeof(int16_t) * w * h);
                    bool same = enc && sprite_decode(copy, px2) && !memcmp(px, px2, sizeof(int16_t) * w * h);
                    log_line("     round trip %s", same ? "ok" : "FAILED");
                    free(px2);
                    if (enc) game_free(enc);
                    char path[MAX_PATH];
                    snprintf(path, sizeof(path), "%s\\mwfrmm_%u.bmp", g_cfg.dump_dir, *(uint16_t*)(c + 0x7b));
                    write_mask_bmp(path, px, w, h);
                }
                free(px);
            }
        }
    }
}

// The control panel is split into three sections spread across the screen: the minimap on
// the left, the message panel in the centre and the logo panel on the right, with the map
// showing between them. The ticker band along the bottom spans the full width.
static const int kCutLeft = 280;       // first column of the centre section
static const int kCutRight = 738;      // first column of the right section
static const int kTickerRow = 223;     // first ticker row in the panel sprite (y 731)
static const int kTickerCapLeft = 300; // the ticker repeats columns [300, 700) in its middle
static const int kTickerCapRight = 700;

static int section_shift(int x, int dx) { return x < kCutLeft ? 0 : x < kCutRight ? dx / 2 : dx; }

// mwfrmm.imb sprites: 1 the tab at the right edge, 2 the strip above the panel with the
// zoom button sockets, 3 the panel itself. They are drawn at their own x/y, so the split
// rewrites the shared sprite; the bank stays loaded, so it is only done once.
static bool split_panel_sprite(uint8_t* img, int sprite, int dx) {
    int w = *(int*)(img + 0x11), h = *(int*)(img + 0x15), nw = w + dx;
    if (w != 1024) return w == 1024 + dx;
    int16_t* px = (int16_t*)malloc(sizeof(int16_t) * w * h);
    int16_t* out = (int16_t*)malloc(sizeof(int16_t) * nw * h);
    bool ok = px && out && sprite_decode(img, px);
    if (ok) {
        for (int i = 0; i < nw * h; i++) out[i] = -1;
        for (int y = 0; y < h; y++) {
            const int16_t* src = px + y * w;
            int16_t* dst = out + y * nw;
            if (sprite == 3 && y >= kTickerRow) {
                for (int x = 0; x < nw; x++) {
                    int sx = x < kTickerCapLeft ? x
                           : x >= nw - (w - kTickerCapRight) ? x - dx
                           : kTickerCapLeft + (x - kTickerCapLeft) % (kTickerCapRight - kTickerCapLeft);
                    dst[x] = src[sx];
                }
                continue;
            }
            // The strip's upper rows have a wider cap on the minimap frame and start the
            // logo frame one column earlier.
            bool upper = sprite == 2 && y < 80;
            int cut_l = upper ? 285 : kCutLeft, cut_r = upper ? 737 : kCutRight;
            for (int x = 0; x < w; x++)
                dst[x + (x < cut_l ? 0 : x < cut_r ? dx / 2 : dx)] = src[x];
        }
        uint8_t* enc = sprite_encode(out, nw, h);
        ok = enc != nullptr;
        if (ok) {
            game_free(*(uint8_t**)(img + 0x19));
            *(uint8_t**)(img + 0x19) = enc;
            *(int*)(img + 0x11) = nw;
        }
    }
    free(px);
    free(out);
    return ok;
}

static int screen_dx() { return *(int*)kScreenWidth - 1024; }
static int screen_dy() { return *(int*)kScreenHeight - 768; }

// Frame sprites (ids 334-336) get their bank from code after the layout loads (mwfrme,
// mwfrms or mwfrmm.imb by game mode) and take their rect from the sprite.
// Sprites 0-3 are the toolbar column, the right edge tab, the strip and the panel; the y they
// were drawn for is checked so a bank that stays loaded is only moved once.
static void fix_frame_control(uint8_t* c, int dx, int dy) {
    static const int kFrameY[] = {6, 353, 403, 508};
    uint16_t id = *(uint16_t*)(c + 0x04);
    uint8_t* img = control_image(c);
    if (!img || id < 333 || id > 336) return;
    int sprite = id - 333;
    if (*(int*)(img + 0x0d) == kFrameY[sprite]) {
        *(int*)(img + 0x0d) += dy;
        if (sprite == 1) *(int*)(img + 0x09) += dx;
    }
    if ((sprite == 2 || sprite == 3) && !split_panel_sprite(img, sprite, dx))
        log_line("Game: frame sprite %u not recognised, left unsplit", id);
    ctl16(c, 0x0b) = (int16_t)*(int*)(img + 0x09);
    ctl16(c, 0x0d) = (int16_t)*(int*)(img + 0x0d);
    ctl16(c, 0x0f) = (int16_t)*(int*)(img + 0x11);
}

// How far a control of the 1024x768 layout moves, for controls that keep their size. On a
// taller screen the panel and the toolbar above it go to the bottom; the map, compass and
// screen-edge strips are handled separately.
static int control_shift(uint8_t* c, int x, int dx) {
    uint16_t id = *(uint16_t*)(c + 0x04), type = *(uint16_t*)(c + 0x06);
    if (id == 253 || id == 268 || id == 255 || (id >= 333 && id <= 336) || (type == 0x80 && id == 0)) return 0;
    if (id >= 280 && id <= 284) return dx;  // compass
    return section_shift(x + ctl16(c, 0x0f) / 2, dx);
}

static int control_vshift(uint8_t* c, int dy) {
    uint16_t id = *(uint16_t*)(c + 0x04), type = *(uint16_t*)(c + 0x06);
    if (id == 253 || id == 268 || (id >= 280 && id <= 284) || (id >= 333 && id <= 336) || (type == 0x80 && id == 0)) return 0;
    return dy;
}

static void widen_mainwin(uint8_t* win) {
    g_mainwin = win;
    int dx = screen_dx(), dy = screen_dy();
    if (dx < 0 || dy < 0 || (dx == 0 && dy == 0)) return;
    if (*(int*)(win + 0x09) != 1024 || *(int*)(win + 0x0d) != 768) return;
    *(int*)(win + 0x09) += dx;
    *(int*)(win + 0x19) += dx;
    *(int*)(win + 0x55) += dx;
    *(int*)(win + 0x0d) += dy;
    *(int*)(win + 0x1d) += dy;
    *(int*)(win + 0x59) += dy;

    int changed = 0;
    for (uint8_t* c = *(uint8_t**)(win + 0x61); c; c = *(uint8_t**)(c + 0x57)) {
        uint16_t id = *(uint16_t*)(c + 0x04), type = *(uint16_t*)(c + 0x06);
        int16_t x = ctl16(c, 0x0b), y = ctl16(c, 0x0d), w = ctl16(c, 0x0f), h = ctl16(c, 0x11);
        int nx = x, ny = y, nw = w, nh = h;
        if (id == 253 || id == 268) {
            // The map fills the screen; the panel sits over it with the map showing between.
            nw = w + dx;
            nh = 768 + dy;
            if (type == 0x10) {
                *(int*)(c + 0x8b) += nw - w;
                *(int*)(c + 0x8f) += nh - h;
            }
        } else if (type == 0x80 && id == 0) {
            // Screen-edge strips: 6px wide along each edge, with corner pieces.
            if (x + w == 1018) nw = w + dx;
            if (x == 1018) nx = x + dx;
            if (y + h == 762) nh = h + dy;
            if (y == 762) ny = y + dy;
        } else if (id >= 333 && id <= 336) {
            fix_frame_control(c, dx, dy);
            changed++;
            continue;
        } else {
            nx = x + control_shift(c, x, dx);
            ny = y + control_vshift(c, dy);
            if (id == 255) nw = w + dx;  // ticker text
        }
        if (nx == x && ny == y && nw == w && nh == h) continue;
        ctl16(c, 0x0b) = (int16_t)nx;
        ctl16(c, 0x0d) = (int16_t)ny;
        ctl16(c, 0x0f) = (int16_t)nw;
        ctl16(c, 0x11) = (int16_t)nh;
        changed++;
    }
    log_line("Game: mainwin.win laid out for %dx%d (%d controls changed)", 1024 + dx, 768 + dy, changed);
}

static uint8_t* find_control(uint8_t* win, uint16_t id) {
    for (uint8_t* c = *(uint8_t**)(win + 0x61); c; c = *(uint8_t**)(c + 0x57))
        if (*(uint16_t*)(c + 0x04) == id) return c;
    return nullptr;
}

// Window commands (0x507400, thiscall(win, msg), ret 4). msg: +0 command, +4 control id,
// then arguments. 0x72 sets a control's sprite bank (+8 name), 0x73 moves it (+8 x, +0xc y).
enum { kCmdSetBank = 0x72, kCmdMove = 0x73 };
typedef int(__fastcall* UiWindowCommandFn)(void* win, void* edx, int* msg);
static UiWindowCommandFn o_ui_window_command;

static int __fastcall h_ui_window_command(void* win, void* edx, int* msg) {
    if (win != g_mainwin || !msg) return o_ui_window_command(win, edx, msg);
    int dx = screen_dx(), dy = screen_dy();
    int cmd = msg[0];
    uint8_t* c = (cmd == kCmdSetBank || cmd == kCmdMove) ? find_control((uint8_t*)win, (uint16_t)msg[1]) : nullptr;
    if (!c) return o_ui_window_command(win, edx, msg);
    if (cmd == kCmdMove) {
        int x = msg[2], y = msg[3];
        msg[2] = x + control_shift(c, x, dx);
        msg[3] = y + control_vshift(c, dy);
        int r = o_ui_window_command(win, edx, msg);
        msg[2] = x;
        msg[3] = y;
        return r;
    }
    int r = o_ui_window_command(win, edx, msg);
    fix_frame_control(c, dx, dy);
    return r;
}

typedef void*(__fastcall* UiWindowLoadFn)(void* win, void* edx, int x, int y, const char* name);
static UiWindowLoadFn o_ui_window_load;

// Layouts the game places for a 1024x768 screen. Full-screen pages are drawn at 0,0 and
// some dialogs at fixed positions that only centre on 1024x768; both are moved to the
// centre. Widths come from the .win files (re/tools/windump).
static const char* const kFullScreenPages[] = {
    "campaign.win", "compndtl.win", "compnlst.win", "credits.win", "hiscore.win", "playrdtl.win",
    "playrlst.win", "set.win", "setup.win", "setupg.win", "setupl.win", "statndtl.win",
    "statnlst.win", "stockdtl.win", "traindtl.win", "trainlst.win",
};
static const struct { const char* name; int w; } kDialogs[] = {
    {"buildbuy.win", 774}, {"cityset.win", 774}, {"eagled.win", 513}, {"eventset.win", 774},
    {"fileopt.win", 475},  {"filerq.win", 562},  {"helpd.win", 401},  {"mapset.win", 774},
    {"paper.win", 771},    {"settings.win", 774}, {"trainbuy.win", 774}, {"winlose.win", 598},
    {"yearend.win", 521},
};

// Whether a window was placed for 1024x768. Dialogs the game centres on the real screen
// itself sit about dx/2 further right; those are left alone in both directions.
static bool placed_for_1024(const char* name, int x, int y) {
    if (!name) return false;
    for (const char* page : kFullScreenPages)
        if (_stricmp(name, page) == 0) return x == 0 && y == 0;
    for (const auto& d : kDialogs) {
        if (_stricmp(name, d.name) != 0) continue;
        int centred_1024 = (1024 - d.w) / 2;
        return x + d.w <= 1024 && abs(x - centred_1024) <= 48;
    }
    return false;
}

static void* __fastcall h_ui_window_load(void* win, void* edx, int x, int y, const char* name) {
    int dx = screen_dx(), dy = screen_dy();
    bool centre = (dx > 0 || dy > 0) && placed_for_1024(name, x, y);
    TRACE("Game ui_window_load", "%s at (%d,%d)%s", name ? name : "?", x, y, centre ? " -> centred" : "");
    if (win == g_mainwin) g_mainwin = nullptr;
    void* r = o_ui_window_load(win, edx, centre ? x + dx / 2 : x, centre ? y + dy / 2 : y, name);
    if (name && _stricmp(name, "mainwin.win") == 0) widen_mainwin((uint8_t*)win);
    return r;
}

// Redirects a function whose first `prologue` bytes are whole instructions; returns a
// trampoline that runs them and continues in the original.
static void* hook_code(uintptr_t va, int prologue, void* detour) {
    uint8_t* target = (uint8_t*)va;
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, target, prologue);
    tramp[prologue] = 0xE9;
    *(int32_t*)(tramp + prologue + 1) = (int32_t)((target + prologue) - (tramp + prologue + 5));

    DWORD old;
    VirtualProtect(target, prologue, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(int32_t*)(target + 1) = (int32_t)((uint8_t*)detour - (target + 5));
    for (int i = 5; i < prologue; i++) target[i] = 0x90;
    VirtualProtect(target, prologue, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, prologue);
    return tramp;
}

static void install_layout_hook() {
    o_ui_window_load = (UiWindowLoadFn)hook_code(kUiWindowLoad, kUiWindowLoadPrologue, (void*)h_ui_window_load);
    o_ui_window_command = (UiWindowCommandFn)hook_code(kUiWindowCommand, kUiWindowCommandPrologue, (void*)h_ui_window_command);
    if (!o_ui_window_load || !o_ui_window_command) { log_line("Game: no memory for layout hooks"); return; }
    log_line("Game: layout hooks installed");
}

// One-byte opcodes with a 32-bit immediate operand; kModRmEsi is the byte after 0x81 in
// "op esi, imm32" (/0 = add).
enum { kPushImm32 = 0x68, kMovEdxImm32 = 0xBA, kAddEaxImm32 = 0x05, kModRmEsi = 0xC6 };

static bool is_imm32(uintptr_t va, uint8_t opcode, uint32_t value) {
    const uint8_t* p = (const uint8_t*)va;
    return p[0] == opcode && *(const uint32_t*)(p + 1) == value;
}

static void patch_imm32(uintptr_t va, uint32_t value) {
    uint8_t* p = (uint8_t*)va;
    DWORD old;
    VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old);
    *(uint32_t*)(p + 1) = value;
    VirtualProtect(p + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p + 1, 4);
}

// The "Game Paused" / "Game Speed" message (text at 0x57fef8) is drawn centred in a fixed
// box (100,214)-(923,343), centred on 1024x768. Widening the box to keep the same side
// margins and moving it down by half the extra height centres it on the real screen; the
// box is also the area redrawn around the message.
static void centre_flash_message(int screen_w, int screen_h) {
    struct Imm { uintptr_t va; uint8_t opcode; uint32_t value; uint32_t patched; };
    int dy = (screen_h - 768) / 2;
    const Imm kImms[] = {
        // redraw box: right edge, top and bottom at each place the box is invalidated or tested
        {0x502324, kPushImm32, 0x39b, (uint32_t)screen_w - 101}, {0x502329, kMovEdxImm32, 0xd6, 0xd6u + dy},
        {0x50231f, kPushImm32, 0x157, 0x157u + dy},
        {0x50276a, kPushImm32, 0x39b, (uint32_t)screen_w - 101}, {0x50276f, kMovEdxImm32, 0xd6, 0xd6u + dy},
        {0x502765, kPushImm32, 0x157, 0x157u + dy},
        {0x5027d9, kPushImm32, 0x39b, (uint32_t)screen_w - 101}, {0x5027de, kMovEdxImm32, 0xd6, 0xd6u + dy},
        {0x5027d4, kPushImm32, 0x157, 0x157u + dy},
        {0x502de9, kPushImm32, 0x39b, (uint32_t)screen_w - 101}, {0x502dee, kMovEdxImm32, 0xd6, 0xd6u + dy},
        {0x502de4, kPushImm32, 0x157, 0x157u + dy},
        // text: width and top
        {0x5027a5, kPushImm32, 0x338, (uint32_t)screen_w - 200}, {0x5027aa, kPushImm32, 0xd6, 0xd6u + dy},
    };
    for (const Imm& i : kImms)
        if (!is_imm32(i.va, i.opcode, i.value)) { log_line("Game: pause/speed message not recognised, left as is"); return; }
    for (const Imm& i : kImms) patch_imm32(i.va, i.patched);
    log_line("Game: pause/speed message centred");
}

// The in-game speed slider sits on the frame tab at a fixed top of 370, added in both where
// the thumb is placed (add esi, 0x172) and where a click on the slider becomes a speed
// (add eax, 0x172). The tab moves down with the HUD, so the slider moves with it.
static void move_speed_slider(int dy) {
    const uintptr_t kPlaceThumb = 0x47e0e2, kReadClick = 0x47be50;
    if (*(const uint8_t*)kPlaceThumb != 0x81 || !is_imm32(kPlaceThumb + 1, kModRmEsi, 0x172) ||
        !is_imm32(kReadClick, kAddEaxImm32, 0x172)) {
        log_line("Game: speed slider not recognised, left as is");
        return;
    }
    patch_imm32(kPlaceThumb + 1, 0x172u + dy);
    patch_imm32(kReadClick, 0x172u + dy);
    log_line("Game: speed slider moved down %d", dy);
}

// Runs inside the game's display probe, after the settings defaults are set and before a
// mode is chosen. Mode 0 is the game's own resizable window; its size is taken from +0x10/+0x14.
void game_on_display_probe() {
    static bool done;
    if (done || !g_cfg.wide || !game_verified()) return;
    done = true;
    uint8_t* s = settings();
    if (!s) { log_line("Game: settings not allocated at display probe, staying in classic mode"); return; }
    game_log_settings("before");
    int w, h;
    presenter_wide_size(&w, &h);
    *(LONG*)(s + 0x18) = 0;
    *(LONG*)(s + 0x10) = w;
    *(LONG*)(s + 0x14) = h;
    game_log_settings("forced wide");
    install_layout_hook();
    if (w > 1024 || h > 768) centre_flash_message(w, h);
    if (h > 768) move_speed_slider(h - 768);
}
