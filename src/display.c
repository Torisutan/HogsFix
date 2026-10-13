// display.c - run on Windows' own DirectDraw/Direct3D 7 (no dgVoodoo), fullscreen or windowed.
//
// display_boot() runs from the boot hook (0x47ef04, where the game initialises its "fullscreen" flag
// [0x520770]+0x314) and installs:
//   - exe IAT LoadLibraryA -> when the game loads Data\_d3d.dll, hook its SelectDriverMode / Begin2D / CopyToScreen
//     and its GetClientRect import.
//   - exe FUN_0044d240 (windowed: size the window for w x h) -> size_window().
//
// The engine's lost-surface recovery is "set DAT_11ad9c38, then re-run SelectDriverMode", which releases and recreates
// primary, back buffer and D3D device. Problems with native DirectDraw, fixed here:
//   1. The exe caches primary/back buffer in [0x520944]+4/+8 (refreshed only in FUN_0044fa40). After a reset inside
//      _d3d the cache dangles -> Bink loop FUN_0043ef50 blits to a freed surface -> crash at 0x44d8b1.
//      Fix: refresh the cache (FUN_0044e010) after every SelectDriverMode.
//   2. A reset while it cannot succeed (window minimized, or fullscreen without focus) left the D3D device NULL
//      (Begin2D then crashed at _d3d+0x663f) or spun forever in Begin2D's reset loop.
//      Fix: only reset when possible, otherwise keep the lost flag and skip the frame; one attempt per frame.
//   3. Windowed: the back buffer was the client size (menus are drawn at fixed 640x480 coordinates) and the blit to
//      the screen assumed a menu bar. Fix: back buffer = logical game size (GetClientRect hook), CopyToScreen blits
//      it letterboxed (aspect kept) into the client area, so the window can be resized or maximized.
// Config: HogsFix.ini [Display].
#include "hogsfix.h"
#include <intrin.h>

// ---- exe ----
#define EXE_SETTINGS   (X->settings)        // ptr: +0x314 fullscreen, +0x44c/+0x450 launcher resolution
#define EXE_HWND       (X->hwnd)
#define EXE_SURFCACHE  (X->surfcache)        // ptr: +0 DirectDraw7, +4 primary, +8 back buffer
#define EXE_GETSURF    (X->getsurf)        // _d3d getter pointer used by FUN_0044e010
#define EXE_REFRESH    (X->refresh)        // __fastcall FUN_0044e010(cache)
#define EXE_SIZEWIN    (X->sizewin)        // stdcall FUN_0044d240(w, h) (ret 8)
#define EXE_RENDER     (X->render)        // __fastcall FUN_0044e490(render obj): 3D frame (battle, menu backgrounds)
#define EXE_GDIBLT     (X->gdiblt)        // stdcall FUN_0044d8c0(src, dst, dst rect, src rect, rop): surface copy via GetDC+StretchBlt
#define EXE_PAINT      (X->paint)        // cdecl FUN_0045c430(screen): WM_PAINT redraw of the menu screen
#define EXE_LOADBMP    (X->loadbmp)        // thiscall FUN_0044dc70(bmp, surface, x, y): BMP -> new sysmem surface (8/16 bpp only)
#define EXE_LAUNCH_FS  (X->launch_fs)       // imm8 of "mov [eax+0x314], 1" after the launcher dialog
// ---- _d3d.dll (rva) ----
#define D_SDM          0x5360         // SelectDriverMode(hwnd, mode)  (mode -1 = windowed)
#define D_BEGIN2D      0x65d0
#define D_COPYTOSCREEN 0x3140
#define D_LOST         0x1ad9c38      // "surfaces lost, re-run SelectDriverMode" flag
#define D_STATE        0x1b65e9c      // ptr: +0xb0 current mode index (-1 windowed), +0xbc disabled
#define D_HWND         0x1b6a050
#define D_DD           0x1b6a2f0      // IDirectDraw7
#define D_PRIMARY      0x1b6a2f4
#define D_BACK         0x1b6a300
#define D_DEVICE       0x1ab9470
#define D_D3D7         0x1ab9474      // IDirect3D7
#define D_BBW          0x2c500
#define D_BBH          0x2c504

static const char *curmode(void) {
    static char b[64]; DEVMODEA dm; dm.dmSize = sizeof dm; dm.dmDriverExtra = 0;
    if (!EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) return "?";
    wsprintfA(b, "%ux%ux%u@%u", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel, dm.dmDisplayFrequency); return b;
}
static int lw = 640, lh = 480;              // logical (back buffer) size requested by the game
static int win_sized;
volatile DWORD hf_dstats[11];               // 8 frames presented (CopyToScreen), 9 2D blits batched, 10 batch copy-backs; 0 soft restores, 1 SelectDriverMode ok, 2 recoveries deferred (no focus /
                                            // minimized), 3 recoveries failed, 4 cache refreshes, 5 lost detected
                                            // (flip/blit/BeginScene), 6 SelectDriverMode failed, 7 TestCooperativeLevel busy
#define D32(rva) U32(d3d + (rva))
#define DI32(rva) I32(d3d + (rva))

typedef HRESULT (__stdcall *m0_t)(void *);
#define VFN(obj, i) ((m0_t)(*(DWORD **)(obj))[i])

// ---- engine state helpers ----
static HWND game_hwnd(void) { return (HWND)U32(EXE_HWND); }
static int is_windowed_mode(void) { DWORD st = D32(D_STATE); return st && I32(st + 0xb0) == -1; }
static int can_reset(void) {
    HWND h = game_hwnd();
    if (!h || IsIconic(h)) return 0;
    if (!cfg_windowed && GetForegroundWindow() != h) return 0;   // exclusive mode needs focus
    return 1;
}
static void refresh_cache(void) {
    DWORD c = U32(EXE_SURFCACHE);
    if (c && U32(EXE_GETSURF)) { ((void (__fastcall *)(DWORD))EXE_REFRESH)(c); hf_dstats[4]++; }
}

typedef int (__cdecl *sdm_t)(HWND, int);
typedef int (__cdecl *v_t)(void);
typedef int (__cdecl *cts_t)(HWND);
static sdm_t o_sdm; static v_t o_begin2d; static cts_t o_cts; static v_t restore_tex;
static BOOL (WINAPI *real_gcr)(HWND, RECT *);

// IDirectDraw7 SetCooperativeLevel / SetDisplayMode wrappers (vtable patch). SelectDriverMode first drops to
// DDSCL_NORMAL (Windows restores the desktop mode), then sets exclusive mode + SetDisplayMode again: two real mode
// changes per call, and on Windows 10/11 the surfaces created right after get lost again (DDERR_WRONGMODE) when the
// mode change settles (other monitors re-laid out) -> endless reset loop. When going fullscreen -> fullscreen we skip
// the NORMAL step and a SetDisplayMode to the mode that is already set: a reset then only recreates the surfaces.
static int keep_excl, cur_fs;
typedef HRESULT (__stdcall *scl_t)(void *, HWND, DWORD);
typedef HRESULT (__stdcall *sdmode_t)(void *, DWORD, DWORD, DWORD, DWORD, DWORD);
static scl_t real_scl; static sdmode_t real_sdmode;
static HRESULT __stdcall my_scl(void *dd, HWND h, DWORD fl) {
    if (keep_excl && fl == 8) { dlog("  skip SetCooperativeLevel(NORMAL)"); return 0; }
    return real_scl(dd, h, fl);
}
// Windows 10/11 loses all surfaces once more 0.1-0.4 s after a real mode change (when the change settles, e.g. the
// other monitors are re-laid out). If that hits while SelectDriverMode is still creating its surfaces, its
// GetAttachedSurface fails and _d3d calls Terminate(6, 0x18, DDERR_SURFACELOST) = "Terminating" message box.
// So after a real mode change, wait (max 1 s) until a small probe surface in video memory has been lost.
// At the desktop resolution (e.g. 3840x2160 on a 4K screen) the surfaces get lost a second time ~80 ms after the
// first loss (RestoreAllSurfaces -> DDERR_WRONGMODE, CreateDevice -> DDERR_INVALIDOBJECT, "Terminating" box). So after
// a loss, probe again until a fresh surface stays valid for 400 ms (max 3 s in total).
static void settle(void *dd) {
    DWORD t0 = timeGetTime(), t, losses = 0;
    for (;;) {
        DWORD sd[31] = { 124, 7 }; void *probe = NULL;           // DDSD_CAPS|HEIGHT|WIDTH
        sd[2] = 8; sd[3] = 8; sd[26] = 0x4040;                     // 8x8, OFFSCREENPLAIN | VIDEOMEMORY
        if (((HRESULT (__stdcall *)(void *, void *, void **, void *))(*(DWORD **)dd)[6])(dd, sd, &probe, NULL) || !probe) {
            if ((t = timeGetTime() - t0) >= 3000) break;
            Sleep(50); continue;                                     // mode still switching
        }
        DWORD t1 = timeGetTime(), lost = 0;
        DWORD wait = losses ? 400 : 1000;                          // first: wait for the expected loss
        while (timeGetTime() - t1 < wait && !(lost = VFN(probe, 24)(probe) != 0)) Sleep(5);   // IsLost
        VFN(probe, 2)(probe);                                                                  // Release
        t = timeGetTime() - t0;
        if (!lost || t >= 3000) break;
        losses++;
    }
    dlog("  settle: %u surface loss(es), stable after %u ms", losses, t);
}
// The mode the game set last. After alt-tab back Windows restored it as 32 bpp (the 16-bit emulation of the
// DWM8And16BitMitigation layer was gone): RestoreAllSurfaces succeeded but everything ran through a slow conversion
// (menus ~20 FPS, battle ~12). Compare the real desktop mode and set the mode again when it differs.
static DWORD want_w, want_h, want_bpp;
static int desktop_is_game_mode(void) {
    DEVMODEA dm; dm.dmSize = sizeof dm; dm.dmDriverExtra = 0;
    if (!want_w || !EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) return 1;
    return dm.dmPelsWidth == want_w && dm.dmPelsHeight == want_h && dm.dmBitsPerPel == want_bpp;
}
static HRESULT __stdcall my_sdmode(void *dd, DWORD w, DWORD h, DWORD bpp, DWORD hz, DWORD fl) {
    if (keep_excl && desktop_is_game_mode()) {
        DWORD sd[31] = { 124 };                                    // DDSURFACEDESC2: +8 h, +0xc w, +0x54 bpp
        if (!((HRESULT (__stdcall *)(void *, void *))(*(DWORD **)dd)[12])(dd, sd)      // GetDisplayMode
            && sd[3] == w && sd[2] == h && sd[21] == bpp) { dlog("  skip SetDisplayMode(%ux%ux%u), already set", w, h, bpp); return 0; }
    }
    HRESULT hr = real_sdmode(dd, w, h, bpp, hz, fl);
    dlog("  SetDisplayMode(%ux%ux%u) = %08x", w, h, bpp, hr);
    if (!hr) { want_w = w; want_h = h; want_bpp = bpp; settle(dd); }
    return hr;
}
// ---- 2D batching (FastBlit) ----
// Menus/HUD: the exe draws every sprite with BltFast(back buffer, sprite, SRCCOLORKEY) right after D3D rendered the
// background into the same back buffer. With the 16-bit emulation each of these 2D blits flushes D3D and waits for
// the GPU (~4 ms): screens with ~25 sprites ran at 10 FPS. Instead, the exe's blits to the back buffer go to a
// system-memory shadow copy (CPU only); the shadow is filled from the back buffer at the first such blit of a frame
// and copied back before anything else uses the back buffer (3D render, Begin2D, present, Lock/GetDC, other blits).
// Once Direct3D has drawn into the back buffer in this frame (the 3D pig on the team screens) it cannot be read back:
// under the 16-bit emulation the copy came back without the 3D picture, and the copy-back then erased it. Then the
// shadow is an overlay instead: filled with a colour key, sprites drawn into it, one colour-keyed Blt onto the back
// buffer (no read-back).
typedef HRESULT (__stdcall *blt_t)(void *, RECT *, void *, RECT *, DWORD, void *);
typedef HRESULT (__stdcall *bltfast_t)(void *, DWORD, DWORD, void *, RECT *, DWORD);
typedef HRESULT (__stdcall *lock_t)(void *, RECT *, void *, DWORD, HANDLE);
typedef HRESULT (__stdcall *getdc_t)(void *, HDC *);
static blt_t real_blt; static bltfast_t real_bltfast; static lock_t real_lock; static getdc_t real_getdc;
#define NSHADOW 2
static void *shadow, *shadow_for, *shadows[NSHADOW]; static int shadow_open, shadow_i;
static int overlay, d3d_drew;                // overlay mode / Direct3D drew into the back buffer since the last present
static DWORD overlay_key;
#define VFNS(p, i) ((HRESULT (__stdcall *)(void *))(*(DWORD **)(p))[i])
#define VFNS2(p, i) ((HRESULT (__stdcall *)(void *, void *))(*(DWORD **)(p))[i])
// vtable patch: take over the current entry as the original (Windows swaps entries at run time), install ours
#define HOOKV(i, fn, real) do { void *p_ = (void *)fn; if (vt[i] != (DWORD)p_) { *(void **)&real = (void *)vt[i]; write_code(&vt[i], &p_, 4); } } while (0)
static void sh_unlock(void);
static void shadow_drop(void) {
    sh_unlock(); shadow_open = 0;
    for (int i = 0; i < NSHADOW; i++) if (shadows[i]) { VFNS(shadows[i], 2)(shadows[i]); shadows[i] = NULL; }   // Release
    shadow = shadow_for = NULL;
}
// Copy-back on the CPU (Lock both + memcpy). A Blt from the system-memory shadow returned before the copy was done
// and the present then read a half-updated back buffer (torn / missing words on the title screen).
static HRESULT cpu_copy(void *dst, void *src) {
    DWORD ds[31] = { 124 }, ss[31] = { 124 };
    HRESULT hr = real_lock(src, NULL, ss, 1 | 0x10, NULL);                      // DDLOCK_WAIT | READONLY
    if (hr) return hr;
    hr = real_lock(dst, NULL, ds, 1 | 0x20, NULL);                              // DDLOCK_WAIT | WRITEONLY
    if (!hr) {
        DWORD w = ds[3] < ss[3] ? ds[3] : ss[3], h = ds[2] < ss[2] ? ds[2] : ss[2], bytes = w * (ds[21] / 8);
        BYTE *d = (BYTE *)ds[9], *s = (BYTE *)ss[9];
        if (ds[21] != ss[21] || !d || !s) hr = -1;
        else for (DWORD y = 0; y < h; y++, d += (int)ds[4], s += (int)ss[4]) __movsb(d, s, bytes);
        VFNS2(dst, 32)(dst, NULL);                                              // Unlock
    }
    VFNS2(src, 32)(src, NULL);
    { static int n; if (hr && ++n <= 5) dlog("2D batch: CPU copy failed %08x", hr); }
    return hr;
}
// Software BltFast into the shadow: DirectDraw's own blit between two system-memory surfaces costs ~130 us under the
// 16-bit emulation (kernel round trips); screens with 100-150 sprites dropped below 60 FPS. The shadow stays locked
// while sprites are copied into it (sprite cache below). Anything unusual -> DirectDraw.
static BYTE *sh_bits; static int sh_pitch, sh_w, sh_h, sh_bpp;
static void sh_unlock(void) { if (sh_bits) { sh_bits = NULL; VFNS2(shadow, 32)(shadow, NULL); } }
static int sh_lock(void) {
    if (sh_bits) return 1;
    DWORD d[31] = { 124 };
    if (real_lock(shadow, NULL, d, 1, NULL)) return 0;                          // DDLOCK_WAIT
    sh_bits = (BYTE *)d[9]; sh_pitch = (int)d[4]; sh_w = d[3]; sh_h = d[2]; sh_bpp = d[21];
    if (!sh_bits || (sh_bpp != 16 && sh_bpp != 32)) { sh_bits = (BYTE *)1; sh_unlock(); return 0; }
    return 1;
}
// Sprite cache: locking a sprite costs even more than DirectDraw's blit (~270 us), so each system-memory sprite is
// locked once and its pixel address, size, format and source colour key are kept. Entries are dropped when the
// surface is released (Release hook) or its colour key changes (SetColorKey hook); every 64th use re-locks to check
// that the address did not move. Sprites in video memory (fonts) get a private copy instead, dropped whenever the game
// writes to the sprite (Lock without READONLY, GetDC, Blt/BltFast into it, Restore) or surfaces are restored.
typedef struct { void *s; BYTE *bits, *own; int pitch, w, h, bpp, key, uses; DWORD lo, hi; } Sprite;
#define NSPR 1024
static Sprite spr[NSPR]; static int spr_count;
static Sprite *spr_find(void *s) {
    DWORD i = ((DWORD)s >> 4) % NSPR;
    for (int n = 0; n < NSPR; n++, i = (i + 1) % NSPR) { if (spr[i].s == s) return &spr[i]; if (!spr[i].s) return NULL; }
    return NULL;
}
static void spr_forget(void *s) {          // remove + re-insert the rest of the cluster (open addressing)
    Sprite *e = spr_find(s); if (!e) return;
    if (e->own) { HeapFree(GetProcessHeap(), 0, e->own); e->own = NULL; }
    DWORD i = (DWORD)(e - spr); e->s = NULL; spr_count--;
    for (i = (i + 1) % NSPR; spr[i].s; i = (i + 1) % NSPR) { Sprite t = spr[i]; spr[i].s = NULL; Sprite *f = &spr[((DWORD)t.s >> 4) % NSPR]; while (f->s) f = f == &spr[NSPR - 1] ? spr : f + 1; *f = t; }
}
static void spr_reset(void) {
    for (int i = 0; i < NSPR; i++) { if (spr[i].own) HeapFree(GetProcessHeap(), 0, spr[i].own); spr[i].s = NULL; spr[i].own = NULL; }
    spr_count = 0;
}
static Sprite *spr_get(void *src) {
    Sprite *e = spr_find(src);
    if (e && (e->own || (++e->uses & 63))) return e->bits ? e : NULL;
    DWORD d[31] = { 124 };
    if (real_lock(src, NULL, d, 1 | 0x10, NULL)) return NULL;                  // DDLOCK_WAIT | READONLY
    BYTE *bits = d[21] == 16 || d[21] == 32 ? (BYTE *)d[9] : NULL, *own = NULL;
    int pitch = (int)d[4];
    if (bits && !(d[26] & 0x800)) {                                             // video memory: private copy
        int row = d[3] * (d[21] / 8);
        own = (BYTE *)HeapAlloc(GetProcessHeap(), 0, row * d[2]);
        if (own) for (DWORD j = 0; j < d[2]; j++) __movsb(own + j * row, bits + j * pitch, row);
        bits = own; pitch = row;
    }
    VFNS2(src, 32)(src, NULL);                                                 // Unlock: system memory stays where it is
    if (e) { if (e->bits != bits) { static int n; if (++n <= 5) dlog("2D batch: sprite %p moved %p -> %p", src, e->bits, bits); } }
    else {
        if (spr_count >= NSPR * 3 / 4) { spr_reset(); dlog("2D batch: sprite cache reset"); }
        e = &spr[((DWORD)src >> 4) % NSPR]; while (e->s) e = e == &spr[NSPR - 1] ? spr : e + 1;
        spr_count++;
    }
    e->s = src; e->bits = bits; e->own = own; e->pitch = pitch; e->w = d[3]; e->h = d[2]; e->bpp = d[21]; e->uses = 1;
    e->key = (d[1] & 0x10000) != 0; e->lo = d[16]; e->hi = d[17];               // DDSD_CKSRCBLT, ddckCKSrcBlt
    return bits ? e : NULL;
}
// 1 = done (*hr = result), 0 = not handled
static int sw_bltfast(DWORD x, DWORD y, void *src, RECT *r, DWORD fl, HRESULT *hr) {
    if (fl & ~0x11) return 0;                                                   // only NOCOLORKEY / SRCCOLORKEY (+WAIT)
    Sprite *e = spr_get(src);
    if (!e || !sh_lock() || e->bpp != sh_bpp) return 0;
    RECT rc = { 0, 0, e->w, e->h }; if (r) rc = *r;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (rc.left < 0 || rc.top < 0 || rc.right > e->w || rc.bottom > e->h || w <= 0 || h <= 0 ||
        (int)x < 0 || (int)y < 0 || (int)x + w > sh_w || (int)y + h > sh_h) { *hr = 0x88760096; return 1; }   // DDERR_INVALIDRECT
    int key = fl & 1; DWORD lo = e->lo, hi = e->hi;
    if (key && !e->key) { *hr = 0x88760033; return 1; }                         // DDERR_NOCOLORKEY
    for (int j = 0; j < h; j++) {
        BYTE *s = e->bits + (rc.top + j) * e->pitch, *t = sh_bits + (y + j) * sh_pitch;
        if (sh_bpp == 16) {
            WORD *a = (WORD *)s + rc.left, *b = (WORD *)t + x;
            if (!key) __movsw(b, a, w);
            else for (int i = 0; i < w; i++) { WORD c = a[i]; if (c < lo || c > hi) b[i] = c; }
        } else {
            DWORD *a = (DWORD *)s + rc.left, *b = (DWORD *)t + x;
            if (!key) __movsd(b, a, w);
            else for (int i = 0; i < w; i++) { DWORD c = a[i] & 0xffffff; if (c < lo || c > hi) b[i] = a[i]; }
        }
    }
    *hr = 0;
    return 1;
}
// Big back buffers (e.g. 1920x1080 outside the battle): full-size fills and copies cost ~4 MB each per batch, so the
// shadow is always an overlay there and only the 16x16 tiles that blits touch are filled with the key and copied back.
#define TILE 16
static int tiled, tw, th, bx0, by0, bx1, by1;    // tile mode, tiles per row/column, dirty bounding box (pixels)
static BYTE tmask[(2048 / TILE) * (2048 / TILE)];
static void tile_fill(int tx, int ty) {
    int x0 = tx * TILE, y0 = ty * TILE, x1 = x0 + TILE, y1 = y0 + TILE;
    if (x1 > sh_w) x1 = sh_w; if (y1 > sh_h) y1 = sh_h;
    for (int y = y0; y < y1; y++) {
        BYTE *row = sh_bits + y * sh_pitch;
        if (sh_bpp == 16) __stosw((WORD *)row + x0, (WORD)overlay_key, x1 - x0); else __stosd((DWORD *)row + x0, overlay_key, x1 - x0);
    }
}
static int tile_prep(int x0, int y0, int x1, int y1) {   // key-fill the untouched tiles under a blit; 0 = failed
    if (!tiled) return 1;
    if (!sh_lock()) return 0;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > sh_w) x1 = sh_w; if (y1 > sh_h) y1 = sh_h;
    if (x0 >= x1 || y0 >= y1) return 1;
    for (int ty = y0 / TILE; ty <= (y1 - 1) / TILE; ty++)
        for (int tx = x0 / TILE; tx <= (x1 - 1) / TILE; tx++)
            if (!tmask[ty * tw + tx]) { tmask[ty * tw + tx] = 1; tile_fill(tx, ty); }
    if (bx1 <= bx0) { bx0 = x0; by0 = y0; bx1 = x1; by1 = y1; }
    else { if (x0 < bx0) bx0 = x0; if (y0 < by0) by0 = y0; if (x1 > bx1) bx1 = x1; if (y1 > by1) by1 = y1; }
    return 1;
}
static HRESULT tile_flush(void) {
    if (bx1 <= bx0) return 0;                                                  // nothing drawn
    RECT r = { bx0 / TILE * TILE, by0 / TILE * TILE, (bx1 + TILE - 1) / TILE * TILE, (by1 + TILE - 1) / TILE * TILE };
    if (r.right > sh_w) r.right = sh_w; if (r.bottom > sh_h) r.bottom = sh_h;
    if (sh_lock()) {                                                           // key-fill the untouched tiles inside the box
        for (int ty = r.top / TILE; ty < (r.bottom + TILE - 1) / TILE; ty++)
            for (int tx = r.left / TILE; tx < (r.right + TILE - 1) / TILE; tx++)
                if (!tmask[ty * tw + tx]) tile_fill(tx, ty);
    }
    sh_unlock();
    return real_blt(shadow_for, &r, shadow, &r, 0x1008000, NULL);             // DDBLT_WAIT | KEYSRC
}
static void shadow_flush(void) {
    if (!shadow_open) return;
    shadow_open = 0;
    if (tiled) { HRESULT hr = tile_flush(); hf_dstats[10]++; if (hr) { static int n; if (++n <= 5) dlog("2D batch: tile copy back failed %08x", hr); } return; }
    sh_unlock();
    HRESULT hr = overlay ? real_blt(shadow_for, NULL, shadow, NULL, 0x1008000, NULL)   // DDBLT_WAIT | KEYSRC
                         : cpu_copy(shadow_for, shadow);
    if (hr && !overlay) hr = real_blt(shadow_for, NULL, shadow, NULL, 0x1000000, NULL);   // DDBLT_WAIT
    hf_dstats[10]++;
    if (hr) { static int n; if (++n <= 5) dlog("2D batch: copy back failed %08x", hr); }
}
// The battle itself (HUD sprites between many 3D draws at e.g. 1920x1080) is drawn directly as before. Other screens
// at battle resolution (pig close-up after the battle: ~6 FPS) are batched too. Battle = the battle loop (battle.c)
// rendered a frame within the last 0.5 s.
extern volatile DWORD hf_stats[16];
static int battle_active(void) {
    static DWORD last_sum, last_t; DWORD sum = hf_stats[0] + hf_stats[1], t = timeGetTime();
    if (sum != last_sum) { last_sum = sum; last_t = t; }
    return t - last_t < 500;
}
static int shadow_begin(void *back) {
    if (shadow_open) return 1;
    if (DI32(D_BBW) * DI32(D_BBH) > 1024 * 768 && battle_active()) return 0;
    if (shadow_for != back) {
        shadow_drop();
        DWORD sd[31] = { 124 }, nd[31] = { 124, 0x1007 };                      // CAPS|HEIGHT|WIDTH|PIXELFORMAT
        int *dd = (int *)D32(D_DD);
        if (!dd || ((HRESULT (__stdcall *)(void *, DWORD *))(*(DWORD **)back)[22])(back, sd)) return 0;   // GetSurfaceDesc
        nd[2] = sd[2]; nd[3] = sd[3]; for (int i = 18; i < 26; i++) nd[i] = sd[i];
        nd[26] = 0x840;                                                           // OFFSCREENPLAIN | SYSTEMMEMORY
        overlay_key = sd[21] == 16 ? 0x0821 : 0x010101;                           // near-black, not used by the menu art's own key
        for (int i = 0; i < NSHADOW; i++) {
            HRESULT hr = ((HRESULT (__stdcall *)(void *, DWORD *, void **, void *))(*(DWORD **)dd)[6])(dd, nd, &shadows[i], NULL);
            if (hr || !shadows[i]) { shadows[i] = NULL; shadow_drop(); static int n; if (++n <= 5) dlog("2D batch: shadow %ux%u failed %08x", sd[3], sd[2], hr); return 0; }
            DWORD ck[2] = { overlay_key, overlay_key };
            ((HRESULT (__stdcall *)(void *, DWORD, DWORD *))(*(DWORD **)shadows[i])[29])(shadows[i], 8, ck);   // SetColorKey(DDCKEY_SRCBLT)
        }
        shadow_for = back;
        dlog("2D batch: %d shadow surfaces %ux%u", NSHADOW, sd[3], sd[2]);
    }
    // Rotate through NSHADOW copies: the colour-keyed copy-back reads the shadow after Blt returned.
    shadow_i = (shadow_i + 1) % NSHADOW; shadow = shadows[shadow_i];
    tiled = DI32(D_BBW) * DI32(D_BBH) > 1024 * 768;
    if (tiled) {
        DWORD sd[31] = { 124 };
        if (((HRESULT (__stdcall *)(void *, DWORD *))(*(DWORD **)shadow)[22])(shadow, sd) || sd[3] > 2048 || sd[2] > 2048) return 0;
        tw = (sd[3] + TILE - 1) / TILE; th = (sd[2] + TILE - 1) / TILE;
        __stosb(tmask, 0, tw * th); bx0 = by0 = bx1 = by1 = 0;
        overlay = 1;
    } else if ((overlay = d3d_drew) != 0) {
        DWORD fx[25] = { 100 }; fx[20] = overlay_key;                           // DDBLTFX.dwFillColor
        if (real_blt(shadow, NULL, NULL, NULL, 0x1000400, fx)) return 0;        // DDBLT_WAIT | COLORFILL
    } else if (cpu_copy(shadow, back) && real_blt(shadow, NULL, back, NULL, 0x1000000, NULL)) return 0;
    // (CPU copy: windowed, the Blt from the video-memory back buffer finished after the first sprite was drawn into
    // the shadow and overwrote it - pedestal on the team screens missing)
    shadow_open = 1;
    return 1;
}
static int batch_target(void *dst, DWORD ret) {   // exe blit onto the current back buffer?
    DWORD c = U32(EXE_SURFCACHE);
    return cfg_fastblit && c && dst == (void *)U32(c + 8) && ret >= 0x401000 && ret < 0x4bc000 && !D32(D_LOST);
}
static HRESULT __stdcall my_bltfast(void *dst, DWORD x, DWORD y, void *src, RECT *r, DWORD fl) {
    if (spr_count) spr_forget(dst);
    if (src != shadow && batch_target(dst, (DWORD)_ReturnAddress()) && shadow_begin(dst)) {
        HRESULT hr;
        if (tiled) {
            int w, h;
            if (r) { w = r->right - r->left; h = r->bottom - r->top; }
            else { DWORD d[31] = { 124 }; ((HRESULT (__stdcall *)(void *, DWORD *))(*(DWORD **)src)[22])(src, d); w = d[3]; h = d[2]; }
            if (!tile_prep(x, y, x + w, y + h)) goto direct;
        }
        if (src == shadow_for || !sw_bltfast(x, y, src, r, fl & ~0x10, &hr)) { sh_unlock(); hr = real_bltfast(shadow, x, y, src, r, fl | 0x10); }   // DDBLTFAST_WAIT (see my_blt)
        if (!hr) { hf_dstats[9]++; return 0; }
        if (hr == (HRESULT)0x88760096) return hr;    // DDERR_INVALIDRECT (list rows scrolled off screen): same size as the back buffer, fails there too
    }
direct:
    if (shadow_open && (dst == shadow_for || src == shadow_for)) shadow_flush();
    return real_bltfast(dst, x, y, src, r, fl);
}
static HRESULT __stdcall my_blt(void *dst, RECT *dr, void *src, RECT *sr, DWORD fl, void *fx) {
    if (spr_count) spr_forget(dst);
    if (src != shadow && src != dst && batch_target(dst, (DWORD)_ReturnAddress()) && shadow_begin(dst)) {
        // DDBLT_WAIT: the title screen blits without it; right after the copy-back the shadow is still busy ->
        // DDERR_WASSTILLDRAWING, the fallback onto the busy back buffer failed the same way (words of "Taste
        // druecken" missing in most frames)
        if (tiled && !(dr ? tile_prep(dr->left, dr->top, dr->right, dr->bottom) : tile_prep(0, 0, sh_w ? sh_w : 4096, sh_h ? sh_h : 4096))) goto direct;
        sh_unlock();
        HRESULT hr = real_blt(shadow, dr, src, sr, fl | 0x1000000, fx);
        if (!hr) { hf_dstats[9]++; return 0; }
        if (hr == (HRESULT)0x88760096) return hr;    // DDERR_INVALIDRECT, see my_bltfast
        { static int n; if (++n <= 5) { DWORD d[31] = { 124 }; ((HRESULT (__stdcall *)(void *, DWORD *))(*(DWORD **)src)[22])(src, d);
          dlog("2D batch: Blt failed %08x (fl %x) dr %d,%d,%d,%d sr %p src %ux%u caps %x", hr, fl, dr ? dr->left : -1, dr ? dr->top : -1, dr ? dr->right : -1, dr ? dr->bottom : -1, sr, d[3], d[2], d[26]); } }
    }
direct:
    if (shadow_open && (dst == shadow_for || src == shadow_for)) shadow_flush();
    return real_blt(dst, dr, src, sr, fl, fx);
}
typedef ULONG (__stdcall *release_t)(void *);
typedef HRESULT (__stdcall *setck_t)(void *, DWORD, void *);
static release_t real_release; static setck_t real_setck;
static ULONG __stdcall my_release(void *s) { ULONG n = real_release(s); if (!n && spr_count) spr_forget(s); return n; }
static HRESULT __stdcall my_setck(void *s, DWORD fl, void *ck) { if (spr_count) spr_forget(s); return real_setck(s, fl, ck); }
typedef HRESULT (__stdcall *restore_t)(void *);
static restore_t real_restore;
static HRESULT __stdcall my_restore(void *s) { if (spr_count) spr_forget(s); return real_restore(s); }
static HRESULT __stdcall my_lock(void *s, RECT *r, void *sd, DWORD fl, HANDLE e) {
    if (spr_count && !(fl & 0x10)) spr_forget(s);                              // written to (not DDLOCK_READONLY)
    if (shadow_open && s == shadow_for) shadow_flush();
    return real_lock(s, r, sd, fl, e);
}
static HRESULT __stdcall my_getdc(void *s, HDC *dc) {
    if (spr_count) spr_forget(s);
    if (shadow_open && s == shadow_for) shadow_flush();
    return real_getdc(s, dc);
}
// D3D drawing outside FUN_0044e490 (the 3D pig on the team screens, drawn inside the same scene as the sprites around
// it) went straight into the back buffer while the shadow was open; the copy-back at present then painted the older
// shadow over it (pig invisible). Flush before the device draws, clears or changes state (a model draw always starts
// with SetRenderState/SetTexture). Direct3D swaps the Draw*Primitive* entries of the device's vtable at run time
// (fast paths), so those hooks are re-installed whenever they were replaced.
typedef HRESULT (__stdcall *bscene_t)(void *);
typedef HRESULT (__stdcall *clear_t)(void *, DWORD, void *, DWORD, DWORD, float, DWORD);
typedef HRESULT (__stdcall *dev2_t)(void *, DWORD, DWORD);
typedef HRESULT (__stdcall *draw6_t)(void *, DWORD, DWORD, DWORD, DWORD, DWORD);
typedef HRESULT (__stdcall *draw8_t)(void *, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
static bscene_t real_bscene; static clear_t real_clear; static dev2_t real_srs, real_stex;
static void rehook_draws(void *dev);
static HRESULT __stdcall my_bscene(void *dev) { shadow_flush(); rehook_draws(dev); return real_bscene(dev); }
static HRESULT __stdcall my_clear(void *dev, DWORD n, void *r, DWORD fl, DWORD c, float z, DWORD st) {
    shadow_flush(); if (fl & 1) d3d_drew = 1; return real_clear(dev, n, r, fl, c, z, st);   // D3DCLEAR_TARGET
}
static HRESULT __stdcall my_srs(void *dev, DWORD a, DWORD b) { shadow_flush(); rehook_draws(dev); return real_srs(dev, a, b); }    // SetRenderState
static HRESULT __stdcall my_stex(void *dev, DWORD a, DWORD b) { shadow_flush(); rehook_draws(dev); return real_stex(dev, a, b); }  // SetTexture
// vtable slots: 25 DrawPrimitive, 26 DrawIndexedPrimitive, 29/30 ...Strided, 31/32 ...VB
#define DRAW6(i) static draw6_t real_d##i; \
    static HRESULT __stdcall my_d##i(void *d, DWORD a, DWORD b, DWORD c, DWORD e, DWORD f) { shadow_flush(); d3d_drew = 1; return real_d##i(d, a, b, c, e, f); }
#define DRAW8(i) static draw8_t real_d##i; \
    static HRESULT __stdcall my_d##i(void *d, DWORD a, DWORD b, DWORD c, DWORD e, DWORD f, DWORD g, DWORD h) { shadow_flush(); d3d_drew = 1; return real_d##i(d, a, b, c, e, f, g, h); }
DRAW6(25) DRAW8(26) DRAW6(29) DRAW8(30) DRAW6(31) DRAW8(32)
static void rehook_draws(void *dev) {
    DWORD *vt = *(DWORD **)dev;
    HOOKV(25, my_d25, real_d25); HOOKV(26, my_d26, real_d26); HOOKV(29, my_d29, real_d29);
    HOOKV(30, my_d30, real_d30); HOOKV(31, my_d31, real_d31); HOOKV(32, my_d32, real_d32);
}
static void hook_device_vtbl(void) {
    if (!cfg_fastblit) return;
    int *dev = (int *)D32(D_DEVICE); if (!dev) return;
    DWORD *vt = *(DWORD **)dev;                       // per-device copy: hook every new device
    if (vt[5] == (DWORD)my_bscene) return;
    HOOKV(5, my_bscene, real_bscene); HOOKV(10, my_clear, real_clear); HOOKV(20, my_srs, real_srs); HOOKV(35, my_stex, real_stex);
    rehook_draws(dev);
    dlog("IDirect3DDevice7 vtable %p hooked (2D batching)", vt);
}
// Windows rewrites the entries of the shared surface vtable when the display mode changes (16-bit emulation on/off):
// after battle -> menu the hooks were gone (no batching, menus ~6 FPS). Checked again after every SelectDriverMode
// and every present; the original pointers are taken over from whatever is there now.
static void hook_surface_vtbl(void) {
    hook_device_vtbl();
    if (!cfg_fastblit) return;
    int *back = (int *)D32(D_BACK); if (!back) return;
    DWORD *vt = *(DWORD **)back;
    if (vt[5] == (DWORD)my_blt && vt[7] == (DWORD)my_bltfast && vt[17] == (DWORD)my_getdc && vt[25] == (DWORD)my_lock &&
        vt[2] == (DWORD)my_release && vt[29] == (DWORD)my_setck && vt[27] == (DWORD)my_restore) return;
    HOOKV(5, my_blt, real_blt); HOOKV(7, my_bltfast, real_bltfast); HOOKV(17, my_getdc, real_getdc); HOOKV(25, my_lock, real_lock);
    HOOKV(2, my_release, real_release); HOOKV(29, my_setck, real_setck); HOOKV(27, my_restore, real_restore);
    static int n; if (++n <= 20) dlog("IDirectDrawSurface7 vtable %p hooked (2D batching)", vt);
}

static void hook_dd_vtbl(void) {
    int *dd = (int *)D32(D_DD); if (!dd) return;
    DWORD *vt = *(DWORD **)dd;
    if (vt[20] == (DWORD)my_scl && vt[21] == (DWORD)my_sdmode) return;
    HOOKV(20, my_scl, real_scl); HOOKV(21, my_sdmode, real_sdmode);
    static int n; if (++n <= 20) dlog("IDirectDraw7 vtable %p hooked", vt);
}
// IDirect3D7::CreateDevice (vtable patch). Entering the 1920x1080 battle, CreateDevice sometimes failed with
// E_INVALIDARG right after the mode change -> _d3d Terminate(0x53, 200) = "Terminating" box, game hangs. Restore the
// surfaces and retry for up to 2 s instead.
typedef HRESULT (__stdcall *cdev_t)(void *, const void *, void *, void **);
static cdev_t real_cdev;
static HRESULT __stdcall my_cdev(void *d3d7, const void *g, void *surf, void **out) {
    HRESULT hr = real_cdev(d3d7, g, surf, out);
    if (!hr) return hr;
    int *dd = (int *)D32(D_DD);
    dlog("CreateDevice failed %08x (target lost %08x, coop %08x, display %s, fg %d), retrying", hr,
         surf ? VFN(surf, 24)(surf) : 0, dd ? VFN(dd, 26)(dd) : 0, curmode(), GetForegroundWindow() == game_hwnd());
    for (int i = 0; hr && i < 20; i++) {
        Sleep(100);
        HRESULT ra = dd ? VFN(dd, 25)(dd) : -1;                     // RestoreAllSurfaces
        HRESULT rs = surf ? VFN(surf, 27)(surf) : -1;               // Restore (the render target itself)
        hr = real_cdev(d3d7, g, surf, out);
        if (i < 5 || !hr) dlog("  retry %d: RestoreAll %08x, Restore %08x, lost %08x, coop %08x, display %s -> %08x", i, ra, rs,
                               surf ? VFN(surf, 24)(surf) : 0, dd ? VFN(dd, 26)(dd) : 0, curmode(), hr);
    }
    dlog("CreateDevice retry: %08x", hr);
    return hr;
}
static void hook_d3d7_vtbl(void) {
    int *d7 = (int *)D32(D_D3D7); if (!d7) return;
    DWORD *vt = *(DWORD **)d7;
    if (vt[4] == (DWORD)my_cdev) return;
    HOOKV(4, my_cdev, real_cdev);
    static int n; if (++n <= 20) dlog("IDirect3D7 vtable %p hooked", vt);
}
static int in_sdm;
static int __cdecl my_sdm(HWND h, int mode) {
    DWORD st = D32(D_STATE);
    in_sdm++;
    shadow_drop(); spr_reset();
    hook_dd_vtbl();
    keep_excl = mode != -1 && cur_fs && real_scl;
    int r = o_sdm(h, mode);
    keep_excl = 0;
    in_sdm--;
    hook_dd_vtbl();
    hook_d3d7_vtbl();
    refresh_cache();
    if (!D32(D_DEVICE) || D32(D_LOST)) {   // failed half way: device is gone, mark lost so nobody uses it
        D32(D_LOST) = 1; if (st) I32(st + 0xb0) = mode;
        hf_dstats[6]++; dlog("SelectDriverMode(%d) failed (r=%d), retry later", mode, r);
    } else {
        hook_surface_vtbl();
        if (mode != -1) {                  // remember the game's mode (the first SetDisplayMode runs before the hooks)
            DEVMODEA dm; dm.dmSize = sizeof dm; dm.dmDriverExtra = 0;
            if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) { want_w = dm.dmPelsWidth; want_h = dm.dmPelsHeight; want_bpp = dm.dmBitsPerPel; }
        }
        cur_fs = mode != -1; hf_dstats[1]++; dlog("SelectDriverMode(%d) ok, back buffer %dx%d, display %s", mode, DI32(D_BBW), DI32(D_BBH), curmode());
    }
    return r;
}
// one reset attempt (the original loops "while lost", which spins forever if the reset can't succeed)
static int try_reset(void) {
    DWORD st = D32(D_STATE); if (!st) return 0;
    int mode = I32(st + 0xb0);
    D32(D_LOST) = 0; I32(st + 0xb0) = -1;
    my_sdm((HWND)D32(D_HWND), mode);
    if (D32(D_LOST)) return 0;
    if (restore_tex) restore_tex();
    return 1;
}
// DirectX 7 recovery without a mode change: TestCooperativeLevel, RestoreAllSurfaces, reload texture contents from
// their system-memory copies. 1 = restored, 0 = not possible now (retry later), -1 = display mode changed (full reset).
// (The engine's own recovery re-runs SelectDriverMode = cooperative level NORMAL + SetDisplayMode, a new mode change
// that loses the new surfaces again ~0.1-0.4 s later on Windows 10/11: endless black-screen loop.)
static int soft_restore(void) {
    int *dd = (int *)D32(D_DD); if (!dd) return -1;
    if (!cfg_windowed && !desktop_is_game_mode()) { dlog("display %s is not the game's mode, resetting", curmode()); return -1; }
    HRESULT hr = VFN(dd, 26)(dd);                                   // TestCooperativeLevel
    if (hr == (HRESULT)0x8876024b) return -1;                       // DDERR_WRONGMODE
    if (hr) { hf_dstats[7]++; return 0; }                           // no exclusive mode (yet)
    hr = VFN(dd, 25)(dd);                                           // RestoreAllSurfaces
    if (hr) { dlog("RestoreAllSurfaces failed %08x, display %s", hr, curmode()); return hr == (HRESULT)0x8876024b ? -1 : 0; }
    D32(D_LOST) = 0;
    spr_reset();                                                    // video-memory sprites were lost: drop private copies
    if (restore_tex) restore_tex();
    hf_dstats[0]++; if (hf_dstats[0] <= 50) dlog("surfaces restored");
    return 1;
}
static int recover(void) {        // 1 = usable again
    if (!can_reset()) { hf_dstats[2]++; return 0; }
    int r = soft_restore();
    if (r < 0) r = try_reset();
    if (!r) hf_dstats[3]++;
    return r > 0;
}
static int __cdecl my_begin2d(void) {
    shadow_flush();
    int reset = 0;
    if (D32(D_LOST)) {
        if (!recover()) return 0;
        reset = 1;
    }
    int r = o_begin2d();
    if (D32(D_LOST)) { hf_dstats[5]++; if (hf_dstats[5] <= 20) dlog("Begin2D: BeginScene reports lost surfaces, display %s, fg %d", curmode(), GetForegroundWindow() == game_hwnd()); }
    return r | reset;
}
// The exe keeps drawing after Begin2D reported lost surfaces; parts of the frame (terrain draw FUN_10003380 reading the
// sky sort list [0x11ab9950]+0x9c3c) then dereference NULL. Skip the whole 3D frame until the surfaces are back.
typedef void (__fastcall *render_t)(DWORD);
static render_t o_render;
static void __fastcall my_render(DWORD robj) {
    shadow_flush();
    if (d3d && D32(D_LOST) && !recover()) { hf_dstats[3]++; return; }
    o_render(robj);
}
static void letterbox(int cw, int ch, int w, int h, RECT *r) {
    int dw = cw, dh = ch;
    if (MulDiv(cw, h, w) > ch) dw = MulDiv(ch, w, h); else dh = MulDiv(cw, h, w);
    r->left = (cw - dw) / 2; r->top = (ch - dh) / 2; r->right = r->left + dw; r->bottom = r->top + dh;
}
// Windowed presents are a plain Blt, nothing waits for the display like the fullscreen Flip(DDFLIP_WAIT): menus and
// movies ran at ~200-1000 FPS (one core busy). Limit to the refresh rate of the window's monitor.
static void pace_windowed(HWND h) {         // ms clock (timeBeginPeriod(1)); doubles keep the fractional period
    static double next, period; static HMONITOR mon; static int init;
    if (!init) { init = 1; timeBeginPeriod(1); }
    HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY);
    if (m != mon) {
        MONITORINFOEXA mi; mi.cbSize = sizeof mi; DEVMODEA dm; dm.dmSize = sizeof dm; dm.dmDriverExtra = 0;
        int hz = GetMonitorInfoA(m, (MONITORINFO *)&mi) && EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 60;
        if (hz < 30 || hz > 500) hz = 60;
        mon = m; period = 1000.0 / hz;
        dlog("windowed frame limit %d Hz", hz);
    }
    double now = (double)(int)timeGetTime();
    if (now < next) {
        int ms = (int)(next - now);
        if (ms > 1) Sleep(ms - 1);
        while ((double)(int)timeGetTime() < next) Sleep(0);
        next += period;
    } else next = now + period;                 // late (or first frame): restart the schedule
}
static int __cdecl my_cts(HWND h) {
    DWORD st = D32(D_STATE);
    d3d_drew = 0;
    hf_dstats[8]++;
    shadow_flush();
    if (!D32(D_LOST) && !in_sdm) hook_surface_vtbl();
    if (D32(D_LOST)) { recover(); return 1; }   // like the original: no flip in the frame that recovers
    if (!st || I32(st + 0xbc)) return o_cts(h);
    if (I32(st + 0xb0) != -1) {              // fullscreen: same Flip as the original, but log the error
        int *prim = (int *)D32(D_PRIMARY);
        HRESULT hr = ((HRESULT (__stdcall *)(void *, void *, DWORD))(*(DWORD **)prim)[11])(prim, NULL, 1);   // Flip(NULL, DDFLIP_WAIT)
        if (hr) { D32(D_LOST) = 1; hf_dstats[5]++; if (hf_dstats[5] <= 20) dlog("Flip failed %08x, display %s, fg %d", hr, curmode(), GetForegroundWindow() == game_hwnd()); }
        return 0;
    }
    // windowed: letterboxed stretch of the logical back buffer into the client area
    pace_windowed(h);
    if (IsIconic(h)) return 0;
    RECT c; real_gcr(h, &c);
    if (c.right <= 0 || c.bottom <= 0) return 0;
    int bw = DI32(D_BBW), bh = DI32(D_BBH);
    RECT dst, src = { 0, 0, bw, bh };
    letterbox(c.right, c.bottom, bw, bh, &dst);
    POINT p = { 0, 0 }; ClientToScreen(h, &p);
    int *prim = (int *)D32(D_PRIMARY), *back = (int *)D32(D_BACK);
    typedef HRESULT (__stdcall *blt_t)(void *, RECT *, void *, RECT *, DWORD, void *);
    blt_t blt = (blt_t)(*(DWORD **)prim)[5];
    RECT bars[2]; int nb = 0;
    if (dst.left > 0) { SetRect(&bars[0], 0, 0, dst.left, c.bottom); SetRect(&bars[1], dst.right, 0, c.right, c.bottom); nb = 2; }
    else if (dst.top > 0) { SetRect(&bars[0], 0, 0, c.right, dst.top); SetRect(&bars[1], 0, dst.bottom, c.right, c.bottom); nb = 2; }
    for (int i = 0; i < nb; i++) {
        DWORD fx[25] = { 100 };                                         // DDBLTFX, dwFillColor (+0x50) = 0
        OffsetRect(&bars[i], p.x, p.y);
        if (bars[i].right > bars[i].left && bars[i].bottom > bars[i].top) blt(prim, &bars[i], NULL, NULL, 0x1000400, fx);
    }
    OffsetRect(&dst, p.x, p.y);
    HRESULT hr = blt(prim, &dst, back, &src, 0x1000000, NULL);         // DDBLT_WAIT
    if (hr) { D32(D_LOST) = 1; hf_dstats[5]++; if (hf_dstats[5] <= 20) dlog("window blit failed %08x", hr); }
    return 0;
}
// _d3d's GetClientRect (windowed SelectDriverMode sizes the back buffer from it): report the logical game size
static BOOL WINAPI my_gcr(HWND h, RECT *r) {
    if (cfg_windowed && h == game_hwnd()) { SetRect(r, 0, 0, lw, lh); return TRUE; }
    return real_gcr(h, r);
}

void display_hook_d3d(HMODULE m) {
    static const BYTE p_sdm[6] = { 0x81, 0xec, 0xf0, 0, 0, 0 }, p_b2d[5] = { 0x51, 0x53, 0x55, 0x56, 0x57 };
    BYTE *f = (BYTE *)(d3d + D_SDM), *g = (BYTE *)(d3d + D_BEGIN2D), *c = (BYTE *)(d3d + D_COPYTOSCREEN);
    for (int i = 0; i < 6; i++) if (f[i] != p_sdm[i]) { dlog("_d3d SelectDriverMode prologue mismatch, no hooks"); return; }
    for (int i = 0; i < 5; i++) if (g[i] != p_b2d[i]) { dlog("_d3d Begin2D prologue mismatch, no hooks"); return; }
    if (c[0] != 0xa1) { dlog("_d3d CopyToScreen prologue mismatch, no hooks"); return; }
    restore_tex = (v_t)GetProcAddress(m, "afRestoreTextureSurfaces");
    o_sdm = (sdm_t)hook(d3d + D_SDM, 6, my_sdm);
    o_begin2d = (v_t)hook(d3d + D_BEGIN2D, 5, my_begin2d);
    o_cts = (cts_t)hook(d3d + D_COPYTOSCREEN, 5, my_cts);
    void **gcr = find_iat(m, "user32.dll", "GetClientRect");
    real_gcr = gcr ? *gcr : (void *)GetClientRect;
    patch_iat(gcr, (void *)my_gcr);
    dlog("_d3d.dll at %08x hooked (GetClientRect import %s)", d3d, gcr ? "patched" : "NOT found");
}

// FUN_0044d240(w, h): windowed mode, called by every mode switch (menus 640x480, battle w x h).
// The first call sizes the window for the launcher resolution (fitted into the work area, aspect kept) and makes it
// resizable; later calls only change the logical size, the letterboxed blit adapts.
static void __stdcall size_window(int w, int h) {   // stdcall like the original (caller 0x44faa4 does not pop the args)
    HWND hw = game_hwnd(); lw = w; lh = h;
    if (!hw || win_sized) return;
    win_sized = 1;
    DWORD s = U32(EXE_SETTINGS);
    int cw = s ? I32(s + 0x44c) : w, ch = s ? I32(s + 0x450) : h;
    if (cw < 320 || ch < 200) { cw = w; ch = h; }
    LONG style = GetWindowLongA(hw, GWL_STYLE) | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    LONG ex = GetWindowLongA(hw, GWL_EXSTYLE);
    SetWindowLongA(hw, GWL_STYLE, style);
    MONITORINFO mi = { sizeof mi }; GetMonitorInfoA(MonitorFromWindow(hw, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT fr = { 0, 0, 0, 0 }; AdjustWindowRectEx(&fr, style, FALSE, ex);
    int fw = fr.right - fr.left, fh = fr.bottom - fr.top;
    int aw = mi.rcWork.right - mi.rcWork.left - fw, ah = mi.rcWork.bottom - mi.rcWork.top - fh;
    if (cw > aw || ch > ah) { RECT r; letterbox(aw, ah, cw, ch, &r); cw = r.right - r.left; ch = r.bottom - r.top; }
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - cw - fw) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - ch - fh) / 2;
    SetWindowPos(hw, NULL, x, y, cw + fw, ch + fh, SWP_NOZORDER | SWP_FRAMECHANGED);
    dlog("window client %dx%d at %d,%d (game %dx%d)", cw, ch, x - fr.left, y - fr.top, w, h);
}

// Surface-to-surface copy (menus): the game used GetDC on both surfaces + GDI StretchBlt. With the 16-bit emulation
// (DWM8And16BitMitigation) every GetDC/BitBlt goes through the kernel: menus ran at ~15 FPS. Plain copies use
// IDirectDrawSurface7::Blt instead; anything else (other ROPs, formats Blt rejects) takes the original GDI path.
typedef BOOL (__stdcall *gdiblt_t)(int *, int *, RECT *, RECT *, DWORD);
static gdiblt_t o_gdiblt;
static BOOL __stdcall my_gdiblt(int *src, int *dst, RECT *dr, RECT *sr, DWORD rop) {
    shadow_flush();
    if (rop == SRCCOPY && src && dst) {
        HRESULT hr = ((HRESULT (__stdcall *)(void *, RECT *, void *, RECT *, DWORD, void *))(*(DWORD **)dst)[5])(dst, dr, src, sr, 0x1000000, NULL);   // Blt(DDBLT_WAIT)
        if (!hr) return TRUE;
        static int nf; if (++nf <= 5) dlog("surface Blt failed %08x, using GDI", hr);
    }
    return o_gdiblt(src, dst, dr, sr, rop);
}

// Sprite/background loader FUN_0044dc70(bmp in memory, surface, x, y): creates a system-memory surface in the display
// format and converts the BMP into it, but only knows 8- and 16-bpp surfaces. Windowed on a 32-bit desktop every
// surface is 32 bpp and stayed uninitialised (menu backgrounds black). The original still creates the surface and sets
// its colour key (computed from the surface format, not its contents); 24/32-bpp surfaces are filled here afterwards.
typedef int *(__fastcall *loadbmp_t)(void *, void *, const BYTE *, int *, int, int);
static loadbmp_t o_loadbmp;
static int mask_shift(DWORD m, int *bits) { int s = 0, b = 0; if (!m) { *bits = 0; return 0; } while (!(m & 1)) { m >>= 1; s++; } while (m & 1) { m >>= 1; b++; } *bits = b; return s; }
static void fill_truecolor(int *s, const BYTE *bmp) {
    DWORD sd[31] = { 124 };
    if (((HRESULT (__stdcall *)(void *, RECT *, void *, DWORD, HANDLE))(*(DWORD **)s)[25])(s, NULL, sd, 1, NULL)) return;   // Lock(DDLOCK_WAIT)
    int dbpp = sd[21], rb, gb, bb, rs = mask_shift(sd[22], &rb), gs = mask_shift(sd[23], &gb), bs = mask_shift(sd[24], &bb);
    if ((dbpp == 24 || dbpp == 32) && rb && gb && bb) {
        DWORD off = *(DWORD *)(bmp + 10), hs = *(DWORD *)(bmp + 14), comp = *(DWORD *)(bmp + 30);
        int w = *(int *)(bmp + 18), h = *(int *)(bmp + 22), bpp = *(WORD *)(bmp + 28), up = h > 0;
        if (h < 0) h = -h;
        const DWORD *pal = (const DWORD *)(bmp + 14 + hs);
        DWORD m16[3] = { 0x7c00, 0x3e0, 0x1f }; int s16[3], b16[3];
        if (bpp == 16 && comp == 3) for (int i = 0; i < 3; i++) m16[i] = pal[i];
        for (int i = 0; i < 3; i++) s16[i] = mask_shift(m16[i], &b16[i]);
        int stride = ((w * bpp + 31) / 32) * 4, cw = w < (int)sd[3] ? w : (int)sd[3], ch = h < (int)sd[2] ? h : (int)sd[2];
        for (int y = 0; y < ch; y++) {
            const BYTE *row = bmp + off + stride * (up ? h - 1 - y : y);
            BYTE *dst = (BYTE *)sd[9] + (int)sd[4] * y;
            for (int x = 0; x < cw; x++) {
                DWORD c, r, g, b;
                switch (bpp) {
                case 4:  c = pal[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15]; break;
                case 8:  c = pal[row[x]]; break;
                case 16: { DWORD v = *(const WORD *)(row + 2 * x); DWORD q[3];
                           for (int i = 0; i < 3; i++) { q[i] = b16[i] ? ((v & m16[i]) >> s16[i]) * 255 / ((1u << b16[i]) - 1) : 0; }
                           c = q[0] << 16 | q[1] << 8 | q[2]; break; }
                case 24: c = row[3 * x] | row[3 * x + 1] << 8 | row[3 * x + 2] << 16; break;
                case 32: c = *(const DWORD *)(row + 4 * x); break;
                default: c = 0;
                }
                r = (c >> 16) & 255; g = (c >> 8) & 255; b = c & 255;
                if ((r >> 3) == 31 && !(g >> 2) && (b >> 3) == 31) r = b = 255, g = 0;   // magenta key as in 16 bpp (e.g. f800f8)
                DWORD v = (r >> (8 - rb)) << rs | (g >> (8 - gb)) << gs | (b >> (8 - bb)) << bs;
                if (dbpp == 32) ((DWORD *)dst)[x] = v;
                else { dst[3 * x] = (BYTE)v; dst[3 * x + 1] = (BYTE)(v >> 8); dst[3 * x + 2] = (BYTE)(v >> 16); }
            }
        }
        // colour key: FUN_0044b160 finds it by GDI SetPixel + reading the pixel back, which went wrong on these surfaces
        // (and with CLR_INVALID it read pixel 0,0 before the surface had any contents). Same rule, on the real pixels:
        // magenta when [0x51f228] (colorkey) is set, else the top-left pixel.
        DWORD key = U32(X->colorkey) ? (255u >> (8 - rb)) << rs | (255u >> (8 - bb)) << bs
                                  : (dbpp == 32 ? *(DWORD *)sd[9] : *(DWORD *)sd[9] & 0xffffff);
        DWORD ck[2] = { key, key };
        ((HRESULT (__stdcall *)(void *, RECT *))(*(DWORD **)s)[32])(s, NULL);                                              // Unlock
        ((HRESULT (__stdcall *)(void *, DWORD, DWORD *))(*(DWORD **)s)[29])(s, 8, ck);                                     // SetColorKey(SRCBLT)
        static int logged; if (!logged++) dlog("sprite loader: filling %d-bpp surfaces (BMP %d bpp %dx%d, key %06x)", dbpp, bpp, w, h, key);
        return;
    }
    ((HRESULT (__stdcall *)(void *, RECT *))(*(DWORD **)s)[32])(s, NULL);                                                  // Unlock
}
static int *__fastcall my_loadbmp(void *self, void *edx, const BYTE *bmp, int *surf, int x, int y) {
    int *s = o_loadbmp(self, edx, bmp, surf, x, y);
    if (s && !surf && bmp && *(const WORD *)bmp == 0x4d42) fill_truecolor(s, bmp);   // all callers let it create the surface
    return s;
}

// Bink movies (FUN_0043ef50): every loop iteration blits the movie surface and presents, but BinkCopyToBuffer returned
// 1 ("skipped") for every frame when presents are not throttled by a fullscreen Flip (windowed: black intro movies).
// BINKNOSKIP: always copy the decoded frame (timing is still done by BinkWait).
static int (__stdcall *o_bcb)(void *, void *, int, int, int, int, int);
static int __stdcall my_bcb(void *b, void *d, int p, int h, int x, int y, int f) { return o_bcb(b, d, p, h, x, y, f | 0x80000); }

// WM_PAINT handler FUN_0045c430(screen): redraws the current menu screen with the cached surfaces. A mode change inside
// SelectDriverMode sends WM_PAINT while the surfaces are released/NULL (crash at 0x44d89c entering a battle after
// alt-tab). Skip it during SelectDriverMode and while surfaces are lost; the next frame draws everything anyway.
typedef void (__cdecl *paint_t)(int);
static paint_t o_paint;
static void __cdecl my_paint(int screen) {
    DWORD c = U32(EXE_SURFCACHE);
    if (in_sdm || (d3d && D32(D_LOST)) || !c || !U32(c + 4) || !U32(c + 8)) return;
    o_paint(screen);
}

// called once at boot (display hooks enabled): 3D frame guard, windowed mode setup
void display_boot(void) {
    dlog("display: %s", cfg_windowed ? "windowed" : "fullscreen");
    if (*(BYTE *)EXE_RENDER == 0x83) o_render = (render_t)hook(EXE_RENDER, 6, my_render);
    static const BYTE p_gb[6] = { 0x83, 0xec, 0x08, 0x53, 0x55, 0x56 };   // sub esp, 8 / push ebx, ebp, esi
    int ok = 1; for (int i = 0; i < 6; i++) if (((BYTE *)EXE_GDIBLT)[i] != p_gb[i]) ok = 0;
    if (!cfg_fastblit) ;
    else if (ok) o_gdiblt = (gdiblt_t)hook(EXE_GDIBLT, 6, my_gdiblt); else dlog("FUN_0044d8c0 prologue mismatch, GDI blit not replaced");
    if (*(BYTE *)EXE_PAINT == 0xa1 && U32(EXE_PAINT + 1) == EXE_SETTINGS) o_paint = (paint_t)hook(EXE_PAINT, 5, my_paint); else dlog("FUN_0045c430 prologue mismatch, WM_PAINT not guarded");
    static const BYTE p_lb[6] = { 0x81, 0xec, 0x98, 0, 0, 0 };              // sub esp, 0x98
    ok = 1; for (int i = 0; i < 6; i++) if (((BYTE *)EXE_LOADBMP)[i] != p_lb[i]) ok = 0;
    if (ok) o_loadbmp = (loadbmp_t)hook(EXE_LOADBMP, 6, my_loadbmp); else dlog("FUN_0044dc70 prologue mismatch, 32-bpp sprites not filled");
    void **bcb = find_iat(GetModuleHandleA(NULL), "binkw32.dll", "_BinkCopyToBuffer@28");
    if (bcb) { o_bcb = *bcb; patch_iat(bcb, (void *)my_bcb); } else dlog("BinkCopyToBuffer import not found");
    if (cfg_windowed) {
        BYTE z = 0; if (*(BYTE *)EXE_LAUNCH_FS == 1) write_code((void *)EXE_LAUNCH_FS, &z, 1);   // other adapter != fullscreen
        if (*(BYTE *)EXE_SIZEWIN == 0xa1) write_jmp(EXE_SIZEWIN, size_window);
    }
}
