#include "ddraw_impl.h"
#include <stdio.h>
#include <string.h>

typedef HRESULT(WINAPI* DirectDrawCreateFn)(GUID*, LPDIRECTDRAW*, IUnknown*);
typedef HRESULT(WINAPI* DirectDrawCreateClipperFn)(DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*);
typedef HRESULT(WINAPI* DirectDrawEnumerateAFn)(LPDDENUMCALLBACKA, LPVOID);

// With enabled=0 the mod steps aside and the game talks to Windows' own DirectDraw.
static FARPROC system_ddraw(const char* name) {
    static HMODULE mod;
    if (!mod) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat(path, "\\ddraw.dll");
        mod = LoadLibraryA(path);
        log_line("Passthrough: loaded %s -> %p", path, mod);
    }
    return mod ? GetProcAddress(mod, name) : nullptr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
        config_load();
        log_open();
        presenter_init();
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        log_line("rt2wide loaded into %s (enabled=%d, mode=%s)", exe, (int)g_cfg.enabled, g_cfg.wide ? "wide" : "classic");
        // The game probes the desktop before it ever calls DirectDrawCreate.
        if (g_cfg.enabled) hooks_install();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (!reserved) presenter_shutdown();
        trace_summary();
        log_line("rt2wide unloading");
        log_close();
    }
    return TRUE;
}

extern "C" HRESULT WINAPI DirectDrawCreate(GUID* guid, LPDIRECTDRAW* out, IUnknown* outer) {
    log_line("DirectDrawCreate guid=%p", guid);
    if (!g_cfg.enabled) {
        DirectDrawCreateFn fn = (DirectDrawCreateFn)system_ddraw("DirectDrawCreate");
        return fn ? fn(guid, out, outer) : DDERR_GENERIC;
    }
    if (!out) return DDERR_INVALIDPARAMS;
    hooks_install();
    game_log_settings("DirectDrawCreate");
    DDraw* dd = new DDraw();
    *out = static_cast<IDirectDraw*>(&dd->v1);
    return DD_OK;
}

extern "C" HRESULT WINAPI DirectDrawCreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER* out, IUnknown* outer) {
    if (!g_cfg.enabled) {
        DirectDrawCreateClipperFn fn = (DirectDrawCreateClipperFn)system_ddraw("DirectDrawCreateClipper");
        return fn ? fn(flags, out, outer) : DDERR_GENERIC;
    }
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new Clipper();
    return DD_OK;
}

extern "C" HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx) {
    if (!g_cfg.enabled) {
        DirectDrawEnumerateAFn fn = (DirectDrawEnumerateAFn)system_ddraw("DirectDrawEnumerateA");
        return fn ? fn(cb, ctx) : DDERR_GENERIC;
    }
    if (cb) cb(nullptr, (LPSTR) "Primary Display Driver", (LPSTR) "display", ctx);
    return DD_OK;
}
