#include "ddraw_impl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char* guid_str(REFIID g) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

static const char* rect_str(const RECT* r) {
    static char buf[4][64];
    static int i;
    char* b = buf[i++ & 3];
    if (!r) return "null";
    snprintf(b, 64, "(%ld,%ld)-(%ld,%ld)", r->left, r->top, r->right, r->bottom);
    return b;
}

static Surface* as_surface(void* p) { return reinterpret_cast<Surface*>(static_cast<IDirectDrawSurface3*>(p)); }

// ---------------------------------------------------------------------------
// Palette

Palette::Palette(DWORD caps, const PALETTEENTRY* e) : caps_(caps) {
    memset(entries, 0, sizeof(entries));
    if (e) memcpy(entries, e, (caps & DDPCAPS_8BIT) || !(caps & (DDPCAPS_4BIT | DDPCAPS_2BIT | DDPCAPS_1BIT)) ? sizeof(entries) : 16 * sizeof(PALETTEENTRY));
}

HRESULT Palette::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IDirectDrawPalette) { *ppv = this; AddRef(); return S_OK; }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG Palette::AddRef() { return InterlockedIncrement(&ref_); }
ULONG Palette::Release() {
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return r;
}
HRESULT Palette::GetCaps(LPDWORD caps) { if (caps) *caps = caps_; return DD_OK; }
HRESULT Palette::GetEntries(DWORD, DWORD base, DWORD count, LPPALETTEENTRY e) {
    if (!e || base >= 256) return DDERR_INVALIDPARAMS;
    if (base + count > 256) count = 256 - base;
    memcpy(e, entries + base, count * sizeof(PALETTEENTRY));
    return DD_OK;
}
HRESULT Palette::Initialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) { return DDERR_ALREADYINITIALIZED; }
HRESULT Palette::SetEntries(DWORD, DWORD start, DWORD count, LPPALETTEENTRY e) {
    TRACE("Palette::SetEntries", "start=%lu count=%lu", start, count);
    if (!e || start >= 256) return DDERR_INVALIDPARAMS;
    if (start + count > 256) count = 256 - start;
    memcpy(entries + start, e, count * sizeof(PALETTEENTRY));
    if (on_primary) presenter_mark_dirty();
    return DD_OK;
}

// ---------------------------------------------------------------------------
// Clipper

HRESULT Clipper::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IDirectDrawClipper) { *ppv = this; AddRef(); return S_OK; }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG Clipper::AddRef() { return InterlockedIncrement(&ref_); }
ULONG Clipper::Release() {
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return r;
}
HRESULT Clipper::GetClipList(LPRECT, LPRGNDATA list, LPDWORD size) {
    TRACE("Clipper::GetClipList", "");
    if (!size) return DDERR_INVALIDPARAMS;
    DWORD need = sizeof(RGNDATAHEADER) + sizeof(RECT);
    if (!list) { *size = need; return DD_OK; }
    if (*size < need) { *size = need; return DDERR_REGIONTOOSMALL; }
    Viewport vp;
    int lw = 1024, lh = 768;
    if (presenter_viewport(&vp)) { lw = vp.lw; lh = vp.lh; }
    list->rdh.dwSize = sizeof(RGNDATAHEADER);
    list->rdh.iType = RDH_RECTANGLES;
    list->rdh.nCount = 1;
    list->rdh.nRgnSize = sizeof(RECT);
    SetRect(&list->rdh.rcBound, 0, 0, lw, lh);
    memcpy(list->Buffer, &list->rdh.rcBound, sizeof(RECT));
    return DD_OK;
}
HRESULT Clipper::GetHWnd(HWND* hwnd) { if (hwnd) *hwnd = hwnd_; return DD_OK; }
HRESULT Clipper::Initialize(LPDIRECTDRAW, DWORD) { return DDERR_ALREADYINITIALIZED; }
HRESULT Clipper::IsClipListChanged(WINBOOL* changed) { if (changed) *changed = FALSE; return DD_OK; }
HRESULT Clipper::SetClipList(LPRGNDATA, DWORD) { TRACE("Clipper::SetClipList", ""); return DD_OK; }
HRESULT Clipper::SetHWnd(DWORD, HWND hwnd) { TRACE("Clipper::SetHWnd", "hwnd=%p", hwnd); hwnd_ = hwnd; return DD_OK; }

// ---------------------------------------------------------------------------
// Surface

Surface::Surface(DDraw* d, int width, int height, int depth, DWORD c)
    : dd(d), w(width), h(height), bpp(depth), caps(c) {
    pitch = ((w * bpp + 31) / 32) * 4;
    DWORD size = (DWORD)pitch * h;
    if (size == 0) size = 4;
    section = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, nullptr);
    if (section) bits = (uint8_t*)MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!bits) log_line("Surface: failed to allocate %dx%dx%d (error %lu)", w, h, bpp, GetLastError());
}

Surface::~Surface() {
    presenter_clear_primary(this);
    if (dd && dd->primary == this) dd->primary = nullptr;
    if (back) back->Release();
    if (palette) { if (caps & DDSCAPS_PRIMARYSURFACE) palette->on_primary = false; palette->Release(); }
    if (clipper) clipper->Release();
    if (dc) DeleteDC(dc);
    if (dib) DeleteObject(dib);
    if (bits) UnmapViewOfFile(bits);
    if (section) CloseHandle(section);
}

bool Surface::is_visible() const { return (caps & DDSCAPS_PRIMARYSURFACE) != 0; }

void Surface::swap_buffers_with(Surface* o) {
    uint8_t* b = bits; bits = o->bits; o->bits = b;
    HANDLE s = section; section = o->section; o->section = s;
    HBITMAP d = dib; dib = o->dib; o->dib = d;
    HDC c = dc; dc = o->dc; o->dc = c;
}

void Surface::fill_pixel_format(DDPIXELFORMAT* pf) const { pixel_format_for(bpp, pf); }

void pixel_format_for(int bpp, DDPIXELFORMAT* pf) {
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(*pf);
    pf->dwFlags = DDPF_RGB;
    pf->dwRGBBitCount = bpp;
    if (bpp == 8) {
        pf->dwFlags |= DDPF_PALETTEINDEXED8;
    } else if (bpp == 16) {
        pf->dwRBitMask = 0xF800; pf->dwGBitMask = 0x07E0; pf->dwBBitMask = 0x001F;
    } else if (bpp == 32) {
        pf->dwRBitMask = 0xFF0000; pf->dwGBitMask = 0x00FF00; pf->dwBBitMask = 0x0000FF;
    }
}

HRESULT Surface::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IDirectDrawSurface || riid == IID_IDirectDrawSurface2 ||
        riid == IID_IDirectDrawSurface3) {
        *ppv = this; AddRef(); return S_OK;
    }
    log_line("Surface::QueryInterface unsupported %s", guid_str(riid));
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG Surface::AddRef() { return InterlockedIncrement(&ref_); }
ULONG Surface::Release() {
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) {
        TRACE("Surface::destroy", "%p %dx%dx%d caps=%08lX", this, w, h, bpp, caps);
        delete this;
    }
    return r;
}

HRESULT Surface::AddAttachedSurface(LPDIRECTDRAWSURFACE3 s) {
    TRACE("Surface::AddAttachedSurface", "%p <- %p", this, s);
    if (!s) return DDERR_INVALIDPARAMS;
    Surface* o = as_surface(s);
    if (!back && (o->caps & DDSCAPS_BACKBUFFER)) { o->AddRef(); back = o; return DD_OK; }
    return DDERR_CANNOTATTACHSURFACE;
}
HRESULT Surface::AddOverlayDirtyRect(LPRECT) { return DDERR_UNSUPPORTED; }

template <typename T>
static void fill_rect_t(uint8_t* bits, int pitch, int x0, int y0, int cw, int ch, T v) {
    for (int y = 0; y < ch; y++) {
        T* row = (T*)(bits + (size_t)(y0 + y) * pitch) + x0;
        for (int x = 0; x < cw; x++) row[x] = v;
    }
}

void Surface::fill(const RECT& r, DWORD color) {
    int x0 = r.left < 0 ? 0 : r.left, y0 = r.top < 0 ? 0 : r.top;
    int x1 = r.right > w ? w : r.right, y1 = r.bottom > h ? h : r.bottom;
    if (x1 <= x0 || y1 <= y0) return;
    switch (bpp) {
    case 8: fill_rect_t<uint8_t>(bits, pitch, x0, y0, x1 - x0, y1 - y0, (uint8_t)color); break;
    case 16: fill_rect_t<uint16_t>(bits, pitch, x0, y0, x1 - x0, y1 - y0, (uint16_t)color); break;
    case 32: fill_rect_t<uint32_t>(bits, pitch, x0, y0, x1 - x0, y1 - y0, (uint32_t)color); break;
    }
}

template <typename T>
static void copy_rect_t(uint8_t* dbits, int dpitch, const uint8_t* sbits, int spitch, int dx, int dy, int sx, int sy,
                        int cw, int ch, bool keyed, T key) {
    for (int y = 0; y < ch; y++) {
        T* d = (T*)(dbits + (size_t)(dy + y) * dpitch) + dx;
        const T* s = (const T*)(sbits + (size_t)(sy + y) * spitch) + sx;
        if (!keyed) {
            memcpy(d, s, cw * sizeof(T));
        } else {
            for (int x = 0; x < cw; x++)
                if (s[x] != key) d[x] = s[x];
        }
    }
}

template <typename T>
static void stretch_rect_t(uint8_t* dbits, int dpitch, int dw_total, int dh_total, const uint8_t* sbits, int spitch,
                           int sw_total, int sh_total, RECT d, RECT s, bool keyed, T key) {
    int dw = d.right - d.left, dh = d.bottom - d.top;
    int sw = s.right - s.left, sh = s.bottom - s.top;
    int y0 = d.top < 0 ? 0 : d.top, y1 = d.bottom > dh_total ? dh_total : d.bottom;
    int x0 = d.left < 0 ? 0 : d.left, x1 = d.right > dw_total ? dw_total : d.right;
    for (int y = y0; y < y1; y++) {
        int sy = s.top + (int)((int64_t)(y - d.top) * sh / dh);
        if (sy < 0 || sy >= sh_total) continue;
        T* drow = (T*)(dbits + (size_t)y * dpitch);
        const T* srow = (const T*)(sbits + (size_t)sy * spitch);
        for (int x = x0; x < x1; x++) {
            int sx = s.left + (int)((int64_t)(x - d.left) * sw / dw);
            if (sx < 0 || sx >= sw_total) continue;
            T v = srow[sx];
            if (!keyed || v != key) drow[x] = v;
        }
    }
}

void Surface::copy_from(Surface* src, RECT d, RECT s, bool keyed, DWORD key) {
    int dw = d.right - d.left, dh = d.bottom - d.top;
    int sw = s.right - s.left, sh = s.bottom - s.top;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (src->bpp != bpp) {
        TRACE("Surface::copy_from bpp mismatch", "dst=%d src=%d", bpp, src->bpp);
        return;
    }

    const uint8_t* sbits = src->bits;
    uint8_t* temp = nullptr;
    if (src == this) {
        temp = (uint8_t*)malloc((size_t)pitch * h);
        if (!temp) return;
        memcpy(temp, bits, (size_t)pitch * h);
        sbits = temp;
    }

    if (dw == sw && dh == sh) {
        int dx = d.left, dy = d.top, sx = s.left, sy = s.top, cw = dw, ch = dh;
        if (dx < 0) { sx -= dx; cw += dx; dx = 0; }
        if (dy < 0) { sy -= dy; ch += dy; dy = 0; }
        if (sx < 0) { dx -= sx; cw += sx; sx = 0; }
        if (sy < 0) { dy -= sy; ch += sy; sy = 0; }
        if (dx + cw > w) cw = w - dx;
        if (dy + ch > h) ch = h - dy;
        if (sx + cw > src->w) cw = src->w - sx;
        if (sy + ch > src->h) ch = src->h - sy;
        if (cw > 0 && ch > 0) {
            switch (bpp) {
            case 8: copy_rect_t<uint8_t>(bits, pitch, sbits, src->pitch, dx, dy, sx, sy, cw, ch, keyed, (uint8_t)key); break;
            case 16: copy_rect_t<uint16_t>(bits, pitch, sbits, src->pitch, dx, dy, sx, sy, cw, ch, keyed, (uint16_t)key); break;
            case 32: copy_rect_t<uint32_t>(bits, pitch, sbits, src->pitch, dx, dy, sx, sy, cw, ch, keyed, (uint32_t)key); break;
            }
        }
    } else {
        switch (bpp) {
        case 8: stretch_rect_t<uint8_t>(bits, pitch, w, h, sbits, src->pitch, src->w, src->h, d, s, keyed, (uint8_t)key); break;
        case 16: stretch_rect_t<uint16_t>(bits, pitch, w, h, sbits, src->pitch, src->w, src->h, d, s, keyed, (uint16_t)key); break;
        case 32: stretch_rect_t<uint32_t>(bits, pitch, w, h, sbits, src->pitch, src->w, src->h, d, s, keyed, (uint32_t)key); break;
        }
    }
    free(temp);
}

HRESULT Surface::Blt(LPRECT dstRect, LPDIRECTDRAWSURFACE3 srcIface, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) {
    TRACE("Surface::Blt", "%p dst=%s src=%p srcRect=%s flags=%08lX", this, rect_str(dstRect), srcIface,
          rect_str(srcRect), flags);
    RECT d = dstRect ? *dstRect : RECT{0, 0, w, h};
    Surface* src = srcIface ? as_surface(srcIface) : nullptr;

    if (flags & DDBLT_COLORFILL) {
        if (!fx) return DDERR_INVALIDPARAMS;
        fill(d, fx->dwFillColor);
    } else if (flags & DDBLT_DEPTHFILL) {
        return DD_OK;
    } else if (!src) {
        if ((flags & DDBLT_ROP) && fx) {
            if (fx->dwROP == BLACKNESS) fill(d, 0);
            else if (fx->dwROP == WHITENESS) fill(d, 0xFFFFFFFF);
            else TRACE("Surface::Blt unsupported ROP", "%08lX", fx->dwROP);
        }
    } else {
        RECT s = srcRect ? *srcRect : RECT{0, 0, src->w, src->h};
        bool keyed = false;
        DWORD key = 0;
        if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) { keyed = true; key = fx->ddckSrcColorkey.dwColorSpaceLowValue; }
        else if ((flags & DDBLT_KEYSRC) && src->has_src_key) { keyed = true; key = src->src_key; }
        copy_from(src, d, s, keyed, key);
    }
    if (is_visible()) presenter_mark_dirty();
    return DD_OK;
}

HRESULT Surface::BltBatch(LPDDBLTBATCH, DWORD, DWORD) { TRACE("Surface::BltBatch UNIMPLEMENTED", ""); return DDERR_UNSUPPORTED; }

HRESULT Surface::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE3 srcIface, LPRECT srcRect, DWORD trans) {
    TRACE("Surface::BltFast", "%p at (%lu,%lu) src=%p srcRect=%s trans=%08lX", this, x, y, srcIface, rect_str(srcRect), trans);
    if (!srcIface) return DDERR_INVALIDPARAMS;
    Surface* src = as_surface(srcIface);
    RECT s = srcRect ? *srcRect : RECT{0, 0, src->w, src->h};
    RECT d = {(LONG)x, (LONG)y, (LONG)x + (s.right - s.left), (LONG)y + (s.bottom - s.top)};
    bool keyed = (trans & DDBLTFAST_SRCCOLORKEY) && src->has_src_key;
    copy_from(src, d, s, keyed, src->src_key);
    if (is_visible()) presenter_mark_dirty();
    return DD_OK;
}

HRESULT Surface::DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE3 s) {
    TRACE("Surface::DeleteAttachedSurface", "%p", s);
    if (back && (!s || as_surface(s) == back)) { back->Release(); back = nullptr; return DD_OK; }
    return DDERR_SURFACENOTATTACHED;
}

HRESULT Surface::EnumAttachedSurfaces(LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) {
    TRACE("Surface::EnumAttachedSurfaces", "");
    if (!cb) return DDERR_INVALIDPARAMS;
    if (back) {
        DDSURFACEDESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.dwSize = sizeof(desc);
        back->GetSurfaceDesc(&desc);
        back->AddRef();
        cb((LPDIRECTDRAWSURFACE)static_cast<IDirectDrawSurface3*>(back), &desc, ctx);
    }
    return DD_OK;
}
HRESULT Surface::EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) { return DDERR_UNSUPPORTED; }

HRESULT Surface::Flip(LPDIRECTDRAWSURFACE3 target, DWORD flags) {
    TRACE("Surface::Flip", "%p target=%p flags=%08lX", this, target, flags);
    Surface* other = target ? as_surface(target) : back;
    if (!other) return DDERR_NOTFLIPPABLE;
    presenter_lock();
    swap_buffers_with(other);
    presenter_unlock();
    presenter_mark_dirty();
    return DD_OK;
}

HRESULT Surface::GetAttachedSurface(LPDDSCAPS c, LPDIRECTDRAWSURFACE3* out) {
    TRACE("Surface::GetAttachedSurface", "%p caps=%08lX", this, c ? c->dwCaps : 0);
    if (!out) return DDERR_INVALIDPARAMS;
    if (back && c && (c->dwCaps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP))) {
        back->AddRef();
        *out = back;
        return DD_OK;
    }
    *out = nullptr;
    return DDERR_NOTFOUND;
}

HRESULT Surface::GetBltStatus(DWORD) { return DD_OK; }
HRESULT Surface::GetCaps(LPDDSCAPS c) { if (!c) return DDERR_INVALIDPARAMS; c->dwCaps = caps; return DD_OK; }
HRESULT Surface::GetClipper(LPDIRECTDRAWCLIPPER* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (!clipper) { *out = nullptr; return DDERR_NOCLIPPERATTACHED; }
    clipper->AddRef();
    *out = clipper;
    return DD_OK;
}
HRESULT Surface::GetColorKey(DWORD flags, LPDDCOLORKEY key) {
    if (!key) return DDERR_INVALIDPARAMS;
    if ((flags & DDCKEY_SRCBLT) && has_src_key) {
        key->dwColorSpaceLowValue = key->dwColorSpaceHighValue = src_key;
        return DD_OK;
    }
    return DDERR_NOCOLORKEY;
}

HRESULT Surface::GetDC(HDC* out) {
    TRACE("Surface::GetDC", "%p %dx%dx%d", this, w, h, bpp);
    if (!out) return DDERR_INVALIDPARAMS;
    if (!dib) {
        struct { BITMAPINFOHEADER h; RGBQUAD colors[256]; } bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.h.biSize = sizeof(BITMAPINFOHEADER);
        bmi.h.biWidth = w;
        bmi.h.biHeight = -h;
        bmi.h.biPlanes = 1;
        bmi.h.biBitCount = (WORD)bpp;
        bmi.h.biCompression = bpp == 16 ? BI_BITFIELDS : BI_RGB;
        if (bpp == 16) {
            DWORD* masks = (DWORD*)bmi.colors;
            masks[0] = 0xF800; masks[1] = 0x07E0; masks[2] = 0x001F;
        }
        void* ptr = nullptr;
        dib = CreateDIBSection(nullptr, (BITMAPINFO*)&bmi, DIB_RGB_COLORS, &ptr, section, 0);
        if (!dib) { log_line("Surface::GetDC CreateDIBSection failed (%lu)", GetLastError()); return DDERR_GENERIC; }
    }
    if (!dc) {
        dc = CreateCompatibleDC(nullptr);
        SelectObject(dc, dib);
    }
    if (bpp == 8) {
        Palette* p = palette ? palette : (dd && dd->primary ? dd->primary->palette : nullptr);
        if (p) {
            RGBQUAD q[256];
            for (int i = 0; i < 256; i++) {
                q[i].rgbRed = p->entries[i].peRed;
                q[i].rgbGreen = p->entries[i].peGreen;
                q[i].rgbBlue = p->entries[i].peBlue;
                q[i].rgbReserved = 0;
            }
            SetDIBColorTable(dc, 0, 256, q);
        }
    }
    *out = dc;
    return DD_OK;
}

HRESULT Surface::GetFlipStatus(DWORD) { return DD_OK; }
HRESULT Surface::GetOverlayPosition(LPLONG, LPLONG) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT Surface::GetPalette(LPDIRECTDRAWPALETTE* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (!palette) { *out = nullptr; return DDERR_NOPALETTEATTACHED; }
    palette->AddRef();
    *out = palette;
    return DD_OK;
}
HRESULT Surface::GetPixelFormat(LPDDPIXELFORMAT pf) {
    if (!pf) return DDERR_INVALIDPARAMS;
    fill_pixel_format(pf);
    return DD_OK;
}
HRESULT Surface::GetSurfaceDesc(LPDDSURFACEDESC desc) {
    if (!desc) return DDERR_INVALIDPARAMS;
    memset(desc, 0, sizeof(DDSURFACEDESC));
    desc->dwSize = sizeof(DDSURFACEDESC);
    desc->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    desc->dwWidth = w;
    desc->dwHeight = h;
    desc->lPitch = pitch;
    desc->ddsCaps.dwCaps = caps;
    if (back) { desc->dwFlags |= DDSD_BACKBUFFERCOUNT; desc->dwBackBufferCount = 1; }
    fill_pixel_format(&desc->ddpfPixelFormat);
    return DD_OK;
}
HRESULT Surface::Initialize(LPDIRECTDRAW, LPDDSURFACEDESC) { return DDERR_ALREADYINITIALIZED; }
HRESULT Surface::IsLost() { return DD_OK; }

HRESULT Surface::Lock(LPRECT rect, LPDDSURFACEDESC desc, DWORD flags, HANDLE) {
    TRACE("Surface::Lock", "%p rect=%s flags=%08lX %dx%dx%d", this, rect_str(rect), flags, w, h, bpp);
    if (!desc) return DDERR_INVALIDPARAMS;
    GetSurfaceDesc(desc);
    desc->dwFlags |= DDSD_LPSURFACE;
    uint8_t* p = bits;
    if (rect) p += (size_t)rect->top * pitch + rect->left * (bpp / 8);
    desc->lpSurface = p;
    return DD_OK;
}

HRESULT Surface::ReleaseDC(HDC) {
    GdiFlush();
    if (is_visible()) presenter_mark_dirty();
    return DD_OK;
}
HRESULT Surface::Restore() { return DD_OK; }
HRESULT Surface::SetClipper(LPDIRECTDRAWCLIPPER c) {
    TRACE("Surface::SetClipper", "%p clipper=%p", this, c);
    if (c) c->AddRef();
    if (clipper) clipper->Release();
    clipper = static_cast<Clipper*>(c);
    return DD_OK;
}
HRESULT Surface::SetColorKey(DWORD flags, LPDDCOLORKEY key) {
    TRACE("Surface::SetColorKey", "%p flags=%08lX key=%08lX", this, flags, key ? key->dwColorSpaceLowValue : 0);
    if (flags & DDCKEY_SRCBLT) {
        has_src_key = key != nullptr;
        src_key = key ? key->dwColorSpaceLowValue : 0;
    }
    return DD_OK;
}
HRESULT Surface::SetOverlayPosition(LONG, LONG) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT Surface::SetPalette(LPDIRECTDRAWPALETTE p) {
    TRACE("Surface::SetPalette", "%p palette=%p", this, p);
    Palette* np = static_cast<Palette*>(p);
    if (np) np->AddRef();
    if (palette) { if (is_visible()) palette->on_primary = false; palette->Release(); }
    palette = np;
    if (palette && is_visible()) { palette->on_primary = true; presenter_mark_dirty(); }
    return DD_OK;
}
HRESULT Surface::Unlock(LPVOID) {
    if (is_visible()) presenter_mark_dirty();
    return DD_OK;
}
HRESULT Surface::UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE3, LPRECT, DWORD, LPDDOVERLAYFX) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT Surface::UpdateOverlayDisplay(DWORD) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT Surface::UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE3) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT Surface::GetDDInterface(LPVOID* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    dd->v1.AddRef();
    *out = static_cast<IDirectDraw*>(&dd->v1);
    return DD_OK;
}
HRESULT Surface::PageLock(DWORD) { return DD_OK; }
HRESULT Surface::PageUnlock(DWORD) { return DD_OK; }
HRESULT Surface::SetSurfaceDesc(LPDDSURFACEDESC, DWORD) { TRACE("Surface::SetSurfaceDesc UNIMPLEMENTED", ""); return DDERR_UNSUPPORTED; }

// ---------------------------------------------------------------------------
// DirectDraw

HRESULT DDraw::QueryInterface(REFIID riid, void** ppv) {
    TRACE("DDraw::QueryInterface", "%s", guid_str(riid));
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirectDraw) { *ppv = static_cast<IDirectDraw*>(&v1); AddRef(); return S_OK; }
    if (riid == IID_IDirectDraw2) { *ppv = static_cast<IDirectDraw2*>(this); AddRef(); return S_OK; }
    log_line("DDraw::QueryInterface unsupported %s", guid_str(riid));
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG DDraw::AddRef() { return InterlockedIncrement(&ref_); }
ULONG DDraw::Release() {
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) {
        log_line("DDraw released");
        trace_summary();
        presenter_shutdown();
        delete this;
    }
    return r;
}
HRESULT DDraw::Compact() { return DD_OK; }

HRESULT DDraw::CreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER* out, IUnknown*) {
    TRACE("DDraw::CreateClipper", "flags=%08lX", flags);
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new Clipper();
    return DD_OK;
}

HRESULT DDraw::CreatePalette(DWORD flags, LPPALETTEENTRY table, LPDIRECTDRAWPALETTE* out, IUnknown*) {
    TRACE("DDraw::CreatePalette", "flags=%08lX", flags);
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new Palette(flags, table);
    return DD_OK;
}

HRESULT DDraw::CreateSurface(LPDDSURFACEDESC desc, LPDIRECTDRAWSURFACE* out, IUnknown*) {
    if (!desc || !out) return DDERR_INVALIDPARAMS;
    DWORD caps = (desc->dwFlags & DDSD_CAPS) ? desc->ddsCaps.dwCaps : 0;
    log_line("DDraw::CreateSurface flags=%08lX caps=%08lX size=%lux%lu backbuffers=%lu pf.flags=%08lX pf.bpp=%lu",
             desc->dwFlags, caps, desc->dwWidth, desc->dwHeight, desc->dwBackBufferCount,
             desc->ddpfPixelFormat.dwFlags, desc->ddpfPixelFormat.dwRGBBitCount);

    int mbpp = mode_bpp ? mode_bpp : 16;
    Surface* s;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        int w, h;
        logical_size(&w, &h);
        DWORD pcaps = caps | DDSCAPS_VISIBLE | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
        bool flip = (desc->dwFlags & DDSD_BACKBUFFERCOUNT) && desc->dwBackBufferCount > 0;
        if (flip) pcaps |= DDSCAPS_FRONTBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
        s = new Surface(this, w, h, mbpp, pcaps);
        if (flip) {
            s->back = new Surface(this, w, h, mbpp,
                                  DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM);
            if (!s->back->ok()) { delete s; return DDERR_OUTOFMEMORY; }
        }
        if (!s->ok()) { delete s; return DDERR_OUTOFMEMORY; }
        primary = s;
        presenter_set_primary(s);
    } else {
        if (!(desc->dwFlags & DDSD_WIDTH) || !(desc->dwFlags & DDSD_HEIGHT)) return DDERR_INVALIDPARAMS;
        int bpp = mbpp;
        if (desc->dwFlags & DDSD_PIXELFORMAT) {
            const DDPIXELFORMAT& pf = desc->ddpfPixelFormat;
            int want = (pf.dwFlags & DDPF_PALETTEINDEXED8) ? 8 : (int)pf.dwRGBBitCount;
            if (want == 8 || want == 16 || want == 32) bpp = want;
            else log_line("DDraw::CreateSurface unsupported pixel format bpp=%d, using %d", want, bpp);
        }
        DWORD scaps = caps;
        if (!(scaps & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY))) scaps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
        if (!(scaps & (DDSCAPS_OFFSCREENPLAIN | DDSCAPS_TEXTURE | DDSCAPS_BACKBUFFER))) scaps |= DDSCAPS_OFFSCREENPLAIN;
        s = new Surface(this, desc->dwWidth, desc->dwHeight, bpp, scaps);
        if (!s->ok()) { delete s; return DDERR_OUTOFMEMORY; }
    }
    if (desc->dwFlags & DDSD_CKSRCBLT) {
        s->has_src_key = true;
        s->src_key = desc->ddckCKSrcBlt.dwColorSpaceLowValue;
    }
    *out = (LPDIRECTDRAWSURFACE)static_cast<IDirectDrawSurface3*>(s);
    log_line("  -> surface %p %dx%dx%d caps=%08lX", s, s->w, s->h, s->bpp, s->caps);
    return DD_OK;
}

HRESULT DDraw::DuplicateSurface(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE*) {
    TRACE("DDraw::DuplicateSurface UNIMPLEMENTED", "");
    return DDERR_UNSUPPORTED;
}

HRESULT DDraw::EnumDisplayModes(DWORD flags, LPDDSURFACEDESC filter, LPVOID ctx, LPDDENUMMODESCALLBACK cb) {
    TRACE("DDraw::EnumDisplayModes", "flags=%08lX", flags);
    if (!cb) return DDERR_INVALIDPARAMS;
    static const int sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}};
    static const int depths[] = {8, 16, 32};
    for (int d : depths) {
        for (auto& sz : sizes) {
            if (filter) {
                if ((filter->dwFlags & DDSD_WIDTH) && filter->dwWidth != (DWORD)sz[0]) continue;
                if ((filter->dwFlags & DDSD_HEIGHT) && filter->dwHeight != (DWORD)sz[1]) continue;
                if ((filter->dwFlags & DDSD_PIXELFORMAT) && filter->ddpfPixelFormat.dwRGBBitCount != (DWORD)d) continue;
            }
            DDSURFACEDESC desc;
            memset(&desc, 0, sizeof(desc));
            desc.dwSize = sizeof(desc);
            desc.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
            desc.dwWidth = sz[0];
            desc.dwHeight = sz[1];
            desc.lPitch = sz[0] * d / 8;
            desc.dwRefreshRate = 60;
            pixel_format_for(d, &desc.ddpfPixelFormat);
            if (cb(&desc, ctx) == DDENUMRET_CANCEL) return DD_OK;
        }
    }
    return DD_OK;
}

HRESULT DDraw::EnumSurfaces(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) {
    TRACE("DDraw::EnumSurfaces UNIMPLEMENTED", "");
    return DDERR_UNSUPPORTED;
}
HRESULT DDraw::FlipToGDISurface() { return DD_OK; }

HRESULT DDraw::GetCaps(LPDDCAPS drv, LPDDCAPS hel) {
    TRACE("DDraw::GetCaps", "drv.size=%lu hel.size=%lu", drv ? drv->dwSize : 0, hel ? hel->dwSize : 0);
    DDCAPS_DX7 caps;
    memset(&caps, 0, sizeof(caps));
    caps.dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_PALETTE | DDCAPS_CANBLTSYSMEM;
    caps.dwCKeyCaps = DDCKEYCAPS_SRCBLT;
    caps.dwPalCaps = DDPCAPS_8BIT | DDPCAPS_PRIMARYSURFACE;
    caps.dwVidMemTotal = caps.dwVidMemFree = 64 * 1024 * 1024;
    caps.ddsCaps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_OFFSCREENPLAIN |
                          DDSCAPS_PALETTE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY;
    LPDDCAPS outs[2] = {drv, hel};
    for (LPDDCAPS out : outs) {
        if (!out) continue;
        DWORD size = out->dwSize ? out->dwSize : sizeof(DDCAPS_DX3);
        if (size > sizeof(caps)) size = sizeof(caps);
        caps.dwSize = size;
        memcpy(out, &caps, size);
    }
    return DD_OK;
}

HRESULT DDraw::GetDisplayMode(LPDDSURFACEDESC desc) {
    if (!desc) return DDERR_INVALIDPARAMS;
    int w = mode_w, h = mode_h, bpp = mode_bpp;
    if (!w) { w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN); bpp = g_cfg.wide ? 16 : 32; }
    TRACE("DDraw::GetDisplayMode", "-> %dx%dx%d", w, h, bpp);
    memset(desc, 0, sizeof(DDSURFACEDESC));
    desc->dwSize = sizeof(DDSURFACEDESC);
    desc->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    desc->dwWidth = w;
    desc->dwHeight = h;
    desc->lPitch = w * bpp / 8;
    desc->dwRefreshRate = 60;
    pixel_format_for(bpp, &desc->ddpfPixelFormat);
    return DD_OK;
}

HRESULT DDraw::GetFourCCCodes(LPDWORD num, LPDWORD) { if (num) *num = 0; return DD_OK; }
HRESULT DDraw::GetGDISurface(LPDIRECTDRAWSURFACE* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (!primary) { *out = nullptr; return DDERR_NOTFOUND; }
    primary->AddRef();
    *out = (LPDIRECTDRAWSURFACE)static_cast<IDirectDrawSurface3*>(primary);
    return DD_OK;
}
HRESULT DDraw::GetMonitorFrequency(LPDWORD f) { if (f) *f = 60; return DD_OK; }
HRESULT DDraw::GetScanLine(LPDWORD line) { if (line) *line = 0; return DD_OK; }
HRESULT DDraw::GetVerticalBlankStatus(WINBOOL* in_vb) {
    static bool toggle;
    toggle = !toggle;
    if (in_vb) *in_vb = toggle;
    return DD_OK;
}
HRESULT DDraw::Initialize(GUID*) { return DDERR_ALREADYINITIALIZED; }
HRESULT DDraw::RestoreDisplayMode() { TRACE("DDraw::RestoreDisplayMode", ""); return DD_OK; }

void DDraw::logical_size(int* w, int* h) const {
    if (exclusive && mode_w) { *w = mode_w; *h = mode_h; }
    else if (!exclusive && g_cfg.wide) presenter_wide_size(w, h);
    else { *w = mode_w ? mode_w : 1024; *h = mode_h ? mode_h : 768; }
}

HRESULT DDraw::SetCooperativeLevel(HWND hwnd, DWORD flags) {
    TRACE("DDraw::SetCooperativeLevel", "hwnd=%p flags=%08lX", hwnd, flags);
    game_log_settings("SetCooperativeLevel");
    exclusive = (flags & DDSCL_FULLSCREEN) != 0;
    int w, h;
    logical_size(&w, &h);
    presenter_set_logical(w, h);
    if (hwnd) presenter_attach_window(hwnd);
    return DD_OK;
}

HRESULT DDraw::SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags) {
    log_line("DDraw::SetDisplayMode %lux%lux%lu refresh=%lu flags=%08lX", w, h, bpp, refresh, flags);
    if (bpp != 8 && bpp != 16 && bpp != 32) return DDERR_INVALIDMODE;
    mode_w = w; mode_h = h; mode_bpp = bpp;
    if (exclusive) presenter_set_logical(w, h);
    presenter_mark_dirty();
    return DD_OK;
}

HRESULT DDraw::WaitForVerticalBlank(DWORD, HANDLE) { TRACE("DDraw::WaitForVerticalBlank", ""); return DD_OK; }

HRESULT DDraw::GetAvailableVidMem(LPDDSCAPS, LPDWORD total, LPDWORD free) {
    if (total) *total = 64 * 1024 * 1024;
    if (free) *free = 64 * 1024 * 1024;
    return DD_OK;
}

// ---------------------------------------------------------------------------
// IDirectDraw v1 facade

HRESULT DDraw1::QueryInterface(REFIID riid, void** ppv) { return p_->QueryInterface(riid, ppv); }
ULONG DDraw1::AddRef() { return p_->AddRef(); }
ULONG DDraw1::Release() { return p_->Release(); }
HRESULT DDraw1::Compact() { return p_->Compact(); }
HRESULT DDraw1::CreateClipper(DWORD f, LPDIRECTDRAWCLIPPER* o, IUnknown* u) { return p_->CreateClipper(f, o, u); }
HRESULT DDraw1::CreatePalette(DWORD f, LPPALETTEENTRY t, LPDIRECTDRAWPALETTE* o, IUnknown* u) { return p_->CreatePalette(f, t, o, u); }
HRESULT DDraw1::CreateSurface(LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE* o, IUnknown* u) { return p_->CreateSurface(d, o, u); }
HRESULT DDraw1::DuplicateSurface(LPDIRECTDRAWSURFACE s, LPDIRECTDRAWSURFACE* o) { return p_->DuplicateSurface(s, o); }
HRESULT DDraw1::EnumDisplayModes(DWORD f, LPDDSURFACEDESC d, LPVOID c, LPDDENUMMODESCALLBACK cb) { return p_->EnumDisplayModes(f, d, c, cb); }
HRESULT DDraw1::EnumSurfaces(DWORD f, LPDDSURFACEDESC d, LPVOID c, LPDDENUMSURFACESCALLBACK cb) { return p_->EnumSurfaces(f, d, c, cb); }
HRESULT DDraw1::FlipToGDISurface() { return p_->FlipToGDISurface(); }
HRESULT DDraw1::GetCaps(LPDDCAPS a, LPDDCAPS b) { return p_->GetCaps(a, b); }
HRESULT DDraw1::GetDisplayMode(LPDDSURFACEDESC d) { return p_->GetDisplayMode(d); }
HRESULT DDraw1::GetFourCCCodes(LPDWORD n, LPDWORD c) { return p_->GetFourCCCodes(n, c); }
HRESULT DDraw1::GetGDISurface(LPDIRECTDRAWSURFACE* o) { return p_->GetGDISurface(o); }
HRESULT DDraw1::GetMonitorFrequency(LPDWORD f) { return p_->GetMonitorFrequency(f); }
HRESULT DDraw1::GetScanLine(LPDWORD l) { return p_->GetScanLine(l); }
HRESULT DDraw1::GetVerticalBlankStatus(WINBOOL* v) { return p_->GetVerticalBlankStatus(v); }
HRESULT DDraw1::Initialize(GUID* g) { return p_->Initialize(g); }
HRESULT DDraw1::RestoreDisplayMode() { return p_->RestoreDisplayMode(); }
HRESULT DDraw1::SetCooperativeLevel(HWND h, DWORD f) { return p_->SetCooperativeLevel(h, f); }
HRESULT DDraw1::SetDisplayMode(DWORD w, DWORD h, DWORD bpp) { return p_->SetDisplayMode(w, h, bpp, 0, 0); }
HRESULT DDraw1::WaitForVerticalBlank(DWORD f, HANDLE e) { return p_->WaitForVerticalBlank(f, e); }
