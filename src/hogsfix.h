// HogsFix.asi - shared declarations (hogsfix.c = loader/patches/NVIEW, battle.c = 60 FPS loop, display.c = DirectDraw)
#include <windows.h>

#define U32(a)   (*(volatile DWORD *)(a))
#define I32(a)   (*(volatile int *)(a))

// HogsFix.ini
extern int cfg_log;
extern int cfg_battle, cfg_interp, cfg_hz;            // [Battle] Enable, Interp, FrameHz
extern int cfg_display, cfg_windowed, cfg_fastblit;                 // [Display] Fix, Windowed
extern int cfg_widescreen, cfg_nviewfix, cfg_nview_widen;
extern DWORD d3d;                                     // _d3d.dll base (0 until loaded and hooked)

// Supported exes: the no-CD Warhogs.exe (retail Sep 2000) and GOG's warhogs_.exe (Nov 2000, LaserLock). Same game
// code at shifted addresses; every exe address used outside hogsfix.c comes from this table.
typedef struct {
    const char *name;
    DWORD settings;     // ptr: +0xc InstallSource, +0x10c InstallLocation, +0x310 active, +0x314 fullscreen, +0x44c/+0x450 resolution
    DWORD hwnd, surfcache, getsurf, colorkey;                         // display.c globals
    DWORD tick_ts, tmptr, rendobj, camptr, objlist, hudptr, msgptr, msgtick;   // battle.c globals
    DWORD step, render, refresh, sizewin, gdiblt, paint, loadbmp, launch_fs;   // functions / patch sites
    DWORD frontend, dinput;                                           // gamepad.c: menu active flag, IDirectInput7
} Exe;
extern const Exe *X;

void dlog(const char *fmt, ...);
void write_code(void *at, const void *src, int n);
void write_jmp(DWORD at, void *to);
void *hook(DWORD fn, int n, void *to);
void **find_iat(HMODULE mod, const char *dll, const char *name);
void patch_iat(void **slot, void *fn);

// battle.c
DWORD __cdecl hf_step(void);
void __fastcall hf_render_tick(DWORD robj);
void battle_init(void);
// cdaudio.c
extern int cfg_music, cfg_music_vol;                 // [Music] Enable, Volume
void cdaudio_init(void);
// gamepad.c
extern int cfg_pad, cfg_pad_dz;                      // [Gamepad] Enable, Deadzone
void gamepad_init(DWORD kb_site, DWORD enum_site);
// display.c
void display_boot(void);
void display_hook_d3d(HMODULE m);
