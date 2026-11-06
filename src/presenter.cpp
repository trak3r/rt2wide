#include "ddraw_impl.h"
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

static CRITICAL_SECTION g_cs;
static HWND g_hwnd;
static WNDPROC g_orig_proc;
static bool g_borderless;
static RECT g_monitor;
static Surface* g_primary;
static volatile LONG g_lw, g_lh;
static volatile LONG g_dirty;
static volatile LONG g_quit;
static HANDLE g_wake;
static HANDLE g_thread;

void presenter_init() {
    InitializeCriticalSection(&g_cs);
    g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
}

void presenter_lock() { EnterCriticalSection(&g_cs); }
void presenter_unlock() { LeaveCriticalSection(&g_cs); }

void presenter_mark_dirty() {
    InterlockedExchange(&g_dirty, 1);
    if (g_wake) SetEvent(g_wake);
}

void presenter_set_logical(int w, int h) {
    if (w == g_lw && h == g_lh) return;
    InterlockedExchange(&g_lw, w);
    InterlockedExchange(&g_lh, h);
    log_line("Presenter: logical size %dx%d", w, h);
    presenter_mark_dirty();
}

void presenter_wide_size(int* w, int* h) {
    MONITORINFO mi = {sizeof(mi)};
    HMONITOR mon = g_hwnd ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY)
                          : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoA(mon, &mi);
    int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    *h = g_cfg.wide_h;
    if (g_cfg.wide_w) *w = g_cfg.wide_w;
    else *w = mh > 0 ? (int)((int64_t)g_cfg.wide_h * mw / mh) & ~7 : 1024;
    if (*w < 1024) *w = 1024;
}

void presenter_set_primary(Surface* s) {
    presenter_lock();
    g_primary = s;
    presenter_unlock();
    log_line("Presenter: primary is %dx%dx%d", s->w, s->h, s->bpp);
    presenter_set_logical(s->w, s->h);
    presenter_mark_dirty();
}

void presenter_clear_primary(Surface* s) {
    presenter_lock();
    if (g_primary == s) g_primary = nullptr;
    presenter_unlock();
}

static Viewport compute_viewport(int cw, int ch, int lw, int lh) {
    Viewport v = {0, 0, cw, ch, lw, lh};
    if (g_cfg.scaling == Scaling::Stretch || lw <= 0 || lh <= 0) return v;
    double sx = (double)cw / lw, sy = (double)ch / lh;
    double s = sx < sy ? sx : sy;
    if (g_cfg.scaling == Scaling::Integer) {
        s = (double)(int)s;
        if (s < 1) s = 1;
    }
    v.w = (int)(lw * s + 0.5);
    v.h = (int)(lh * s + 0.5);
    v.x = (cw - v.w) / 2;
    v.y = (ch - v.h) / 2;
    return v;
}

bool presenter_viewport(Viewport* out) {
    HWND hwnd = g_hwnd;
    int lw = g_lw, lh = g_lh;
    if (!hwnd || !lw || !lh) return false;
    RECT rc;
    if (!GetClientRect(hwnd, &rc)) return false;
    *out = compute_viewport(rc.right - rc.left, rc.bottom - rc.top, lw, lh);
    return true;
}

bool presenter_owns(HWND hwnd, int* lw, int* lh) {
    if (!g_borderless || !hwnd || hwnd != g_hwnd || !g_lw) return false;
    if (lw) *lw = g_lw;
    if (lh) *lh = g_lh;
    return true;
}

bool presenter_map_client(POINT* pt) {
    if (!g_borderless) return false;
    Viewport v;
    if (!presenter_viewport(&v) || v.w <= 0 || v.h <= 0) return false;
    int x = (int)((int64_t)(pt->x - v.x) * v.lw / v.w);
    int y = (int)((int64_t)(pt->y - v.y) * v.lh / v.h);
    pt->x = x < 0 ? 0 : (x >= v.lw ? v.lw - 1 : x);
    pt->y = y < 0 ? 0 : (y >= v.lh ? v.lh - 1 : y);
    return true;
}

bool presenter_map_screen(POINT* pt) {
    if (!g_borderless || !g_hwnd) return false;
    POINT p = *pt;
    if (!ScreenToClient(g_hwnd, &p)) return false;
    if (!presenter_map_client(&p)) return false;
    *pt = p;
    return true;
}

bool presenter_unmap_client(POINT* pt) {
    Viewport v;
    if (!presenter_viewport(&v) || v.lw <= 0 || v.lh <= 0) return false;
    pt->x = v.x + (int)(((int64_t)pt->x * 2 + 1) * v.w / (2 * v.lw));
    pt->y = v.y + (int)(((int64_t)pt->y * 2 + 1) * v.h / (2 * v.lh));
    return true;
}

HWND presenter_window() { return g_hwnd; }

// ---------------------------------------------------------------------------
// Debug frame dumps (BMP files in [debug] dump_dir)

static volatile LONG g_dump_now;
static int g_dumps;

void presenter_request_dump() {
    InterlockedExchange(&g_dump_now, 1);
    presenter_mark_dirty();
}

// ---------------------------------------------------------------------------
// Rendering thread

struct Frame {
    uint8_t* pixels = nullptr;
    size_t capacity = 0;
    int w = 0, h = 0, bpp = 0, pitch = 0;
    PALETTEENTRY pal[256];
};

static bool snapshot(Frame* f) {
    presenter_lock();
    Surface* p = g_primary;
    if (!p || !p->bits) { presenter_unlock(); return false; }
    size_t size = (size_t)p->pitch * p->h;
    if (size > f->capacity) {
        free(f->pixels);
        f->pixels = (uint8_t*)malloc(size);
        f->capacity = f->pixels ? size : 0;
        if (!f->pixels) { presenter_unlock(); return false; }
    }
    memcpy(f->pixels, p->bits, size);
    f->w = p->w; f->h = p->h; f->bpp = p->bpp; f->pitch = p->pitch;
    if (p->bpp == 8) {
        if (p->palette) memcpy(f->pal, p->palette->entries, sizeof(f->pal));
        else for (int i = 0; i < 256; i++) f->pal[i] = PALETTEENTRY{(BYTE)i, (BYTE)i, (BYTE)i, 0};
    }
    presenter_unlock();
    return true;
}

static void dump_frame(const Frame& f) {
    if (!g_cfg.dump_dir[0] || g_dumps >= g_cfg.dump_max) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\frame_%03d_%dx%d.bmp", g_cfg.dump_dir, g_dumps++, f.w, f.h);
    int row = (f.w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + row * f.h;
    ih.biSize = sizeof(ih);
    ih.biWidth = f.w;
    ih.biHeight = -f.h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    FILE* out = fopen(path, "wb");
    if (!out) { log_line("Dump: cannot write %s", path); return; }
    fwrite(&fh, sizeof(fh), 1, out);
    fwrite(&ih, sizeof(ih), 1, out);
    uint8_t* line = (uint8_t*)calloc(row, 1);
    for (int y = 0; y < f.h && line; y++) {
        const uint8_t* src = f.pixels + (size_t)y * f.pitch;
        for (int x = 0; x < f.w; x++) {
            uint8_t r, g, b;
            if (f.bpp == 16) {
                uint16_t p = ((const uint16_t*)src)[x];
                r = (uint8_t)(((p >> 11) & 31) * 255 / 31);
                g = (uint8_t)(((p >> 5) & 63) * 255 / 63);
                b = (uint8_t)((p & 31) * 255 / 31);
            } else if (f.bpp == 32) {
                b = src[x * 4]; g = src[x * 4 + 1]; r = src[x * 4 + 2];
            } else {
                const PALETTEENTRY& e = f.pal[src[x]];
                r = e.peRed; g = e.peGreen; b = e.peBlue;
            }
            line[x * 3] = b; line[x * 3 + 1] = g; line[x * 3 + 2] = r;
        }
        fwrite(line, row, 1, out);
    }
    free(line);
    fclose(out);
    log_line("Dump: wrote %s", path);
}

struct GlState {
    HGLRC ctx = nullptr;
    GLuint tex = 0;
    int tw = 0, th = 0, tbpp = 0;
    uint32_t* rgba = nullptr;
    size_t rgba_cap = 0;
};

static bool gl_init(HDC hdc, GlState* gl) {
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;
    if (GetPixelFormat(hdc) == 0) {
        int fmt = ChoosePixelFormat(hdc, &pfd);
        if (!fmt || !SetPixelFormat(hdc, fmt, &pfd)) {
            log_line("Presenter: SetPixelFormat failed (%lu)", GetLastError());
            return false;
        }
    }
    gl->ctx = wglCreateContext(hdc);
    if (!gl->ctx || !wglMakeCurrent(hdc, gl->ctx)) {
        log_line("Presenter: OpenGL context creation failed (%lu)", GetLastError());
        if (gl->ctx) wglDeleteContext(gl->ctx);
        gl->ctx = nullptr;
        return false;
    }
    typedef BOOL(WINAPI * SwapIntervalFn)(int);
    SwapIntervalFn swap_interval = (SwapIntervalFn)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap_interval) swap_interval(g_cfg.vsync ? 1 : 0);
    log_line("Presenter: OpenGL %s | %s | %s", glGetString(GL_VERSION), glGetString(GL_VENDOR), glGetString(GL_RENDERER));
    glGenTextures(1, &gl->tex);
    return true;
}

static void gl_shutdown(GlState* gl) {
    if (gl->ctx) {
        if (gl->tex) glDeleteTextures(1, &gl->tex);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(gl->ctx);
    }
    free(gl->rgba);
    *gl = GlState();
}

static void gl_draw(HDC hdc, GlState* gl, const Frame& f, int cw, int ch, const Viewport& v) {
    glBindTexture(GL_TEXTURE_2D, gl->tex);
    bool realloc_tex = gl->tw != f.w || gl->th != f.h || gl->tbpp != f.bpp;
    if (realloc_tex) {
        GLint filter = g_cfg.linear_filter ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (f.bpp == 16) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, f.w, f.h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, nullptr);
        else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f.w, f.h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);
        gl->tw = f.w; gl->th = f.h; gl->tbpp = f.bpp;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (f.bpp == 16) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, f.pitch / 2);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.w, f.h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, f.pixels);
    } else if (f.bpp == 32) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, f.pitch / 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.w, f.h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, f.pixels);
    } else {
        size_t need = (size_t)f.w * f.h;
        if (need > gl->rgba_cap) {
            free(gl->rgba);
            gl->rgba = (uint32_t*)malloc(need * 4);
            gl->rgba_cap = gl->rgba ? need : 0;
        }
        if (gl->rgba) {
            uint32_t lut[256];
            for (int i = 0; i < 256; i++)
                lut[i] = 0xFF000000u | (f.pal[i].peRed << 16) | (f.pal[i].peGreen << 8) | f.pal[i].peBlue;
            for (int y = 0; y < f.h; y++) {
                const uint8_t* src = f.pixels + (size_t)y * f.pitch;
                uint32_t* dst = gl->rgba + (size_t)y * f.w;
                for (int x = 0; x < f.w; x++) dst[x] = lut[src[x]];
            }
            glPixelStorei(GL_UNPACK_ROW_LENGTH, f.w);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.w, f.h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, gl->rgba);
        }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    glViewport(0, 0, cw, ch);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, cw, ch, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2i(v.x, v.y);
    glTexCoord2f(1, 0); glVertex2i(v.x + v.w, v.y);
    glTexCoord2f(1, 1); glVertex2i(v.x + v.w, v.y + v.h);
    glTexCoord2f(0, 1); glVertex2i(v.x, v.y + v.h);
    glEnd();
    SwapBuffers(hdc);
}

static void gdi_draw(HDC hdc, const Frame& f, int cw, int ch, const Viewport& v) {
    struct { BITMAPINFOHEADER h; RGBQUAD colors[256]; } bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.h.biSize = sizeof(BITMAPINFOHEADER);
    bmi.h.biWidth = f.pitch / (f.bpp / 8);
    bmi.h.biHeight = -f.h;
    bmi.h.biPlanes = 1;
    bmi.h.biBitCount = (WORD)f.bpp;
    bmi.h.biCompression = f.bpp == 16 ? BI_BITFIELDS : BI_RGB;
    if (f.bpp == 16) {
        DWORD* m = (DWORD*)bmi.colors;
        m[0] = 0xF800; m[1] = 0x07E0; m[2] = 0x001F;
    } else if (f.bpp == 8) {
        for (int i = 0; i < 256; i++)
            bmi.colors[i] = RGBQUAD{f.pal[i].peBlue, f.pal[i].peGreen, f.pal[i].peRed, 0};
    }
    if (g_cfg.linear_filter) { SetStretchBltMode(hdc, HALFTONE); SetBrushOrgEx(hdc, 0, 0, nullptr); }
    else SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc, v.x, v.y, v.w, v.h, 0, 0, f.w, f.h, f.pixels, (BITMAPINFO*)&bmi, DIB_RGB_COLORS, SRCCOPY);
    if (v.x > 0) { PatBlt(hdc, 0, 0, v.x, ch, BLACKNESS); PatBlt(hdc, v.x + v.w, 0, cw - v.x - v.w, ch, BLACKNESS); }
    if (v.y > 0) { PatBlt(hdc, 0, 0, cw, v.y, BLACKNESS); PatBlt(hdc, 0, v.y + v.h, cw, ch - v.y - v.h, BLACKNESS); }
}

static DWORD WINAPI render_thread(void*) {
    HWND hwnd = g_hwnd;
    HDC hdc = GetDC(hwnd);
    GlState gl;
    bool use_gl = g_cfg.renderer == Renderer::OpenGL && gl_init(hdc, &gl);
    log_line("Presenter: rendering with %s, scaling=%d, linear=%d", use_gl ? "OpenGL" : "GDI", (int)g_cfg.scaling,
             (int)g_cfg.linear_filter);
    Frame frame;
    DWORD last = 0, last_dump = GetTickCount();
    while (!g_quit) {
        WaitForSingleObject(g_wake, 16);
        if (g_quit) break;
        bool dirty = InterlockedExchange(&g_dirty, 0) != 0;
        DWORD now = GetTickCount();
        if (!dirty && now - last < 500) continue;
        if (!snapshot(&frame)) continue;
        bool timed = g_cfg.dump_every_ms > 0 && now - last_dump >= (DWORD)g_cfg.dump_every_ms;
        if (InterlockedExchange(&g_dump_now, 0) || timed) {
            dump_frame(frame);
            last_dump = now;
        }
        RECT rc;
        GetClientRect(hwnd, &rc);
        int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
        if (cw <= 0 || ch <= 0) continue;
        Viewport v = compute_viewport(cw, ch, frame.w, frame.h);
        TRACE("Presenter::frame", "%dx%dx%d -> client %dx%d, image at (%d,%d) %dx%d", frame.w, frame.h, frame.bpp, cw,
              ch, v.x, v.y, v.w, v.h);
        if (use_gl) gl_draw(hdc, &gl, frame, cw, ch, v);
        else gdi_draw(hdc, frame, cw, ch, v);
        last = now;
    }
    if (use_gl) gl_shutdown(&gl);
    ReleaseDC(hwnd, hdc);
    free(frame.pixels);
    log_line("Presenter: render thread stopped");
    return 0;
}

static void stop_thread() {
    if (!g_thread) return;
    InterlockedExchange(&g_quit, 1);
    SetEvent(g_wake);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
}

void presenter_shutdown() { stop_thread(); }

// ---------------------------------------------------------------------------
// Window

static const LONG kFrameStyles = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME;
static const LONG kFrameExStyles = WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE;

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_WINDOWPOSCHANGING:
        if (g_borderless && !IsIconic(hwnd)) {
            WINDOWPOS* pos = (WINDOWPOS*)lp;
            if (pos->x > -32000) {
                pos->x = g_monitor.left;
                pos->y = g_monitor.top;
                pos->cx = g_monitor.right - g_monitor.left;
                pos->cy = g_monitor.bottom - g_monitor.top;
                pos->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
            }
        }
        break;
    case WM_STYLECHANGING:
        if (g_borderless) {
            STYLESTRUCT* ss = (STYLESTRUCT*)lp;
            if (wp == (WPARAM)GWL_STYLE) ss->styleNew = (ss->styleNew & ~kFrameStyles) | WS_POPUP;
            else if (wp == (WPARAM)GWL_EXSTYLE) ss->styleNew &= ~kFrameExStyles;
        }
        break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK: {
        POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
        if (presenter_map_client(&pt)) lp = MAKELPARAM(pt.x, pt.y);
        break;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
        if (presenter_map_screen(&pt)) lp = MAKELPARAM(pt.x, pt.y);
        break;
    }
    case WM_ERASEBKGND:
        if (g_borderless) return 1;
        break;
    case WM_SIZE:
        if (g_borderless && wp != SIZE_MINIMIZED && g_lw) {
            TRACE("Presenter WM_SIZE", "%dx%d reported as %ldx%ld", LOWORD(lp), HIWORD(lp), g_lw, g_lh);
            lp = MAKELPARAM(g_lw, g_lh);
        }
        presenter_mark_dirty();
        break;
    case WM_PAINT:
    case WM_ACTIVATEAPP:
    case WM_DISPLAYCHANGE:
        presenter_mark_dirty();
        break;
    case WM_DESTROY: {
        log_line("Presenter: game window destroyed");
        stop_thread();
        g_borderless = false;
        WNDPROC orig = g_orig_proc;
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)orig);
        g_hwnd = nullptr;
        return CallWindowProcA(orig, hwnd, msg, wp, lp);
    }
    }
    return CallWindowProcA(g_orig_proc, hwnd, msg, wp, lp);
}

static void make_borderless(HWND hwnd) {
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    g_monitor = mi.rcMonitor;
    if (GetMenu(hwnd)) {
        log_line("Presenter: removing window menu");
        SetMenu(hwnd, nullptr);
    }
    g_borderless = true;
    SetWindowLongA(hwnd, GWL_STYLE, (GetWindowLongA(hwnd, GWL_STYLE) & ~kFrameStyles) | WS_POPUP | WS_VISIBLE);
    SetWindowLongA(hwnd, GWL_EXSTYLE, GetWindowLongA(hwnd, GWL_EXSTYLE) & ~kFrameExStyles);
    SetWindowPos(hwnd, HWND_TOP, g_monitor.left, g_monitor.top, g_monitor.right - g_monitor.left,
                 g_monitor.bottom - g_monitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    log_line("Presenter: window %p now borderless at (%ld,%ld) %ldx%ld", hwnd, g_monitor.left, g_monitor.top,
             g_monitor.right - g_monitor.left, g_monitor.bottom - g_monitor.top);
}

void presenter_attach_window(HWND hwnd) {
    if (g_hwnd != hwnd) {
        if (g_hwnd) {
            stop_thread();
            SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_orig_proc);
        }
        g_hwnd = hwnd;
        g_orig_proc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)wndproc);
        log_line("Presenter: attached to window %p", hwnd);
        debug_start();
    }
    make_borderless(hwnd);
    if (!g_thread) {
        InterlockedExchange(&g_quit, 0);
        g_thread = CreateThread(nullptr, 0, render_thread, nullptr, 0, nullptr);
    }
    presenter_mark_dirty();
}
