#pragma once

#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <stdint.h>

enum class Renderer { OpenGL, Gdi };
enum class Scaling { Aspect, Integer, Stretch };

struct Config {
    bool enabled = true;
    Renderer renderer = Renderer::OpenGL;
    Scaling scaling = Scaling::Aspect;
    bool linear_filter = true;
    bool vsync = true;
    // wide: run the game in its own resizable windowed mode at a monitor-shaped logical size.
    // Otherwise it keeps its fixed 1024x768 full-screen mode and the picture is scaled.
    bool wide = true;
    int wide_w = 0;  // 0 = derive from the monitor's aspect ratio
    int wide_h = 768;
    bool log = true;
    int trace_limit = 25;
    char dump_dir[MAX_PATH] = "";
    int dump_every_ms = 0;
    int dump_max = 20;
    int autoquit_ms = 0;
    char script[1024] = "";
};

extern Config g_cfg;
extern HMODULE g_module;

void config_load();
void log_open();
void log_close();
void log_line(const char* fmt, ...);

// Per-call-site counter: the first trace_limit hits are logged, the rest are only counted.
struct TraceCounter {
    const char* name;
    volatile LONG count;
    TraceCounter* next;
};
void trace_hit(TraceCounter* tc, const char* fmt, ...);
void trace_summary();

#define TRACE(name, ...)                                         \
    do {                                                         \
        static TraceCounter tc_ = {name, 0, nullptr};            \
        trace_hit(&tc_, __VA_ARGS__);                            \
    } while (0)

// Where the logical (game-sized) frame lands inside the real window client area.
struct Viewport {
    int x, y, w, h;
    int lw, lh;
};

class Surface;

void presenter_init();
// Size the game is told its window has; the primary surface and all coordinate hooks use it.
void presenter_set_logical(int w, int h);
void presenter_wide_size(int* w, int* h);
void presenter_attach_window(HWND hwnd);
void presenter_set_primary(Surface* s);
void presenter_clear_primary(Surface* s);
void presenter_mark_dirty();
void presenter_lock();
void presenter_unlock();
void presenter_shutdown();
bool presenter_viewport(Viewport* out);
// True when hwnd is the game window we are scaling; lw/lh receive the game's logical size.
bool presenter_owns(HWND hwnd, int* lw, int* lh);
// Convert real window-client / real screen coordinates into the game's logical coordinates.
bool presenter_map_client(POINT* pt);
bool presenter_map_screen(POINT* pt);
// Inverse of presenter_map_client: logical point to the centre of its real client pixel.
bool presenter_unmap_client(POINT* pt);
HWND presenter_window();
void presenter_request_dump();

void hooks_install();
// When set, GetCursorPos reports this logical point instead of the real cursor.
void hooks_override_cursor(const POINT* pt);

// Game-specific memory tweaks; only applied when the exe matches the known build.
bool game_verified();
void game_on_display_probe();
void game_log_settings(const char* when);
void game_dump_layout();

void debug_start();
