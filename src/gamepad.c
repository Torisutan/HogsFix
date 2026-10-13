// HogsFix - gamepad: XInput controllers (Xbox 360/One/Series and compatibles) with the PlayStation layout.
//
// The PC game has two input paths:
//   battle  FUN_0045d750 (human player, vtable 0x4bc918 slot 2) reads the DirectInput keyboard (GetDeviceState via
//           FUN_0044cdb0) and builds a 16-bit pad mask in PlayStation order from the player's key bindings
//           (user+0x108: 16 DIK codes, set in Optionen -> Steuerung). bits: 0 up, 1 down, 2 left, 3 right, 4 triangle,
//           5 cross, 6 square, 7 circle, 8 L1, 9 L2, 10 R2, 11 R1, 12 L3, 13 R3, 14 start, 15 select.
//           -> the call is wrapped: the pad's buttons set the DIK bytes of the bound keys, so the game's own logic
//           decides what each button means in each situation (exactly like the PlayStation version). Pause menu and
//           weapon menu run on this mask too.
//   menus   the window procedure 0x47e7a0 handles WM_KEYDOWN while the frontend [0x51f228] is active (Return, Escape,
//           Backspace, arrows; a key-up re-arms it). Outside the menus it only takes Space/Escape/Backspace while a
//           debrief screen waits (settings +0x479: Space = continue, Escape = retry / team screen). Escape also skips the intro movies ([0x520950]). Backspace = back (Escape also
//           goes back, but jumps to the top entry and confirms "Programm wirklich beenden?").
//           -> key down/up pairs are posted to the game window (arrows with auto-repeat).
// The game's own DirectInput joystick support (first joystick found, FUN_0044c900 enumeration callback) would also see
// an XInput pad, with a different button order (triangle = LB, start = right stick click, ...): XInput devices are
// hidden from that enumeration. Other (DirectInput-only) joysticks stay with the game's own support.
// XInput comes from Windows (xinput1_4.dll, else xinput9_1_0.dll); slots are re-scanned once per second (hot-plug).
// Test input: a named file mapping "HogsFixPadTest" (XINPUT_GAMEPAD, 12 bytes) acts as a connected pad.
// Config: HogsFix.ini [Gamepad] Enable, Deadzone (percent of the stick range).
#include "hogsfix.h"

int cfg_pad = 1, cfg_pad_dz = 30;

typedef struct { WORD b; BYTE lt, rt; SHORT lx, ly, rx, ry; } XPad;
typedef struct { DWORD pkt; XPad g; } XState;
static DWORD (WINAPI *xget)(DWORD, XState *);

enum { P_UP = 1, P_DOWN = 2, P_LEFT = 4, P_RIGHT = 8, P_TRI = 0x10, P_CROSS = 0x20, P_SQUARE = 0x40, P_CIRCLE = 0x80,
       P_L1 = 0x100, P_L2 = 0x200, P_R2 = 0x400, P_R1 = 0x800, P_L3 = 0x1000, P_R3 = 0x2000, P_START = 0x4000, P_SELECT = 0x8000 };
// XInput button -> PlayStation button at the same place
static const WORD XMAP[16] = {
    P_UP, P_DOWN, P_LEFT, P_RIGHT,      // d-pad
    P_START, P_SELECT, P_L3, P_R3,      // Start/Menu, Back/View, stick clicks
    P_L1, P_R1, 0, 0,                   // LB, RB
    P_CROSS, P_CIRCLE, P_SQUARE, P_TRI, // A, B, X, Y
};

static int slot = -1;                   // connected XInput slot, -1 none
static DWORD next_scan, last_poll;
static XPad *testpad; static HANDLE testmap;
static WORD cur, prev, latched;         // PlayStation bits now / at the last poll / pressed since the last battle read
static DWORD rep_at; static WORD rep_bit;

static WORD to_psx(const XPad *p) {
    WORD m = 0; int dz = cfg_pad_dz * 327;
    for (int i = 0; i < 16; i++) if (p->b & (1 << i)) m |= XMAP[i];
    if (p->lt > 64) m |= P_L2;
    if (p->rt > 64) m |= P_R2;
    if (p->ly > dz) m |= P_UP;
    if (p->ly < -dz) m |= P_DOWN;
    if (p->lx < -dz) m |= P_LEFT;
    if (p->lx > dz) m |= P_RIGHT;
    if (p->ry > 16000) m |= P_R1;       // right stick up/down: trajectory (R1/R2 = Page Up/Down)
    if (p->ry < -16000) m |= P_R2;
    return m;
}

static void post_key(HWND h, UINT vk) {
    UINT sc = MapVirtualKeyA(vk, 0); DWORD ext = (vk >= VK_PRIOR && vk <= VK_DOWN) ? 1 << 24 : 0;
    PostMessageA(h, WM_KEYDOWN, vk, 1 | sc << 16 | ext);
    PostMessageA(h, WM_KEYUP, vk, 0xc0000001 | sc << 16 | ext);
}

static void read_pad(void) {
    DWORD now = GetTickCount(); XState s;
    if (!testpad && (int)(now - next_scan) >= 0) {
        next_scan = now + 1000;
        if (!testmap && (testmap = OpenFileMappingA(FILE_MAP_READ, FALSE, "HogsFixPadTest")))
            if ((testpad = MapViewOfFile(testmap, FILE_MAP_READ, 0, 0, sizeof(XPad)))) dlog("Gamepad: test input HogsFixPadTest");
        if (!testpad && slot < 0 && xget)
            for (int i = 0; i < 4; i++) if (!xget(i, &s)) { slot = i; dlog("Gamepad: XInput controller in slot %d", i); break; }
    }
    if (testpad) { cur = to_psx(testpad); return; }
    if (slot < 0) { cur = 0; return; }
    if (xget(slot, &s)) { dlog("Gamepad: XInput controller %d disconnected", slot); slot = -1; next_scan = now; cur = 0; return; }
    cur = to_psx(&s.g);
}

// once per few ms from PeekMessage (menu loops, movies, battle loop) and from the battle input read
static void poll(void) {
    DWORD now = timeGetTime();
    if (now - last_poll < 4) return;
    last_poll = now;
    read_pad();
    HWND h = (HWND)U32(X->hwnd);
    if (!h || GetForegroundWindow() != h) cur = 0;
    WORD down = cur & ~prev; prev = cur;
    if (!h) return;
    if (U32(X->frontend)) {             // menus: cross/start = Return, circle/triangle = Backspace (back), arrows repeat
        latched = 0;                    // menu presses must not reach the next battle read
        static const struct { WORD bit; BYTE vk; } M[] = {
            { P_UP, VK_UP }, { P_DOWN, VK_DOWN }, { P_LEFT, VK_LEFT }, { P_RIGHT, VK_RIGHT },
            { P_CROSS, VK_RETURN }, { P_START, VK_RETURN }, { P_CIRCLE, VK_BACK }, { P_TRI, VK_BACK } };
        for (int i = 0; i < ARRAYSIZE(M); i++) if (down & M[i].bit) {
            post_key(h, M[i].vk);
            if (M[i].bit & 15) { rep_bit = M[i].bit; rep_at = now + 400; }
        }
        if (rep_bit && !(cur & rep_bit)) rep_bit = 0;
        if (rep_bit && (int)(now - rep_at) >= 0) {
            rep_at = now + 110;
            post_key(h, rep_bit == P_UP ? VK_UP : rep_bit == P_DOWN ? VK_DOWN : rep_bit == P_LEFT ? VK_LEFT : VK_RIGHT);
        }
    } else {
        rep_bit = 0; latched |= cur;
        if (*(BYTE *)(U32(X->settings) + 0x479)) {   // debrief: cross = Space (continue), triangle = Escape (retry)
            if (down & P_CROSS) post_key(h, VK_SPACE);
            if (down & P_TRI) post_key(h, VK_ESCAPE);
        } else if (down & (P_CROSS | P_START | P_CIRCLE)) post_key(h, VK_ESCAPE);   // skips movies, ignored in battle
    }
}

static BOOL (WINAPI *real_peek)(MSG *, HWND, UINT, UINT, UINT);
static BOOL WINAPI my_peek(MSG *m, HWND h, UINT a, UINT b, UINT rm) { poll(); return real_peek(m, h, a, b, rm); }

// battle: wraps call FUN_0044cdb0 (keyboard GetDeviceState into a 256-byte DIK array) in FUN_0045d750, edi = player
static DWORD kbstate_fn;
static BYTE __cdecl kb_state(BYTE *buf, BYTE *user) {
    BYTE ok = ((BYTE (__cdecl *)(BYTE *))kbstate_fn)(buf);
    last_poll -= 4; poll();
    WORD m = cur | latched; latched = 0;
    if (!m) return ok;
    if (!ok) memset(buf, 0, 256);
    const WORD *bind = (const WORD *)(user + 0x108);
    for (int i = 0; i < 16; i++) if ((m & (1 << i)) && bind[i] && bind[i] < 256) buf[bind[i]] = 0x80;
    return 1;
}
static __declspec(naked) void kb_stub(void) {
    __asm {
        push edi
        push dword ptr [esp + 8]
        call kb_state
        add esp, 8
        ret
    }
}

// DirectInput joystick enumeration: skip XInput devices (their HID path contains "IG_")
typedef HRESULT (__stdcall *fn_t)();
#define VFN(o, i) ((fn_t)(*(DWORD **)(o))[i])
static BOOL (__stdcall *real_enum)(const BYTE *, void *);
static int is_xinput(const BYTE *ddi) {
    void *di = *(void **)X->dinput, *dev = NULL; int r = 0;
    const char *name = (const char *)ddi + 0x12c;            // DIDEVICEINSTANCEA.tszProductName
    if (di && !VFN(di, 3)(di, ddi + 4, &dev, NULL) && dev) {   // IDirectInput7A::CreateDevice(guidInstance)
        static struct { DWORD size, hsize, obj, how; GUID cls; WCHAR path[260]; } gp;
        gp.size = sizeof gp; gp.hsize = 16; gp.obj = 0; gp.how = 0;
        if (!VFN(dev, 5)(dev, (const GUID *)12, &gp))            // GetProperty(DIPROP_GUIDANDPATH)
            for (WCHAR *p = gp.path; p[0] && p[1] && p[2]; p++)
                if ((p[0] | 32) == 'i' && (p[1] | 32) == 'g' && p[2] == '_') { r = 1; break; }
        VFN(dev, 2)(dev);
        dlog("Gamepad: DirectInput joystick '%s' path %S%s", name, gp.path, r ? " = XInput controller, hidden from the game" : "");
    }
    return r;
}
static BOOL __stdcall my_enum(const BYTE *ddi, void *ref) {
    if (is_xinput(ddi)) return TRUE;                         // DIENUM_CONTINUE
    return real_enum(ddi, ref);
}

void gamepad_init(DWORD kb_site, DWORD enum_site) {
    HMODULE x = LoadLibraryA("xinput1_4.dll");
    if (!x) x = LoadLibraryA("xinput9_1_0.dll");
    if (x) xget = (void *)GetProcAddress(x, "XInputGetState");
    dlog("Gamepad: XInput %s", xget ? "loaded" : "not available");
    void **pm = find_iat(GetModuleHandleA(NULL), "user32.dll", "PeekMessageA");
    if (pm) { real_peek = *pm; patch_iat(pm, (void *)my_peek); }
    kbstate_fn = kb_site + 5 + *(int *)(kb_site + 1);
    BYTE c[5] = { 0xe8 }; *(int *)(c + 1) = (int)((DWORD)kb_stub - (kb_site + 5)); write_code((void *)kb_site, c, 5);
    real_enum = *(void **)(enum_site + 1);
    void *e = (void *)my_enum; write_code((void *)(enum_site + 1), &e, 4);
}
