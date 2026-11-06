#include "common.h"
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
};

static const uintptr_t kScreenWidth = 0x57ab99;
static const uintptr_t kUiWindowLoad = 0x507600;
static const int kUiWindowLoadPrologue = 8;  // whole instructions copied to the trampoline

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
// screen behind the panel and the items pinned to its right edge move with it.
//
// Window object: +0x09 width, +0x19/+0x55 copies of it, +0x61 first control.
// Control object: +0x04 id (u16), +0x06 type (u16), +0x0b/+0x0d/+0x0f/+0x11 x/y/w/h (i16),
// +0x57 next control. Map view (type 0x10): +0x8b/+0x8f viewport width/height.

static int16_t& ctl16(uint8_t* c, int off) { return *(int16_t*)(c + off); }

static void widen_mainwin(uint8_t* win) {
    int screen_w = *(int*)kScreenWidth;
    int dx = screen_w - 1024;
    if (dx <= 0 || *(int*)(win + 0x09) != 1024) return;
    *(int*)(win + 0x09) += dx;
    *(int*)(win + 0x19) += dx;
    *(int*)(win + 0x55) += dx;
    int stretched = 0, moved = 0;
    for (uint8_t* c = *(uint8_t**)(win + 0x61); c; c = *(uint8_t**)(c + 0x57)) {
        uint16_t id = *(uint16_t*)(c + 0x04), type = *(uint16_t*)(c + 0x06);
        int16_t x = ctl16(c, 0x0b), w = ctl16(c, 0x0f);
        bool stretch = false, move = false;
        if (id == 253 || id == 268) stretch = true;                         // map view and its image
        else if (id >= 280 && id <= 284) move = true;                       // compass
        else if (type == 0x80 && id == 0 && x + w == 1018) stretch = true;  // top/bottom border strips
        else if (type == 0x80 && id == 0 && x == 1018) move = true;         // right border pieces
        if (stretch) {
            ctl16(c, 0x0f) = (int16_t)(w + dx);
            if (type == 0x10) *(int*)(c + 0x8b) += dx;
            // The map continues under the control panel so the space beside it is not empty.
            if (id == 253 || id == 268) {
                int16_t h = ctl16(c, 0x11);
                ctl16(c, 0x11) = 768;
                if (type == 0x10) *(int*)(c + 0x8f) += 768 - h;
            }
            stretched++;
        } else if (move) {
            ctl16(c, 0x0b) = (int16_t)(x + dx);
            moved++;
        }
    }
    log_line("Game: mainwin.win widened to %d (stretched %d controls, moved %d)", screen_w, stretched, moved);
}

typedef void*(__fastcall* UiWindowLoadFn)(void* win, void* edx, int x, int y, const char* name);
static UiWindowLoadFn o_ui_window_load;

// Layouts the game places for a 1024-wide screen. Full-screen pages are drawn at x=0 and
// some dialogs at fixed positions that only centre on 1024; both are moved to the centre.
// Widths come from the .win files (re/tools/windump).
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

static int centre_shift(const char* name, int x) {
    int dx = *(int*)kScreenWidth - 1024;
    if (dx <= 0 || !name) return 0;
    for (const char* page : kFullScreenPages)
        if (_stricmp(name, page) == 0) return x == 0 ? dx / 2 : 0;
    for (const auto& d : kDialogs) {
        if (_stricmp(name, d.name) != 0) continue;
        int centred_1024 = (1024 - d.w) / 2;
        // Dialogs the game already centres on the real screen width sit about dx/2 further right.
        return (x + d.w <= 1024 && abs(x - centred_1024) <= 48) ? dx / 2 : 0;
    }
    return 0;
}

static void* __fastcall h_ui_window_load(void* win, void* edx, int x, int y, const char* name) {
    int shift = centre_shift(name, x);
    TRACE("Game ui_window_load", "%s at (%d,%d)%s", name ? name : "?", x, y, shift ? " -> centred" : "");
    void* r = o_ui_window_load(win, edx, x + shift, y, name);
    if (name && _stricmp(name, "mainwin.win") == 0) widen_mainwin((uint8_t*)win);
    return r;
}

static void install_layout_hook() {
    uint8_t* target = (uint8_t*)kUiWindowLoad;
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { log_line("Game: no memory for layout hook"); return; }
    memcpy(tramp, target, kUiWindowLoadPrologue);
    tramp[kUiWindowLoadPrologue] = 0xE9;
    *(int32_t*)(tramp + kUiWindowLoadPrologue + 1) =
        (int32_t)((target + kUiWindowLoadPrologue) - (tramp + kUiWindowLoadPrologue + 5));
    o_ui_window_load = (UiWindowLoadFn)tramp;

    DWORD old;
    VirtualProtect(target, kUiWindowLoadPrologue, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(int32_t*)(target + 1) = (int32_t)((uint8_t*)h_ui_window_load - (target + 5));
    for (int i = 5; i < kUiWindowLoadPrologue; i++) target[i] = 0x90;
    VirtualProtect(target, kUiWindowLoadPrologue, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, kUiWindowLoadPrologue);
    log_line("Game: layout hook installed");
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
}
