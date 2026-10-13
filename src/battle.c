// battle.c - battle loop: idle wait instead of busy spin, and rendering decoupled from the 25 Hz logic tick with
// interpolation of camera + objects. Addresses below are the no-CD exe; the per-exe table in hogsfix.c has both.
//
// Hooks (installed by hogsfix.c):
//   0x47e1d0  call FUN_0047f6c0 (battle step)          -> hf_step
//   0x47f9c5  call FUN_0044e490 (render, after a tick) -> hf_render_tick
//
// Per tick (DAT_0052098c changed) we snapshot every object in the render list DAT_005380cc: transform
// [obj+0x18] pos (+4..+0xc) and 3x3 matrix (+0x20..+0x40), pig (type 0x1357) heading obj+0x174, and the camera
// angles cam+0x88/+0x8c/+0x90. Before each render we write lerp(prev, cur, alpha), alpha = (now - lastTick) / period,
// render, then restore the true values, so the simulation never sees interpolated state.
// Frames are rendered on a fixed grid (FrameHz). The HUD/message objects advance exactly once per tick: the first
// frame after a tick renders normally, all other frames save/restore the HUD state around the render.
// Config: HogsFix.ini [Battle]: Interp=1, FrameHz=60 (0 = only tick frames, like original).
#include "hogsfix.h"

#define F32P(a)  ((volatile float *)(a))
typedef DWORD (__cdecl *step_t)(void);
typedef void (__fastcall *render_t)(DWORD);
#define STEP     ((step_t)X->step)
#define RENDER   ((render_t)X->render)
#define TICK_TS  (X->tick_ts)
#define TMPTR    (X->tmptr)      // turn manager: period +0x314
#define RENDOBJ  (X->rendobj)
#define CAMPTR   (X->camptr)
#define OBJLIST  (X->objlist)
#define HUDPTR   (X->hudptr)      // size 0xe18 (new at 0x485ba3)
#define MSGPTR   (X->msgptr)      // size 0x96c (new at 0x485b36)
#define MSGTICK  (X->msgtick)
#define HUD_SIZE 0xe18
#define MSG_SIZE 0x96c

#define MAXE 512
typedef struct {
    DWORD obj, tr; int type, seen, head_ok;
    float pos[2][3], mat[2][9];             // [0] = prev, [1] = cur
    int head[2];
    float spos[3], smat[9]; int shead;      // true values saved during an interpolated render
    int applied;
} Ent;
static Ent ents[MAXE];
static int nents;
static int cang[2][3], sang[3], ang_ok;
static DWORD last_ts, ts_rendered;          // ts of last snapshot; ts for which the tick frame was handled
static int inited, hud_pending;
static DWORD next_frame, frame_q;          // microseconds in the timeGetTime()*1000 domain (wraps; compare signed)
static unsigned char hud_save[HUD_SIZE], msg_save[MSG_SIZE];
static DWORD msgtick_save;
static DWORD flip_obj[64]; static int flip_val[64], nflip;

// statistics, readable from outside (exported)
volatile DWORD hf_stats[16];  // 0 tick frames, 1 extra frames, 2 skipped tick frames, 3 snapshots,
                                                    // 4 snaps (no lerp), 5 entries, 6 last alpha*1000, 7 deferred-hud frames,
                                                    // 8 last snapped obj type, 9 its jump distance

static void init(void) {
    inited = 1;
    timeBeginPeriod(1);
    frame_q = cfg_hz ? 1000000 / cfg_hz : 0;
    next_frame = timeGetTime() * 1000;
}

static float fabsf_(float x) { return x < 0 ? -x : x; }
static int iround(float x) { return x < 0 ? -(int)(-x + 0.5f) : (int)(x + 0.5f); }
static int adiff(int a, int b) { return ((b - a + 2048) & 4095) - 2048; }   // shortest a->b on the 4096 circle

static Ent *find(DWORD obj, int hint) {
    if (hint < nents && ents[hint].obj == obj) return &ents[hint];
    for (int i = 0; i < nents; i++) if (ents[i].obj == obj) return &ents[i];
    return 0;
}

// called once per new tick timestamp, at the tick render (after the camera update)
static void snapshot(void) {
    int i, k = 0;
    for (i = 0; i < nents; i++) ents[i].seen = 0;
    DWORD o = U32(OBJLIST);
    for (int n = 0; o && n < 4096; n++, o = U32(o + 4)) {
        DWORD tr = U32(o + 0x18);
        if (!tr) continue;
        Ent *e = find(o, k);
        int type = I32(o + 0x38);
        if (type < 0x1357 || type > 0x1362) continue;                        // known render object types only
        if (!e || e->tr != tr || e->type != type) {
            if (IsBadWritePtr((void *)tr, 0x44) || (type == 0x1357 && IsBadWritePtr((void *)o, 0x178))) continue;
            if (!e) { if (nents >= MAXE) continue; e = &ents[nents++]; }
            e->obj = o; e->tr = tr; e->type = type; e->seen = 1;
            for (i = 0; i < 3; i++) e->pos[1][i] = F32P(tr + 4)[i];
            for (i = 0; i < 9; i++) e->mat[1][i] = F32P(tr + 0x20)[i];
            e->head[1] = type == 0x1357 ? I32(o + 0x174) : 0;
            memcpy(e->pos[0], e->pos[1], sizeof e->pos[0]); memcpy(e->mat[0], e->mat[1], sizeof e->mat[0]);
            e->head[0] = e->head[1]; e->head_ok = 0;
            hf_stats[4]++;
        } else {
            e->seen = 1;
            memcpy(e->pos[0], e->pos[1], sizeof e->pos[0]); memcpy(e->mat[0], e->mat[1], sizeof e->mat[0]);
            e->head[0] = e->head[1];
            for (i = 0; i < 3; i++) e->pos[1][i] = F32P(tr + 4)[i];
            for (i = 0; i < 9; i++) e->mat[1][i] = F32P(tr + 0x20)[i];
            e->head[1] = type == 0x1357 ? I32(o + 0x174) : 0;
            float dx = e->pos[1][0] - e->pos[0][0], dy = e->pos[1][1] - e->pos[0][1], dz = e->pos[1][2] - e->pos[0][2];
            if (dx * dx + dy * dy + dz * dz > 3000.f * 3000.f) {      // teleport / respawn / camera cut: no lerp
                memcpy(e->pos[0], e->pos[1], sizeof e->pos[0]); hf_stats[4]++;
                hf_stats[8] = type; hf_stats[9] = (DWORD)(int)((dx * dx + dy * dy + dz * dz) / 1000.f);
            }
            int big = 0;
            for (i = 0; i < 9; i++) if (fabsf_(e->mat[1][i] - e->mat[0][i]) > 0.5f) big = 1;
            if (big) memcpy(e->mat[0], e->mat[1], sizeof e->mat[0]);
            e->head_ok = type == 0x1357 && adiff(e->head[0], e->head[1]) != 0 && (adiff(e->head[0], e->head[1]) < 600 && adiff(e->head[0], e->head[1]) > -600);
        }
        k = (int)(e - ents) + 1;
    }
    for (i = 0, k = 0; i < nents; i++) if (ents[i].seen) { if (k != i) ents[k] = ents[i]; k++; }   // drop vanished objects
    nents = k;
    DWORD cam = U32(CAMPTR);
    if (cam) {
        for (i = 0; i < 3; i++) { cang[0][i] = cang[1][i]; cang[1][i] = I32(cam + 0x88 + 4 * i); }
        ang_ok = 1;
        for (i = 0; i < 3; i++) { int d = adiff(cang[0][i], cang[1][i]); if (d > 700 || d < -700) ang_ok = 0; }
    } else ang_ok = 0;
    hf_stats[3]++; hf_stats[5] = nents;
}

static int lerp_ang(int a, int b, float t) {
    int r = a + iround(adiff(a, b) * t);
    if (a >= 0 && a < 4096 && b >= 0 && b < 4096) r &= 4095;
    return r;
}

static void apply(float t) {
    int i;
    for (int j = 0; j < nents; j++) {
        Ent *e = &ents[j]; e->applied = 0;
        volatile float *p = F32P(e->tr + 4), *m = F32P(e->tr + 0x20);
        // only touch the object if it still holds exactly the snapshot values (nothing moved it since)
        int same = 1;
        for (i = 0; i < 3; i++) if (p[i] != e->pos[1][i]) same = 0;
        for (i = 0; i < 9; i++) if (m[i] != e->mat[1][i]) same = 0;
        if (!same) continue;
        e->applied = 1;
        for (i = 0; i < 3; i++) { e->spos[i] = p[i]; p[i] = e->pos[0][i] + (e->pos[1][i] - e->pos[0][i]) * t; }
        for (i = 0; i < 9; i++) { e->smat[i] = m[i]; m[i] = e->mat[0][i] + (e->mat[1][i] - e->mat[0][i]) * t; }
        if (e->head_ok && I32(e->obj + 0x174) == e->head[1]) {
            e->shead = e->head[1]; I32(e->obj + 0x174) = lerp_ang(e->head[0], e->head[1], t); e->applied = 2;
        }
    }
    DWORD cam = U32(CAMPTR);
    if (cam && ang_ok) {
        int same = 1;
        for (i = 0; i < 3; i++) if (I32(cam + 0x88 + 4 * i) != cang[1][i]) same = 0;
        if (same) for (i = 0; i < 3; i++) { sang[i] = cang[1][i]; I32(cam + 0x88 + 4 * i) = lerp_ang(cang[0][i], cang[1][i], t); }
        ang_ok = same ? 2 : 1;
    }
}

static void restore(void) {
    int i;
    for (int j = 0; j < nents; j++) {
        Ent *e = &ents[j];
        if (!e->applied) continue;
        for (i = 0; i < 3; i++) F32P(e->tr + 4)[i] = e->spos[i];
        for (i = 0; i < 9; i++) F32P(e->tr + 0x20)[i] = e->smat[i];
        if (e->applied == 2) I32(e->obj + 0x174) = e->shead;
        e->applied = 0;
    }
    DWORD cam = U32(CAMPTR);
    if (cam && ang_ok == 2) { for (i = 0; i < 3; i++) I32(cam + 0x88 + 4 * i) = sang[i]; ang_ok = 1; }
}

static void render_interp(DWORD robj) {
    DWORD tm = U32(TMPTR);
    int lerp = cfg_interp && cfg_hz && tm;     // at the original tick rate interpolation would only add latency
    if (lerp) {
        int period = I32(tm + 0x314); if (period <= 0) period = 40;
        float t = (float)(int)(timeGetTime() - U32(TICK_TS)) / (float)period;
        if (t < 0) t = 0; if (t > 1) t = 1;
        hf_stats[6] = (DWORD)(int)(t * 1000);
        apply(t);
    }
    RENDER(robj);
    if (lerp) restore();
}

// render whose per-render side effects on the HUD must not persist (it is not the tick's HUD frame)
static void render_quiet(DWORD robj) {
    DWORD hud = U32(HUDPTR), msg = U32(MSGPTR);
    if (hud) memcpy(hud_save, (void *)hud, HUD_SIZE);
    if (msg) memcpy(msg_save, (void *)msg, MSG_SIZE);
    msgtick_save = U32(MSGTICK);
    nflip = 0;   // blinking objects (type 0x135b) flip [model+0x54] each render
    for (DWORD o = U32(OBJLIST); o && nflip < 64; o = U32(o + 4))
        if (I32(o + 0x38) == 0x135b && U32(o + 0x58)) { flip_obj[nflip] = U32(o + 0x58); flip_val[nflip++] = I32(U32(o + 0x58) + 0x54); }
    render_interp(robj);
    if (hud) memcpy((void *)hud, hud_save, HUD_SIZE);
    if (msg) memcpy((void *)msg, msg_save, MSG_SIZE);
    U32(MSGTICK) = msgtick_save;
    for (int i = 0; i < nflip; i++) I32(flip_obj[i] + 0x54) = flip_val[i];
}

static DWORD now_us(void) { return timeGetTime() * 1000; }
static int frame_due(DWORD now) { return !frame_q || (int)(now - next_frame) >= -500; }
static void frame_done(DWORD now) {
    if (!frame_q) return;
    next_frame += frame_q;
    if ((int)(next_frame - now) < 0 || (int)(next_frame - now) > 2 * (int)frame_q) next_frame = now + frame_q;
}

void __fastcall hf_render_tick(DWORD robj) {
    if (!inited) init();
    DWORD ts = U32(TICK_TS);
    if (ts != last_ts) { snapshot(); last_ts = ts; }
    ts_rendered = ts;
    DWORD now = now_us();
    if (cfg_hz && !frame_due(now)) { hud_pending = 1; hf_stats[2]++; return; }   // next grid frame does the HUD
    hud_pending = 0;
    render_interp(robj);
    frame_done(now);
    hf_stats[0]++;
}

DWORD __cdecl hf_step(void) {
    DWORD ts0 = U32(TICK_TS);
    DWORD r = STEP();
    if (r >= 2 && r <= 5) return r;
    DWORD tm = U32(TMPTR);
    if (!tm) return r;
    if (!inited) init();
    DWORD now;
    DWORD robj = U32(RENDOBJ);
    int period = I32(tm + 0x314);
    // extra frame between ticks: only while the tick frame for the current timestamp went through our hook
    if (cfg_hz && robj && ts_rendered == U32(TICK_TS) && last_ts == ts_rendered) {
        now = now_us();
        if (frame_due(now) && (int)(timeGetTime() - U32(TICK_TS)) < period) {
            if (hud_pending) { hud_pending = 0; render_interp(robj); hf_stats[7]++; }
            else render_quiet(robj);
            frame_done(now);
            hf_stats[1]++;
        }
    }
    // wait until the next tick or the next frame, whichever is first; any input/message wakes us
    int rem = period - (int)(timeGetTime() - U32(TICK_TS));
    if (cfg_hz && ts_rendered == U32(TICK_TS)) {
        int fr = (int)(next_frame - now_us()) / 1000;
        if (fr < rem) rem = fr;
    }
    if (rem <= 0) {   // something is due now; if a tick is due but the step didn't run one (pause/state 7), don't spin
        if (U32(TICK_TS) == ts0 && (int)(timeGetTime() - U32(TICK_TS)) >= period) MsgWaitForMultipleObjects(0, NULL, FALSE, 1, QS_ALLINPUT);
        return r;
    }
    if (rem == 1) { Sleep(0); return r; }
    if (rem > 100) rem = 100;
    MsgWaitForMultipleObjects(0, NULL, FALSE, rem - 1, QS_ALLINPUT);
    return r;
}

void battle_init(void) { frame_q = cfg_hz ? 1000000 / cfg_hz : 0; }
