#include "ddraw_impl.h"
#include <GL/gl.h>
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

void presenter_set_primary(Surface* s) {
    presenter_lock();
    g_primary = s;
    InterlockedExchange(&g_lw, s->w);
    InterlockedExchange(&g_lh, s->h);
    presenter_unlock();
    log_line("Presenter: primary is %dx%dx%d", s->w, s->h, s->bpp);
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
    DWORD last = 0;
    while (!g_quit) {
        WaitForSingleObject(g_wake, 16);
        if (g_quit) break;
        bool dirty = InterlockedExchange(&g_dirty, 0) != 0;
        DWORD now = GetTickCount();
        if (!dirty && now - last < 500) continue;
        if (!snapshot(&frame)) continue;
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
    case WM_PAINT:
    case WM_SIZE:
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
    }
    make_borderless(hwnd);
    if (!g_thread) {
        InterlockedExchange(&g_quit, 0);
        g_thread = CreateThread(nullptr, 0, render_thread, nullptr, 0, nullptr);
    }
    presenter_mark_dirty();
}
