// HogsFix.asi - drop-in fix for Hogs of War PC ("Frontschweine"), loaded by Ultimate ASI Loader (as dinput.dll:
// GOG's IgnoreAltTab shim loads the system winmm.dll before the exe's imports, so a winmm.dll loader is not used there).
// Applies every fix in memory, nothing on disk is changed:
//   exe: no-CD Warhogs.exe (Sep 2000) or GOG's warhogs_.exe (Nov 2000, LaserLock 5): same code at shifted addresses
//   (FUN_ names below = no-CD exe; per-exe tables, bytes are verified, a group is skipped if anything differs):
//     CallDLL    warhogs_.exe only: call sites LaserLock routes through wh32LIB get their IAT call back (hooked imports)
//     NoCD       CD check FUN_0047f550 (InstallSource must be a CD-ROM with a read-only PIGS.ID) -> return true
//     Portable   install info FUN_00481a80: the game chdirs to InstallLocation from HKLM\...\Uninstall\Hogs Of War and
//                refuses to start without that key -> use the exe's folder, registry values only where present
//     AIFix      CPU pigs never move: inverted bounds check in the path request FUN_004618e0 (4 bytes)
//     WinFix     window creation disabled every other top-level window (EnumWindows -> EnableWindow(FALSE))
//     ResumeFix  menus froze for good after the window was minimized (alt-tab): "active" flag never set again
//     Widescreen launcher lists modes above 1024x768 (up to 2048 wide: the DX7 device limit)
//     Battle     battle loop: idle wait instead of busy spin, 60 FPS with interpolation (battle.c)
//     Music      CD audio tracks -> MUSIC\TrackNN.ogg (cdaudio.c: emulated MCI cdaudio + mixer CD line volume)
//     Gamepad    XInput controller with the PlayStation layout (gamepad.c)
//     MenuFPS    menu loop paced to 25 FPS (menu animations are per frame)
//     boot hook  0x47ef04 (sets the fullscreen flag) -> display_boot (display.c, native DirectDraw / windowed)
//   AltTab     warhogs_.exe only: stop the keyboard hook of GOG's IgnoreAltTab compatibility fix (goggame.sdb)
//   DpiAware   HIGHDPIAWARE layer, same relaunch (else the output is scaled by the desktop scale factor)
//   Shim16Bit  Windows only offers 16-bpp DirectDraw modes (launcher list, SetDisplayMode) to an exe with the
//              DWM8And16BitMitigation compatibility layer: if __COMPAT_LAYER lacks it, set it and relaunch the exe once
//              (the first process waits and returns the game's exit code; nothing is written to the registry)
//   Data\_d3d.dll (patched when the exe loads it, exe IAT LoadLibraryA hook):
//     Widescreen Hor+ focal length, 16-bpp mode limit 1024x768 -> 4096
//     NviewFix   terrain culling tables (Language\Tims\NVIEW00n.BMP) were read via LoadImage+GetBitmapBits, only
//                correct on a 16-bit desktop -> flickering/missing terrain blocks. Replaced by a direct BMP reader,
//                optionally dilated by NviewWiden cells for the wider Hor+ view.
//     Display    CopyToScreen windowed blit bMenu fix + display.c hooks
// Config: HogsFix.ini next to the .asi. Log: HogsFix.log.
#include "hogsfix.h"
#include <tlhelp32.h>

#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

int _fltused = 0;
#pragma function(memcpy, memset)
void *memcpy(void *d, const void *s, size_t n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { char *a = d; while (n--) *a++ = (char)c; return d; }

int cfg_log;
int cfg_battle = 1, cfg_interp = 1, cfg_hz = 60;
int cfg_display = 1, cfg_windowed, cfg_fastblit = 1;
int cfg_widescreen = 1, cfg_nviewfix = 1, cfg_nview_widen = 1;
static int cfg_menufps = 25;
static int cfg_aifix = 1, cfg_winfix = 1, cfg_resume = 1, cfg_nocd = 1, cfg_shim = 1, cfg_portable = 1, cfg_dpiaware = 1, cfg_alttab = 1, cfg_titlekey = 1;
DWORD d3d;
static HMODULE self;
static int exe_ok;
static char ini[MAX_PATH];

// ---- log ----
static HANDLE logf;
void dlog(const char *fmt, ...) {
    if (!cfg_log) return;
    if (!logf) {
        char p[MAX_PATH]; DWORD n = GetModuleFileNameA(self, p, MAX_PATH);
        while (n && p[n - 1] != '\\') n--;
        lstrcpyA(p + n, "HogsFix.log");
        logf = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        if (logf == INVALID_HANDLE_VALUE) { logf = 0; cfg_log = 0; return; }
    }
    char b[512]; int n = wsprintfA(b, "%8u ", timeGetTime());
    va_list a; va_start(a, fmt); n += wvsprintfA(b + n, fmt, a); va_end(a);
    b[n++] = '\r'; b[n++] = '\n'; DWORD w; WriteFile(logf, b, n, &w, NULL);
}

// ---- code patching helpers ----
void write_code(void *at, const void *src, int n) {
    DWORD old; VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    BYTE *d = at; const BYTE *s = src; for (int i = 0; i < n; i++) d[i] = s[i];
    VirtualProtect(at, n, old, &old); FlushInstructionCache(GetCurrentProcess(), at, n);
}
void write_jmp(DWORD at, void *to) {
    BYTE j[5] = { 0xe9 }; *(int *)(j + 1) = (int)((DWORD)to - (at + 5)); write_code((void *)at, j, 5);
}
static void write_call(DWORD at, void *to) {
    BYTE j[5] = { 0xe8 }; *(int *)(j + 1) = (int)((DWORD)to - (at + 5)); write_code((void *)at, j, 5);
}
// copy the first n bytes (whole instructions, position independent) of fn into a trampoline, jmp fn -> to
static BYTE *tramp_mem; static int tramp_used;
void *hook(DWORD fn, int n, void *to) {
    if (!tramp_mem) tramp_mem = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp_mem || tramp_used + n + 5 > 4096) return NULL;
    BYTE *t = tramp_mem + tramp_used; tramp_used += (n + 5 + 15) & ~15;
    for (int i = 0; i < n; i++) t[i] = ((BYTE *)fn)[i];
    t[n] = 0xe9; *(int *)(t + n + 1) = (int)((fn + n) - ((DWORD)t + n + 5));
    write_jmp(fn, to);
    return t;
}
void **find_iat(HMODULE mod, const char *dll, const char *name) {
    BYTE *b = (BYTE *)mod; IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR *id = (IMAGE_IMPORT_DESCRIPTOR *)(b + nt->OptionalHeader.DataDirectory[1].VirtualAddress);
    for (; id->Name; id++) {
        if (lstrcmpiA((char *)(b + id->Name), dll)) continue;
        IMAGE_THUNK_DATA *ot = (IMAGE_THUNK_DATA *)(b + (id->OriginalFirstThunk ? id->OriginalFirstThunk : id->FirstThunk));
        void **ft = (void **)(b + id->FirstThunk);
        for (; ot->u1.AddressOfData; ot++, ft++)
            if (!(ot->u1.Ordinal & IMAGE_ORDINAL_FLAG32) && !lstrcmpA((char *)((IMAGE_IMPORT_BY_NAME *)(b + ot->u1.AddressOfData))->Name, name))
                return ft;
    }
    return NULL;
}
void patch_iat(void **slot, void *fn) { if (slot) write_code(slot, &fn, 4); }

// ---- verified byte patches ----
// va = address at the preferred image base; rel: the dword at old/new + 2 is an absolute address inside the module
// (relocated when the module is not at its preferred base).
typedef struct { DWORD va; const char *old, *new; int rel; } Patch;
static int hexb(const char *s, BYTE *o) {
    int n = 0;
    for (; s[0] && s[1]; s += 2) {
        int h = s[0] <= '9' ? s[0] - '0' : (s[0] | 32) - 'a' + 10, l = s[1] <= '9' ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
        o[n++] = (BYTE)(h * 16 + l);
    }
    return n;
}
static int apply(const char *name, const Patch *p, int np, DWORD pref, DWORD base) {
    BYTE o[32], w[32]; int i, n;                         // patch strings up to 32 bytes
    DWORD delta = base - pref;
    for (i = 0; i < np; i++) {
        n = hexb(p[i].old, o);
        if (p[i].rel) *(DWORD *)(o + 2) += delta;
        BYTE *at = (BYTE *)(p[i].va + delta);
        for (int k = 0; k < n; k++) if (at[k] != o[k]) {
            dlog("%s: bytes at %08x differ, skipped (unknown version?)", name, p[i].va); return 0;
        }
    }
    for (i = 0; i < np; i++) {
        n = hexb(p[i].new, w);
        if (p[i].rel) *(DWORD *)(w + 2) += delta;
        if (n) write_code((void *)(p[i].va + delta), w, n);
    }
    dlog("%s: applied", name);
    return 1;
}

// exe patch groups: [0] = no-CD Warhogs.exe, [1] = GOG warhogs_.exe (settings pointer 0x520770 / 0x520668)
static const Patch P_NOCD[2][1] = {     // FUN_0047f550 CD check: mov eax, [settings] -> mov al, 1 / ret
    { { 0x47f550, "a170075200", "b001c3" } },
    { { 0x47f7c0, "a168065200", "b001c3" } },
};
static const Patch P_PORTABLE[2][1] = { // FUN_00481a80 prologue: mov eax, [settings] / add eax, 0xc / push eax
    { { 0x481a80, "a17007520083c00c50", "" } },
    { { 0x481d00, "a16806520083c00c50", "" } },
};
static const Patch P_AIFIX[2][4] = {    // FUN_004618e0: "destination outside the walk grid" test was inverted
    { { 0x46194a, "7e26", "7e1c" }, { 0x461951, "7d1f", "7d15" }, { 0x46195f, "7e11", "7e07" }, { 0x461966, "7d0a", "7c0a" } },
    { { 0x461aea, "7e26", "7e1c" }, { 0x461af1, "7d1f", "7d15" }, { 0x461aff, "7e11", "7e07" }, { 0x461b06, "7d0a", "7c0a" } },
};
static const Patch P_WINFIX[2][2] = {   // call EnumWindows (disable/enable all other windows; GOG: via CallDLL) -> add esp, 8
    { { 0x44d1b0, "ff15e4f55400", "83c408909090" }, { 0x47e816, "ff15e4f55400", "83c408909090" } },
    { { 0x44cfb0, "ff1538025500", "83c408909090" }, { 0x47ea86, "ff1538025500", "83c408909090" } },
};
static const Patch P_RESUME[2][1] = {   // WM_SIZE: cmp edi, 1 / jne / mov eax, [settings] / mov byte [eax+0x310], 0
    { { 0x47e7d3, "83ff01750ca170075200c6801003000000", "a17007520083ff010f958010030000" "9090" } },   // -> setne byte [eax+0x310]
    { { 0x47ea43, "83ff01750ca168065200c6801003000000", "a16806520083ff010f958010030000" "9090" } },
};
// launcher mode list: w <= 1024, h <= 768 -> 2048. Windows' DirectX 7 runtime refuses IDirect3D7::CreateDevice on
// render targets wider than 2048 (2560x1440, 3840x2160 -> DDERR_INVALIDOBJECT, "Failed to create D3DDevice").
static const Patch P_WS_EXE[2][2] = {
    { { 0x4814a6, "3d00040000", "3d00080000" }, { 0x4814c0, "81f900030000", "81f900080000" } },
    { { 0x481726, "3d00040000", "3d00080000" }, { 0x481740, "81f900030000", "81f900080000" } },
};
static const Patch P_HOOKSITES[2][2] = {    // call FUN_0047f6c0 (battle step), call FUN_0044e490 (render after a tick)
    { { 0x47e1d0, "e8eb140000", "" }, { 0x47f9c5, "e8c6eafcff", "" } },
    { { 0x47e42e, "e8fd140000", "" }, { 0x47fc49, "e842e6fcff", "" } },
};
// Title screen loop FUN_0045d030: call FUN_0044d6d0(back, surface, x, y, flags=0) for title.bmp and the three word
// sprites press/any/key.bmp (black text on white). Flags 0 = no colour key: the words were white boxes over the
// picture. -> title_blit: white is transparent for the word sprites.
static const Patch P_TITLE[2][1] = {
    { { 0x45d272, "e85904ffff", "" } },
    { { 0x45d229, "e8a202ffff", "" } },
};
static const DWORD TITLE_BLIT[2] = { 0x44d6d0, 0x44d4d0 };
static DWORD title_blit_fn;
static int __stdcall title_blit(void *dst, void *src, int x, int y, DWORD fl) {
    DWORD sd[31] = { 124 };
    if (src && !((HRESULT (__stdcall *)(void *, DWORD *))(*(DWORD **)src)[22])(src, sd) && sd[3] < 320) {   // GetSurfaceDesc: a word, not title.bmp
        DWORD white = sd[22] | sd[23] | sd[24], ck[2] = { white, white };                                      // R|G|B masks
        if (!((HRESULT (__stdcall *)(void *, DWORD, DWORD *))(*(DWORD **)src)[29])(src, 8, ck)) fl |= 0x8000;  // SetColorKey(SRCBLT) -> DDBLT_KEYSRC
    }
    return ((int (__stdcall *)(void *, void *, int, int, DWORD))title_blit_fn)(dst, src, x, y, fl);
}
// Gamepad (gamepad.c): call FUN_0044cdb0 (keyboard state) in the player input FUN_0045d750; push FUN_0044c900
// (DirectInput joystick enumeration callback) in FUN_0044cac0
static const Patch P_PAD[2][2] = {
    { { 0x45d77d, "e82ef6feff", "" }, { 0x44cade, "6800c94400", "" } },
    { { 0x45d72d, "e87ef4feff", "" }, { 0x44c8de, "6800c74400", "" } },
};
// Menu loop FUN_00427340: renders as fast as it can; the blinking lamps, smoke puffs etc. advance once per frame (the
// game's own menu timer is 40 ms = 25 Hz, the PAL PlayStation rate). With fast menus (FastBlit, 60+ Hz) they ran 2-3x
// too fast. call FUN_004ae320 (clock, start of each menu frame) -> menu_pace: waits for the next MenuFPS slot.
static const Patch P_MENUFPS[2][1] = {
    { { 0x42741f, "e8fc6e0800", "" } },
    { { 0x42745f, "e8cc700800", "" } },
};
static void pace(DWORD *next, int fps) {    // *next: microseconds in the timeGetTime()*1000 domain
    DWORD q = 1000000 / fps, now = timeGetTime() * 1000;
    if ((int)(now - *next) > (int)q || (int)(*next - now) > (int)q) *next = now;   // first frame / stall
    while ((int)(*next - now) > 0) { Sleep((*next - now) / 1000 >= 2 ? 1 : 0); now = timeGetTime() * 1000; }
    *next += q;
}
static DWORD menu_clock_fn, menu_next;
static int __cdecl menu_pace(void) { pace(&menu_next, cfg_menufps); return ((int (__cdecl *)(void))menu_clock_fn)(); }
// (the results/debrief screens FUN_00482xxx-00484xxx animate by elapsed time: no pacing needed)
static const Patch P_BOOT[2][1] = {     // mov [edx+0x314], ebx / mov eax, [settings]
    { { 0x47ef04, "899a14030000a170075200", "" } },
    { { 0x47f174, "899a14030000a168065200", "" } },
};
// warhogs_.exe: LaserLock routes ~315 API calls through wh32LIB's CallDLL (IAT 0x550238) instead of the exe's IAT;
// CallDLL identifies the API by the return address. The call sites of the imports HogsFix hooks in the IAT get their
// plain "call [IAT slot]" back (same 6 bytes). All other routed calls are left alone.
typedef struct { const char *dll, *name; DWORD site[11]; } Routed;
static const Routed GOG_ROUTED[] = {
    { "kernel32.dll", "LoadLibraryA", { 0x4811bf, 0x4adb03, 0x4b79c3 } },
    { "user32.dll", "PeekMessageA", { 0x47e38e, 0x47e420, 0x480794, 0x480840 } },
    { "binkw32.dll", "_BinkCopyToBuffer@28", { 0x43f114 } },
    { "winmm.dll", "mciSendCommandA", { 0x45f924, 0x45f961, 0x45f9a1, 0x45fa51, 0x45fa99, 0x45fae9, 0x45fc0b, 0x45fc2e, 0x45fc54, 0x45fc84, 0x45fcc9 } },
    { "winmm.dll", "mixerGetNumDevs", { 0x45fce7 } },
    { "winmm.dll", "mixerGetDevCapsA", { 0x45fd08 } },
    { "winmm.dll", "mixerOpen", { 0x45fd2c } },
    { "winmm.dll", "mixerGetLineInfoA", { 0x45fd5e } },
    { "winmm.dll", "mixerGetLineControlsA", { 0x45fdd9 } },
    { "winmm.dll", "mixerSetControlDetails", { 0x45fe9b } },
    { "winmm.dll", "mixerGetControlDetailsA", { 0x45fef4 } },
    { "winmm.dll", "mixerClose", { 0x45fe16 } },
};
#define GOG_CALLDLL 0x550238

typedef struct {
    Exe e;
    DWORD timestamp, imagesize;
    DWORD install, regstr, regdword, launchstr;     // Portable: FUN_00481a80 and its helpers
    DWORD step_site, render_site, boot_site;
} ExeDef;
static const ExeDef EXES[2] = {
    { { "no-CD Warhogs.exe (Sep 2000)", 0x520770, 0x520968, 0x520944, 0x5381cc, 0x51f228,
        0x52098c, 0x538034, 0x51c608, 0x5380a8, 0x5380cc, 0x51d49c, 0x51d4d4, 0x51d4d0,
        0x47f6c0, 0x44e490, 0x44e010, 0x44d240, 0x44d8c0, 0x45c430, 0x44dc70, 0x481046, 0x51f228, 0x51be04 },
      0x39c903e8, 0x15e000, 0x481a80, 0x47f450, 0x47f4d0, 0x481b00, 0x47e1d0, 0x47f9c5, 0x47ef04 },
    { { "GOG warhogs_.exe (Nov 2000, LaserLock)", 0x520668, 0x520860, 0x52083c, 0x5380c0, 0x51f120,
        0x520884, 0x537f24, 0x51c4f8, 0x537f98, 0x537fbc, 0x51d38c, 0x51d3c4, 0x51d3c0,
        0x47f930, 0x44e290, 0x44de10, 0x44d040, 0x44d6c0, 0x45c3d0, 0x44da70, 0x4812c6, 0x51f120, 0x51bcf0 },
      0x3a069f75, 0x15e000, 0x481d00, 0x47f6c0, 0x47f740, 0x481d80, 0x47e42e, 0x47fc49, 0x47f174 },
};
static const ExeDef *XD;
const Exe *X;
static DWORD settings_va;                     // X->settings for boot_stub

static void reroute_calldll(void) {
    HMODULE exe = GetModuleHandleA(NULL); int n = 0, bad = 0;
    for (int i = 0; i < ARRAYSIZE(GOG_ROUTED); i++) {
        void **slot = find_iat(exe, GOG_ROUTED[i].dll, GOG_ROUTED[i].name);
        for (int k = 0; k < ARRAYSIZE(GOG_ROUTED[i].site) && GOG_ROUTED[i].site[k]; k++) {
            BYTE *at = (BYTE *)GOG_ROUTED[i].site[k];
            if (!slot || at[0] != 0xff || at[1] != 0x15 || *(DWORD *)(at + 2) != GOG_CALLDLL) { bad++; continue; }
            write_code(at + 2, &slot, 4); n++;
        }
    }
    dlog("LaserLock CallDLL: %d call sites rerouted to the IAT, %d differ (skipped)", n, bad);
}

static const Patch P_WS_D3D[] = {       // fx = h*zoom*0.0667 instead of w*zoom*0.05 (4 sites); 16 bpp mode limit
    { 0x10004baf, "8bd9", "8bde" },         { 0x10004bf1, "d80db8000210", "d80dbc000210", 1 },
    { 0x10005e5b, "89542410", "894c2410" }, { 0x10005e6e, "d80db8000210", "d80dbc000210", 1 },
    { 0x1000768b, "89742424", "897c2424" }, { 0x100076ab, "d80db8000210", "d80dbc000210", 1 },
    { 0x10009533, "897c2408", "89542408" }, { 0x1000954a, "d80db8000210", "d80dbc000210", 1 },
    { 0x10006be3, "813800040000", "813800100000" }, { 0x10006beb, "81780400030000", "81780400100000" },
};
static const Patch P_CTS_D3D[] = {      // CopyToScreen windowed: AdjustWindowRectEx(bMenu=TRUE) -> FALSE
    { 0x1000321c, "6a01", "6a00" },
};
static const Patch P_NVIEW_D3D[] = {    // FUN_10014120 prologue: sub esp, 0x100
    { 0x10014120, "81ec00010000", "" },
};

// ---- ResumeFix ----
// The window procedure 0x47e7a0 clears the "active" flag [0x520770]+0x310 on WM_SIZE(SIZE_MINIMIZED) and nothing ever
// sets it again: after a minimize (alt-tab out of exclusive fullscreen minimizes the window) menus and the pause screen
// stopped rendering for good while the menu loop FUN_00427340 spun PeekMessage on one core. The patch sets the flag from
// every WM_SIZE (setne: 0 when minimized, 1 when restored); while the window is minimized the exe's PeekMessage waits
// for the next message (max 50 ms) instead of spinning.
static BOOL (WINAPI *real_peek)(MSG *, HWND, UINT, UINT, UINT);
static BOOL WINAPI my_peek(MSG *m, HWND h, UINT a, UINT b, UINT rm) {
    HWND g = (HWND)U32(X->hwnd);
    if (g && IsIconic(g)) MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
    return real_peek(m, h, a, b, rm);
}

// ---- AltTab ----
// GOG's goggame.sdb applies the Windows compatibility fix IgnoreAltTab to warhogs_.exe: AcGenral.dll starts a thread
// that installs a low-level keyboard hook swallowing Alt+Tab, Alt+Esc, Ctrl+Esc and the Windows/menu keys, and pumps
// messages for it. Its parameter block {HHOOK, thread handle, thread id} is a heap block pointed to from AcGenral's data:
// found via the thread id, the hook is removed and WM_QUIT ends the thread.
static int readable(DWORD a, DWORD n) {
    MEMORY_BASIC_INFORMATION mi;
    return a && VirtualQuery((void *)a, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        && a + n <= (DWORD)mi.BaseAddress + mi.RegionSize;
}
static int allow_alttab(int quiet) {   // quiet: relaunch parent (the log belongs to the game process)
    HMODULE ac = GetModuleHandleA("AcGenral.dll");
    if (!ac) return -1;
    typedef LONG (WINAPI *qit_t)(HANDLE, int, void *, ULONG, ULONG *);
    qit_t qit = (qit_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (!qit || snap == INVALID_HANDLE_VALUE) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)ac + ((IMAGE_DOS_HEADER *)ac)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    THREADENTRY32 te; te.dwSize = sizeof te; int n = 0, removed = 0;
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID); DWORD start = 0;
        if (!t) continue;
        qit(t, 9, &start, sizeof start, NULL);                  // ThreadQuerySetWin32StartAddress
        CloseHandle(t);
        HMODULE m = NULL;
        if (!start || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)start, &m) || m != ac) continue;
        int unhooked = 0;
        for (int i = 0; i < nt->FileHeader.NumberOfSections && !unhooked; i++) {
            if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            DWORD *p = (DWORD *)((BYTE *)ac + sec[i].VirtualAddress), *e = p + sec[i].Misc.VirtualSize / 4;
            for (; p < e && !unhooked; p++)
                if (readable(*p, 12) && ((DWORD *)*p)[2] == te.th32ThreadID && ((DWORD *)*p)[0]) {
                    HHOOK hk = (HHOOK)((DWORD *)*p)[0];
                    unhooked = UnhookWindowsHookEx(hk) ? 1 : -1;
                    if (!quiet) dlog("AltTab: UnhookWindowsHookEx(%08x) %s", (DWORD)hk, unhooked > 0 ? "ok" : "failed");
                }
        }
        if (unhooked > 0) removed++;
        if (PostThreadMessageA(te.th32ThreadID, WM_QUIT, 0, 0)) n++;
        if (!quiet) dlog("AltTab: IgnoreAltTab keyboard hook thread %u (AcGenral.dll+%x) stopped%s", te.th32ThreadID, start - (DWORD)ac,
             unhooked > 0 ? "" : ", hook block not found");
    }
    CloseHandle(snap);
    if (!n && !quiet) dlog("AltTab: AcGenral.dll loaded, no hook thread found");
    return removed;
}

// ---- install info (replaces FUN_00481a80) ----
// settings [0x520770]: +0xc InstallSource, +0x10c InstallLocation (the game's working directory), +0x20c LanguageLetter
// (launcher/message language E/G/F/I/S), +0x30c Footprint (dword). Registry readers: 0x47f450 string, 0x47f4d0 dword.
static BYTE __cdecl install_info(void) {
    typedef BYTE (__cdecl *rq_t)(const char *, BYTE *);
    BYTE *st = *(BYTE **)X->settings; rq_t rs = (rq_t)XD->regstr, rd = (rq_t)XD->regdword;
    if (!rs("InstallSource", st + 0xc)) lstrcpyA((char *)st + 0xc, ".\\");
    if (!rs("LanguageLetter", st + 0x20c)) lstrcpyA((char *)st + 0x20c, "E");
    if (!rd("Footprint", st + 0x30c)) *(DWORD *)(st + 0x30c) = 1;
    char *loc = (char *)st + 0x10c; DWORD n = GetModuleFileNameA(NULL, loc, 256);
    while (n && loc[n - 1] != '\\') n--;
    loc[n] = 0;
    ((void (__cdecl *)(void))XD->launchstr)();           // launcher strings for the language
    dlog("install folder %s, language %c", loc, st[0x20c]);
    return 1;
}

// ---- NVIEW culling tables ----
// 10 tables (one per camera angle) of 1024 shorts at _d3d 0x11ed0188: cell i = row i/33, column i%33 of the 33x33
// 8-bit BMP, top row first; 0 = draw the 4x4-tile block (black pixel), 0xffff = cull.
#define NV_TABLES 0x11ed0188
#define NV_FMT    0x1002f07c
static void __cdecl nview_load(void) {
    WORD *tab = (WORD *)(d3d + NV_TABLES - 0x10000000);
    const char *fmt = (const char *)(d3d + NV_FMT - 0x10000000);
    static BYTE buf[4096]; static BYTE vis[33][33], dil[33][33];
    int ok = 0;
    for (int t = 0; t < 10; t++, tab += 1024) {
        for (int i = 0; i < 1024; i++) tab[i] = 0xffff;
        char path[MAX_PATH]; wsprintfA(path, fmt, t);
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) { dlog("NVIEW: cannot open %s", path); continue; }
        DWORD n = 0; ReadFile(f, buf, sizeof buf, &n, NULL); CloseHandle(f);
        DWORD off = *(DWORD *)(buf + 10), hs = *(DWORD *)(buf + 14);
        if (n < 54 || *(WORD *)buf != 0x4d42 || *(WORD *)(buf + 28) != 8 || *(int *)(buf + 18) != 33 || *(int *)(buf + 22) != 33
            || *(DWORD *)(buf + 30) != 0 || off + 36 * 33 > n || 14 + hs + 1024 > n) { dlog("NVIEW: %s not a 33x33 8-bit BMP", path); continue; }
        DWORD *pal = (DWORD *)(buf + 14 + hs);
        for (int y = 0; y < 33; y++)
            for (int x = 0; x < 33; x++)
                vis[y][x] = !(pal[buf[off + 36 * (32 - y) + x]] & 0xf8fcf8);   // bottom-up rows
        int w = cfg_nview_widen; if (w < 0) w = 0; if (w > 4) w = 4;
        int nv = 0, nd = 0;
        for (int y = 0; y < 33; y++)
            for (int x = 0; x < 33; x++) {
                int v = 0;
                for (int dy = -w; dy <= w && !v; dy++)
                    for (int dx = -w; dx <= w; dx++) {
                        int yy = y + dy, xx = x + dx;
                        if (yy >= 0 && yy < 33 && xx >= 0 && xx < 33 && vis[yy][xx]) { v = 1; break; }
                    }
                dil[y][x] = (BYTE)v; nv += vis[y][x]; nd += v;
            }
        for (int i = 0; i < 1024; i++) if (dil[i / 33][i % 33]) tab[i] = 0;
        if (t == 0) dlog("NVIEW: table 0 visible blocks %d -> %d (widen %d)", nv, nd, w);
        ok++;
    }
    dlog("NVIEW: %d/10 tables loaded", ok);
}

// ---- _d3d.dll ----
static void on_d3d(HMODULE m) {
    if (d3d) return;
    d3d = (DWORD)m;
    dlog("_d3d.dll loaded at %08x", d3d);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(d3d + ((IMAGE_DOS_HEADER *)d3d)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x3a069f61) dlog("_d3d.dll: unknown version (timestamp %08x), patches verified byte by byte", nt->FileHeader.TimeDateStamp);
    if (cfg_widescreen) apply("Widescreen (_d3d Hor+)", P_WS_D3D, ARRAYSIZE(P_WS_D3D), 0x10000000, d3d);
    if (cfg_nviewfix && apply("NviewFix", P_NVIEW_D3D, ARRAYSIZE(P_NVIEW_D3D), 0x10000000, d3d))
        write_jmp(d3d + 0x14120, nview_load);
    if (cfg_display || cfg_windowed) {
        apply("Display (_d3d CopyToScreen)", P_CTS_D3D, ARRAYSIZE(P_CTS_D3D), 0x10000000, d3d);
        display_hook_d3d(m);
    }
}
static HMODULE (WINAPI *real_ll)(LPCSTR);
static HMODULE WINAPI my_ll(LPCSTR name) {
    HMODULE m = real_ll(name);
    if (m && name) {
        int n = lstrlenA(name);
        if (n >= 8 && !lstrcmpiA(name + n - 8, "_d3d.dll")) on_d3d(m);
    }
    return m;
}

// ---- boot: the game sets its fullscreen flag ([0x520770]+0x314 = ebx); runs after the exe's CRT init ----
static DWORD __stdcall boot(DWORD settings, DWORD fs) {
    static int booted;
    if (!booted) {
        booted = 1;
        dlog("boot");
        if (cfg_alttab) allow_alttab(0);
        void **ll = find_iat(GetModuleHandleA(NULL), "kernel32.dll", "LoadLibraryA");
        real_ll = ll ? *ll : (void *)LoadLibraryA;
        patch_iat(ll, (void *)my_ll);
        if (!ll) dlog("exe LoadLibraryA import not found: _d3d.dll patches disabled");
        HMODULE m = GetModuleHandleA("_d3d.dll"); if (m) on_d3d(m);
        if (cfg_display || cfg_windowed) display_boot();
    }
    return cfg_windowed ? 0 : fs;
}
static __declspec(naked) void boot_stub(void) {
    __asm {
        pushad
        push ebx
        mov eax, settings_va
        push dword ptr [eax]
        call boot
        mov edx, settings_va
        mov edx, dword ptr [edx]
        mov dword ptr [edx + 0x314], eax
        popad
        ret
    }
}

static void load_cfg(void) {
    DWORD n = GetModuleFileNameA(self, ini, MAX_PATH);
    while (n && ini[n - 1] != '\\') n--;
    lstrcpyA(ini + n, "HogsFix.ini");
#define GI(sec, key, def) GetPrivateProfileIntA(sec, key, def, ini)
    cfg_log = GI("General", "Log", 0);
    cfg_nocd = GI("Fixes", "NoCD", 1);
    cfg_portable = GI("Fixes", "Portable", 1);
    cfg_aifix = GI("Fixes", "AIFix", 1);
    cfg_winfix = GI("Fixes", "WinFix", 1);
    cfg_resume = GI("Fixes", "ResumeFix", 1);
    cfg_alttab = GI("Fixes", "AltTab", 1);
    cfg_titlekey = GI("Fixes", "TitleText", 1);
    cfg_nviewfix = GI("Fixes", "NviewFix", 1);
    cfg_widescreen = GI("Widescreen", "Enable", 1);
    cfg_nview_widen = GI("Widescreen", "NviewWiden", 1);
    if (!cfg_widescreen) cfg_nview_widen = 0;
    cfg_battle = GI("Battle", "Enable", 1);
    cfg_hz = GI("Battle", "FrameHz", 60);
    cfg_interp = GI("Battle", "Interp", 1);
    if (cfg_hz < 0 || cfg_hz > 1000) cfg_hz = 60;
    cfg_display = GI("Display", "Fix", 1);
    cfg_windowed = GI("Display", "Windowed", 0);
    cfg_shim = GI("Display", "Shim16Bit", 1);
    cfg_dpiaware = GI("Display", "DpiAware", 1);
    cfg_fastblit = GI("Display", "FastBlit", 1);
    cfg_menufps = GI("Display", "MenuFPS", 25);
    if (cfg_menufps < 0 || cfg_menufps > 1000) cfg_menufps = 25;
    cfg_pad = GI("Gamepad", "Enable", 1);
    cfg_pad_dz = GI("Gamepad", "Deadzone", 30);
    if (cfg_pad_dz < 5 || cfg_pad_dz > 90) cfg_pad_dz = 30;
    cfg_music = GI("Music", "Enable", 1);
    cfg_music_vol = GI("Music", "Volume", 100);
    if (cfg_music_vol < 0) cfg_music_vol = 0;
    if (cfg_music_vol > 100) cfg_music_vol = 100;
}

// ---- 16-bit compatibility layer: relaunch with __COMPAT_LAYER (before anything is logged: the log is not shared) ----
static int has_word(const char *s, const char *w) {
    int n = lstrlenA(w);
    for (; *s; s++) {
        int i = 0;
        while (i < n && s[i] && (s[i] | 32) == (w[i] | 32)) i++;
        if (i == n) return 1;
    }
    return 0;
}
// Layers must be active from process start: SetProcessDPIAware() at this point leaves the 16-bit emulation scaling its
// fullscreen output by the desktop scale factor (640x480 menus shrunk to 1/1.75). Windows may also add its own
// DWM8And16BitMitigation entry for the exe (Program Compatibility Assistant) - then only HIGHDPIAWARE is missing.
static void relaunch_with_layers(void) {
    static const char L16[] = "DWM8And16BitMitigation", LDPI[] = "HIGHDPIAWARE";
    char v[1024], nv[1100], m[4]; DWORD n = GetEnvironmentVariableA("__COMPAT_LAYER", v, sizeof v);
    if (n >= sizeof v || GetEnvironmentVariableA("HOGSFIX_RELAUNCHED", m, sizeof m)) return;   // no relaunch loops
    if (!n) v[0] = 0;
    int add16 = cfg_shim && !has_word(v, L16), adddpi = cfg_dpiaware && !has_word(v, LDPI);
    if (!add16 && !adddpi) return;
    wsprintfA(nv, "%s%s%s%s%s", v, n && add16 ? " " : "", add16 ? L16 : "", (n || add16) && adddpi ? " " : "", adddpi ? LDPI : "");
    SetEnvironmentVariableA("__COMPAT_LAYER", nv);
    SetEnvironmentVariableA("HOGSFIX_RELAUNCHED", "1");
    STARTUPINFOA si; GetStartupInfoA(&si); si.cb = sizeof si; si.lpReserved = NULL; si.cbReserved2 = 0; si.lpReserved2 = NULL;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, GetCommandLineA(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        SetEnvironmentVariableA("__COMPAT_LAYER", n ? v : NULL);
        SetEnvironmentVariableA("HOGSFIX_RELAUNCHED", NULL);
        return;                                       // run without the layers (launcher may show no modes)
    }
    CloseHandle(pi.hThread);
    // the hook is desktop-wide: this waiting process has one too (retried in case its thread has not installed it yet)
    for (int i = 0; cfg_alttab && i < 20 && allow_alttab(1) == 0; i++) if (!WaitForSingleObject(pi.hProcess, 500)) break;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    TerminateProcess(GetCurrentProcess(), code);      // still inside DllMain: no ExitProcess/DLL detach
}

static void init(void) {
    load_cfg();
    relaunch_with_layers();
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    BYTE *b = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    char layer[256]; if (!GetEnvironmentVariableA("__COMPAT_LAYER", layer, sizeof layer) || lstrlenA(layer) > 200) lstrcpyA(layer, "-");
    dlog("HogsFix " __DATE__ " in %s", exe);
    dlog("config: Log=%d NoCD=%d Portable=%d AIFix=%d WinFix=%d ResumeFix=%d AltTab=%d NviewFix=%d Widescreen=%d NviewWiden=%d Battle=%d FrameHz=%d Interp=%d Display=%d Windowed=%d Shim16Bit=%d DpiAware=%d FastBlit=%d MenuFPS=%d Music=%d/%d%% Gamepad=%d/%d%% __COMPAT_LAYER=%s",
         cfg_log, cfg_nocd, cfg_portable, cfg_aifix, cfg_winfix, cfg_resume, cfg_alttab, cfg_nviewfix, cfg_widescreen, cfg_nview_widen, cfg_battle, cfg_hz, cfg_interp, cfg_display, cfg_windowed, cfg_shim, cfg_dpiaware, cfg_fastblit, cfg_menufps, cfg_music, cfg_music_vol, cfg_pad, cfg_pad_dz, layer);
    for (int i = 0; i < ARRAYSIZE(EXES); i++)
        if ((DWORD)b == 0x400000 && nt->FileHeader.TimeDateStamp == EXES[i].timestamp && nt->OptionalHeader.SizeOfImage == EXES[i].imagesize) XD = &EXES[i];
    if (!XD) {
        dlog("unsupported exe (base %08x, timestamp %08x, image size %x): use GOG's warhogs_.exe or the no-CD Warhogs.exe of the Sep 2000 retail build. Nothing patched.",
             (DWORD)b, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        return;
    }
    X = &XD->e; settings_va = X->settings;
    int g = XD != &EXES[0];
    dlog("exe: %s", X->name);
    exe_ok = 1;
    if (g) reroute_calldll();
    if (cfg_nocd) apply("NoCD", P_NOCD[g], ARRAYSIZE(P_NOCD[g]), 0, 0);
    if (cfg_portable && apply("Portable", P_PORTABLE[g], ARRAYSIZE(P_PORTABLE[g]), 0, 0)) write_jmp(XD->install, install_info);
    if (cfg_aifix) apply("AIFix", P_AIFIX[g], ARRAYSIZE(P_AIFIX[g]), 0, 0);
    if (cfg_winfix) apply("WinFix", P_WINFIX[g], ARRAYSIZE(P_WINFIX[g]), 0, 0);
    if (cfg_resume && apply("ResumeFix", P_RESUME[g], ARRAYSIZE(P_RESUME[g]), 0, 0)) {
        void **pm = find_iat(GetModuleHandleA(NULL), "user32.dll", "PeekMessageA");
        if (pm) { real_peek = *pm; patch_iat(pm, (void *)my_peek); }
    }
    if (cfg_titlekey && apply("TitleText", P_TITLE[g], ARRAYSIZE(P_TITLE[g]), 0, 0)) {
        title_blit_fn = TITLE_BLIT[g];
        write_call(P_TITLE[g][0].va, (void *)title_blit);
    }
    if (cfg_menufps && apply("MenuFPS", P_MENUFPS[g], ARRAYSIZE(P_MENUFPS[g]), 0, 0)) {
        timeBeginPeriod(1);
        menu_clock_fn = P_MENUFPS[g][0].va + 5 + *(int *)(P_MENUFPS[g][0].va + 1);
        write_call(P_MENUFPS[g][0].va, (void *)menu_pace);
    }
    if (cfg_widescreen) apply("Widescreen (launcher modes)", P_WS_EXE[g], ARRAYSIZE(P_WS_EXE[g]), 0, 0);
    if (cfg_battle && apply("Battle loop hooks", P_HOOKSITES[g], ARRAYSIZE(P_HOOKSITES[g]), 0, 0)) {
        battle_init();
        write_call(XD->step_site, (void *)hf_step);
        write_call(XD->render_site, (void *)hf_render_tick);
    }
    if (cfg_music) cdaudio_init();
    if (cfg_pad && apply("Gamepad", P_PAD[g], ARRAYSIZE(P_PAD[g]), 0, 0)) gamepad_init(P_PAD[g][0].va, P_PAD[g][1].va);
    if (apply("Boot hook", P_BOOT[g], ARRAYSIZE(P_BOOT[g]), 0, 0)) {
        static const BYTE tail[6] = { 0x8b, 0xc2, 0x90, 0x90, 0x90, 0x90 };   // mov eax, edx (= [settings])
        write_call(XD->boot_site, (void *)boot_stub);
        write_code((void *)(XD->boot_site + 5), tail, 6);
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void *res) {
    if (reason == DLL_PROCESS_ATTACH) { self = h; DisableThreadLibraryCalls(h); init(); }
    return TRUE;
}
