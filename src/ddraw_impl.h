#pragma once
#include "common.h"

class DDraw;

void pixel_format_for(int bpp, DDPIXELFORMAT* pf);

class Palette final : public IDirectDrawPalette {
public:
    Palette(DWORD caps, const PALETTEENTRY* entries);

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetCaps)(LPDWORD caps) override;
    STDMETHOD(GetEntries)(DWORD flags, DWORD base, DWORD count, LPPALETTEENTRY entries) override;
    STDMETHOD(Initialize)(LPDIRECTDRAW dd, DWORD flags, LPPALETTEENTRY table) override;
    STDMETHOD(SetEntries)(DWORD flags, DWORD start, DWORD count, LPPALETTEENTRY entries) override;

    PALETTEENTRY entries[256];
    bool on_primary = false;

private:
    LONG ref_ = 1;
    DWORD caps_;
};

class Clipper final : public IDirectDrawClipper {
public:
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetClipList)(LPRECT rect, LPRGNDATA list, LPDWORD size) override;
    STDMETHOD(GetHWnd)(HWND* hwnd) override;
    STDMETHOD(Initialize)(LPDIRECTDRAW dd, DWORD flags) override;
    STDMETHOD(IsClipListChanged)(WINBOOL* changed) override;
    STDMETHOD(SetClipList)(LPRGNDATA list, DWORD flags) override;
    STDMETHOD(SetHWnd)(DWORD flags, HWND hwnd) override;

private:
    LONG ref_ = 1;
    HWND hwnd_ = nullptr;
};

// One object serves IDirectDrawSurface, 2 and 3: their vtables share a common prefix.
class Surface final : public IDirectDrawSurface3 {
public:
    Surface(DDraw* dd, int w, int h, int bpp, DWORD caps);
    ~Surface();
    bool ok() const { return bits != nullptr; }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE3 s) override;
    STDMETHOD(AddOverlayDirtyRect)(LPRECT r) override;
    STDMETHOD(Blt)(LPRECT dst, LPDIRECTDRAWSURFACE3 src, LPRECT srcRect, DWORD flags, LPDDBLTFX fx) override;
    STDMETHOD(BltBatch)(LPDDBLTBATCH batch, DWORD count, DWORD flags) override;
    STDMETHOD(BltFast)(DWORD x, DWORD y, LPDIRECTDRAWSURFACE3 src, LPRECT srcRect, DWORD trans) override;
    STDMETHOD(DeleteAttachedSurface)(DWORD flags, LPDIRECTDRAWSURFACE3 s) override;
    STDMETHOD(EnumAttachedSurfaces)(LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) override;
    STDMETHOD(EnumOverlayZOrders)(DWORD flags, LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) override;
    STDMETHOD(Flip)(LPDIRECTDRAWSURFACE3 target, DWORD flags) override;
    STDMETHOD(GetAttachedSurface)(LPDDSCAPS caps, LPDIRECTDRAWSURFACE3* out) override;
    STDMETHOD(GetBltStatus)(DWORD flags) override;
    STDMETHOD(GetCaps)(LPDDSCAPS caps) override;
    STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER* out) override;
    STDMETHOD(GetColorKey)(DWORD flags, LPDDCOLORKEY key) override;
    STDMETHOD(GetDC)(HDC* dc) override;
    STDMETHOD(GetFlipStatus)(DWORD flags) override;
    STDMETHOD(GetOverlayPosition)(LPLONG x, LPLONG y) override;
    STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE* out) override;
    STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT pf) override;
    STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC desc) override;
    STDMETHOD(Initialize)(LPDIRECTDRAW dd, LPDDSURFACEDESC desc) override;
    STDMETHOD(IsLost)() override;
    STDMETHOD(Lock)(LPRECT rect, LPDDSURFACEDESC desc, DWORD flags, HANDLE ev) override;
    STDMETHOD(ReleaseDC)(HDC dc) override;
    STDMETHOD(Restore)() override;
    STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER c) override;
    STDMETHOD(SetColorKey)(DWORD flags, LPDDCOLORKEY key) override;
    STDMETHOD(SetOverlayPosition)(LONG x, LONG y) override;
    STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE p) override;
    STDMETHOD(Unlock)(LPVOID data) override;
    STDMETHOD(UpdateOverlay)(LPRECT srcRect, LPDIRECTDRAWSURFACE3 dst, LPRECT dstRect, DWORD flags, LPDDOVERLAYFX fx) override;
    STDMETHOD(UpdateOverlayDisplay)(DWORD flags) override;
    STDMETHOD(UpdateOverlayZOrder)(DWORD flags, LPDIRECTDRAWSURFACE3 ref) override;
    STDMETHOD(GetDDInterface)(LPVOID* dd) override;
    STDMETHOD(PageLock)(DWORD flags) override;
    STDMETHOD(PageUnlock)(DWORD flags) override;
    STDMETHOD(SetSurfaceDesc)(LPDDSURFACEDESC desc, DWORD flags) override;

    void fill_pixel_format(DDPIXELFORMAT* pf) const;
    bool is_visible() const;
    void swap_buffers_with(Surface* other);

    DDraw* dd;
    int w, h, bpp, pitch;
    DWORD caps;
    uint8_t* bits = nullptr;
    HANDLE section = nullptr;
    HBITMAP dib = nullptr;
    HDC dc = nullptr;
    Palette* palette = nullptr;
    Clipper* clipper = nullptr;
    Surface* back = nullptr;
    bool has_src_key = false;
    DWORD src_key = 0;

private:
    LONG ref_ = 1;
    void fill(const RECT& r, DWORD color);
    void copy_from(Surface* src, RECT d, RECT s, bool keyed, DWORD key);
};

class DDraw;

// IDirectDraw (v1) differs from v2 only in SetDisplayMode's arguments; this facade forwards to DDraw.
class DDraw1 final : public IDirectDraw {
public:
    explicit DDraw1(DDraw* p) : p_(p) {}
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Compact)() override;
    STDMETHOD(CreateClipper)(DWORD flags, LPDIRECTDRAWCLIPPER* out, IUnknown* outer) override;
    STDMETHOD(CreatePalette)(DWORD flags, LPPALETTEENTRY table, LPDIRECTDRAWPALETTE* out, IUnknown* outer) override;
    STDMETHOD(CreateSurface)(LPDDSURFACEDESC desc, LPDIRECTDRAWSURFACE* out, IUnknown* outer) override;
    STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE s, LPDIRECTDRAWSURFACE* out) override;
    STDMETHOD(EnumDisplayModes)(DWORD flags, LPDDSURFACEDESC desc, LPVOID ctx, LPDDENUMMODESCALLBACK cb) override;
    STDMETHOD(EnumSurfaces)(DWORD flags, LPDDSURFACEDESC desc, LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) override;
    STDMETHOD(FlipToGDISurface)() override;
    STDMETHOD(GetCaps)(LPDDCAPS drv, LPDDCAPS hel) override;
    STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC desc) override;
    STDMETHOD(GetFourCCCodes)(LPDWORD num, LPDWORD codes) override;
    STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE* out) override;
    STDMETHOD(GetMonitorFrequency)(LPDWORD freq) override;
    STDMETHOD(GetScanLine)(LPDWORD line) override;
    STDMETHOD(GetVerticalBlankStatus)(WINBOOL* in_vb) override;
    STDMETHOD(Initialize)(GUID* guid) override;
    STDMETHOD(RestoreDisplayMode)() override;
    STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD flags) override;
    STDMETHOD(SetDisplayMode)(DWORD w, DWORD h, DWORD bpp) override;
    STDMETHOD(WaitForVerticalBlank)(DWORD flags, HANDLE ev) override;

private:
    DDraw* p_;
};

class DDraw final : public IDirectDraw2 {
public:
    DDraw() : v1(this) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Compact)() override;
    STDMETHOD(CreateClipper)(DWORD flags, LPDIRECTDRAWCLIPPER* out, IUnknown* outer) override;
    STDMETHOD(CreatePalette)(DWORD flags, LPPALETTEENTRY table, LPDIRECTDRAWPALETTE* out, IUnknown* outer) override;
    STDMETHOD(CreateSurface)(LPDDSURFACEDESC desc, LPDIRECTDRAWSURFACE* out, IUnknown* outer) override;
    STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE s, LPDIRECTDRAWSURFACE* out) override;
    STDMETHOD(EnumDisplayModes)(DWORD flags, LPDDSURFACEDESC desc, LPVOID ctx, LPDDENUMMODESCALLBACK cb) override;
    STDMETHOD(EnumSurfaces)(DWORD flags, LPDDSURFACEDESC desc, LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) override;
    STDMETHOD(FlipToGDISurface)() override;
    STDMETHOD(GetCaps)(LPDDCAPS drv, LPDDCAPS hel) override;
    STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC desc) override;
    STDMETHOD(GetFourCCCodes)(LPDWORD num, LPDWORD codes) override;
    STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE* out) override;
    STDMETHOD(GetMonitorFrequency)(LPDWORD freq) override;
    STDMETHOD(GetScanLine)(LPDWORD line) override;
    STDMETHOD(GetVerticalBlankStatus)(WINBOOL* in_vb) override;
    STDMETHOD(Initialize)(GUID* guid) override;
    STDMETHOD(RestoreDisplayMode)() override;
    STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD flags) override;
    STDMETHOD(SetDisplayMode)(DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags) override;
    STDMETHOD(WaitForVerticalBlank)(DWORD flags, HANDLE ev) override;
    STDMETHOD(GetAvailableVidMem)(LPDDSCAPS caps, LPDWORD total, LPDWORD free) override;

    DDraw1 v1;
    int mode_w = 0, mode_h = 0, mode_bpp = 0;
    bool exclusive = false;
    Surface* primary = nullptr;

    // What the game should believe its screen (or window client area) measures.
    void logical_size(int* w, int* h) const;

private:
    LONG ref_ = 1;
};
