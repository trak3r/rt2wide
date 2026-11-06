#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

Config g_cfg;
HMODULE g_module;

static char g_dir[MAX_PATH];
static FILE* g_log;
static CRITICAL_SECTION g_log_cs;
static bool g_log_cs_ready;
static DWORD g_start_tick;
static TraceCounter* g_counters;

static void module_dir() {
    GetModuleFileNameA(g_module, g_dir, MAX_PATH);
    char* slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = 0;
}

static bool ini_equals(const char* section, const char* key, const char* def, const char* value, const char* path) {
    char buf[64];
    GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), path);
    return _stricmp(buf, value) == 0;
}

void config_load() {
    module_dir();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%srt2wide.ini", g_dir);

    g_cfg.enabled = GetPrivateProfileIntA("rt2wide", "enabled", 1, path) != 0;
    g_cfg.renderer = ini_equals("display", "renderer", "opengl", "gdi", path) ? Renderer::Gdi : Renderer::OpenGL;
    if (ini_equals("display", "scaling", "aspect", "integer", path)) g_cfg.scaling = Scaling::Integer;
    else if (ini_equals("display", "scaling", "aspect", "stretch", path)) g_cfg.scaling = Scaling::Stretch;
    else g_cfg.scaling = Scaling::Aspect;
    g_cfg.linear_filter = !ini_equals("display", "filter", "linear", "nearest", path);
    g_cfg.vsync = GetPrivateProfileIntA("display", "vsync", 1, path) != 0;
    g_cfg.log = GetPrivateProfileIntA("debug", "log", 1, path) != 0;
    g_cfg.trace_limit = GetPrivateProfileIntA("debug", "trace_limit", 25, path);
}

void log_open() {
    InitializeCriticalSection(&g_log_cs);
    g_log_cs_ready = true;
    g_start_tick = GetTickCount();
    if (!g_cfg.log) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%srt2wide.log", g_dir);
    g_log = fopen(path, "w");
}

void log_close() {
    if (!g_log_cs_ready) return;
    EnterCriticalSection(&g_log_cs);
    if (g_log) { fclose(g_log); g_log = nullptr; }
    LeaveCriticalSection(&g_log_cs);
}

static void log_v(const char* fmt, va_list ap) {
    if (!g_log) return;
    EnterCriticalSection(&g_log_cs);
    if (g_log) {
        fprintf(g_log, "[%7.3f t%05lu] ", (GetTickCount() - g_start_tick) / 1000.0, GetCurrentThreadId());
        vfprintf(g_log, fmt, ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_log_cs);
}

void log_line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_v(fmt, ap);
    va_end(ap);
}

void trace_hit(TraceCounter* tc, const char* fmt, ...) {
    LONG n = InterlockedIncrement(&tc->count);
    if (n == 1 && g_log_cs_ready) {
        EnterCriticalSection(&g_log_cs);
        tc->next = g_counters;
        g_counters = tc;
        LeaveCriticalSection(&g_log_cs);
    }
    if (n > g_cfg.trace_limit) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_line("%s %s%s", tc->name, buf, n == g_cfg.trace_limit ? "  (further calls only counted)" : "");
}

void trace_summary() {
    if (!g_log) return;
    log_line("---- call counts ----");
    for (TraceCounter* tc = g_counters; tc; tc = tc->next) log_line("%8ld  %s", tc->count, tc->name);
}
