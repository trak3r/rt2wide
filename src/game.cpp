#include "common.h"
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
};

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
}
