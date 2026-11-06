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
    bool log = true;
    int trace_limit = 25;
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

void hooks_install();
