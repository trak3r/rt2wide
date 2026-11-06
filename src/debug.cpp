#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test harness driven by [debug] in rt2wide.ini. The game runs elevated, so outside tools
// cannot click into it; instead a script of logical-coordinate inputs is played from inside.
//   script = wait 5000; click 512 400; key 0x1B; dump; quit
// Commands: wait MS | move X Y | click X Y | rclick X Y | key VK | dump | controls | quit

static void post_mouse(HWND hwnd, UINT msg, WPARAM keys, int lx, int ly) {
    POINT logical = {lx, ly};
    hooks_override_cursor(&logical);
    POINT real = logical;
    presenter_unmap_client(&real);
    PostMessageA(hwnd, msg, keys, MAKELPARAM(real.x, real.y));
}

static void quit_now(const char* why) {
    log_line("Debug: %s, terminating", why);
    trace_summary();
    log_close();
    TerminateProcess(GetCurrentProcess(), 0);
}

static DWORD WINAPI script_thread(void*) {
    char buf[sizeof(g_cfg.script)];
    strcpy(buf, g_cfg.script);
    char* save = nullptr;
    for (char* cmd = strtok_s(buf, ";", &save); cmd; cmd = strtok_s(nullptr, ";", &save)) {
        while (*cmd == ' ') cmd++;
        HWND hwnd = presenter_window();
        char op[16] = "";
        long a = 0, b = 0;
        int n = sscanf(cmd, "%15s %li %li", op, &a, &b);
        if (n < 1) continue;
        log_line("Debug script: %s", cmd);
        if (!_stricmp(op, "wait")) Sleep((DWORD)a);
        else if (!hwnd) log_line("Debug script: no game window");
        else if (!_stricmp(op, "move")) post_mouse(hwnd, WM_MOUSEMOVE, 0, a, b);
        else if (!_stricmp(op, "click") || !_stricmp(op, "rclick")) {
            bool right = op[0] == 'r' || op[0] == 'R';
            post_mouse(hwnd, WM_MOUSEMOVE, 0, a, b);
            Sleep(100);
            post_mouse(hwnd, right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, right ? MK_RBUTTON : MK_LBUTTON, a, b);
            Sleep(300);
            post_mouse(hwnd, right ? WM_RBUTTONUP : WM_LBUTTONUP, 0, a, b);
        } else if (!_stricmp(op, "key")) {
            UINT scan = MapVirtualKeyA((UINT)a, MAPVK_VK_TO_VSC);
            PostMessageA(hwnd, WM_KEYDOWN, (WPARAM)a, 1 | (scan << 16));
            Sleep(80);
            PostMessageA(hwnd, WM_KEYUP, (WPARAM)a, 1 | (scan << 16) | 0xC0000000);
        } else if (!_stricmp(op, "dump")) {
            presenter_request_dump();
            Sleep(300);
        } else if (!_stricmp(op, "controls")) game_dump_layout();
        else if (!_stricmp(op, "quit")) quit_now("script quit");
        else log_line("Debug script: unknown command '%s'", op);
    }
    hooks_override_cursor(nullptr);
    log_line("Debug script: finished");
    return 0;
}

static DWORD WINAPI autoquit_thread(void*) {
    Sleep((DWORD)g_cfg.autoquit_ms);
    quit_now("autoquit_ms elapsed");
    return 0;
}

// Logs access violations in the game's own code; the game's handling is unchanged.
static LONG CALLBACK crash_logger(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* c = ep->ContextRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || c->Eip < 0x401000 || c->Eip >= 0x540000) return EXCEPTION_CONTINUE_SEARCH;
    log_line("Crash: access violation at %08lX (%s %08lX)", c->Eip, er->ExceptionInformation[0] ? "writing" : "reading",
             (DWORD)er->ExceptionInformation[1]);
    log_line("  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX", c->Eax, c->Ebx, c->Ecx,
             c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    const DWORD* sp = (const DWORD*)c->Esp;
    for (int i = 0; i < 40 && !IsBadReadPtr(sp + i, 4); i += 8)
        log_line("  [esp+%02X] %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX", i * 4, sp[i], sp[i + 1], sp[i + 2], sp[i + 3],
                 sp[i + 4], sp[i + 5], sp[i + 6], sp[i + 7]);
    return EXCEPTION_CONTINUE_SEARCH;
}

void debug_start() {
    static bool started;
    if (started) return;
    started = true;
    AddVectoredExceptionHandler(1, crash_logger);
    if (g_cfg.dump_dir[0]) CreateDirectoryA(g_cfg.dump_dir, nullptr);
    if (g_cfg.autoquit_ms > 0) CloseHandle(CreateThread(nullptr, 0, autoquit_thread, nullptr, 0, nullptr));
    if (g_cfg.script[0]) CloseHandle(CreateThread(nullptr, 0, script_thread, nullptr, 0, nullptr));
}
