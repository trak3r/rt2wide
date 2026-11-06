#include "common.h"
#include <stdint.h>
#include <string.h>

// The game reads the mouse and window geometry through these user32 calls. Its import
// table is redirected here so it sees a window of the logical size at the screen origin.

typedef BOOL(WINAPI* GetCursorPosFn)(LPPOINT);
typedef BOOL(WINAPI* PointFn)(HWND, LPPOINT);
typedef BOOL(WINAPI* RectFn)(HWND, LPRECT);
typedef int(WINAPI* GetDeviceCapsFn)(HDC, int);

static GetCursorPosFn o_GetCursorPos;
static PointFn o_ScreenToClient, o_ClientToScreen;
static RectFn o_GetClientRect, o_GetWindowRect;
static GetDeviceCapsFn o_GetDeviceCaps;

static volatile LONG g_cursor_override;
static POINT g_cursor;

void hooks_override_cursor(const POINT* pt) {
    if (pt) g_cursor = *pt;
    InterlockedExchange(&g_cursor_override, pt ? 1 : 0);
}

static BOOL WINAPI h_GetCursorPos(LPPOINT pt) {
    if (g_cursor_override && pt) { *pt = g_cursor; return TRUE; }
    BOOL r = o_GetCursorPos(pt);
    if (r && pt) presenter_map_screen(pt);
    return r;
}

static BOOL WINAPI h_ScreenToClient(HWND hwnd, LPPOINT pt) {
    if (presenter_owns(hwnd, nullptr, nullptr)) return TRUE;
    return o_ScreenToClient(hwnd, pt);
}

static BOOL WINAPI h_ClientToScreen(HWND hwnd, LPPOINT pt) {
    if (presenter_owns(hwnd, nullptr, nullptr)) return TRUE;
    return o_ClientToScreen(hwnd, pt);
}

static BOOL WINAPI h_GetClientRect(HWND hwnd, LPRECT rc) {
    int lw, lh;
    if (rc && presenter_owns(hwnd, &lw, &lh)) { SetRect(rc, 0, 0, lw, lh); return TRUE; }
    return o_GetClientRect(hwnd, rc);
}

static BOOL WINAPI h_GetWindowRect(HWND hwnd, LPRECT rc) {
    int lw, lh;
    if (rc && presenter_owns(hwnd, &lw, &lh)) { SetRect(rc, 0, 0, lw, lh); return TRUE; }
    return o_GetWindowRect(hwnd, rc);
}

// The game only offers its windowed mode on a 16-bit desktop; our surfaces are 16-bit
// whatever the real desktop depth is.
static int WINAPI h_GetDeviceCaps(HDC hdc, int index) {
    int r = o_GetDeviceCaps(hdc, index);
    int reported = r;
    if (index == BITSPIXEL && g_cfg.wide && game_verified()) {
        reported = 16;
        game_on_display_probe();
    }
    TRACE("GDI GetDeviceCaps", "index=%d -> %d (reported %d)", index, r, reported);
    return reported;
}

static bool patch_import(HMODULE mod, const char* dll, const char* name, void* repl, void** orig) {
    uint8_t* base = (uint8_t*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    void* target = (void*)GetProcAddress(GetModuleHandleA(dll), name);

    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char*)(base + d->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA* names = d->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk) : nullptr;
        IMAGE_THUNK_DATA* addrs = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (int i = 0; addrs[i].u1.Function; i++) {
            bool match;
            if (names && !(names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
                match = strcmp((const char*)ibn->Name, name) == 0;
            } else {
                match = (void*)addrs[i].u1.Function == target;
            }
            if (!match) continue;
            if ((void*)addrs[i].u1.Function == repl) return true;
            DWORD old;
            VirtualProtect(&addrs[i], sizeof(addrs[i]), PAGE_READWRITE, &old);
            *orig = (void*)addrs[i].u1.Function;
            addrs[i].u1.Function = (ULONG_PTR)repl;
            VirtualProtect(&addrs[i], sizeof(addrs[i]), old, &old);
            return true;
        }
    }
    return false;
}

void hooks_install() {
    HMODULE exe = GetModuleHandleA(nullptr);
    struct { const char* dll; const char* name; void* repl; void** orig; } hooks[] = {
        {"USER32.dll", "GetCursorPos", (void*)h_GetCursorPos, (void**)&o_GetCursorPos},
        {"USER32.dll", "ScreenToClient", (void*)h_ScreenToClient, (void**)&o_ScreenToClient},
        {"USER32.dll", "ClientToScreen", (void*)h_ClientToScreen, (void**)&o_ClientToScreen},
        {"USER32.dll", "GetClientRect", (void*)h_GetClientRect, (void**)&o_GetClientRect},
        {"USER32.dll", "GetWindowRect", (void*)h_GetWindowRect, (void**)&o_GetWindowRect},
        {"GDI32.dll", "GetDeviceCaps", (void*)h_GetDeviceCaps, (void**)&o_GetDeviceCaps},
    };
    for (auto& h : hooks) {
        void* before = *h.orig;
        bool ok = patch_import(exe, h.dll, h.name, h.repl, h.orig);
        if (!ok || *h.orig != before) log_line("Hook %s!%s: %s", h.dll, h.name, ok ? "ok" : "NOT FOUND");
    }
}
