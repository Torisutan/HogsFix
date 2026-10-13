// cdaudio.c - CD music without a CD drive.
// The game plays its music as Red Book audio tracks of the game CD (MCI "cdaudio" through mciSendCommandA: open by
// type id, TMSF time format, track table via STATUS, "play track n to track n+1", mode polling, pause/resume) and sets
// the music volume on the mixer's CD line (mixerGetLineInfo SRC_COMPACTDISC + VOLUME control). Neither exists on a
// modern PC; GOG ships a winmm.dll proxy (+ libvorbis DLLs) that plays MUSIC\TrackNN.ogg instead.
// Here, without extra DLLs: the exe's mciSendCommandA and mixer* imports -> an emulated CD drive whose audio tracks
// are MUSIC\TrackNN.ogg (track 1 = data track), decoded with stb_vorbis (embedded) and streamed through waveOut. The
// CD line volume scales the decoded samples (sound effects/speech use DirectSound and are not affected).
// Exe call sites: FUN_0045f870 open/FUN_0045f900 close, FUN_0045f9c0 track table, FUN_0045fb00 play, FUN_0045f970
// "is playing", FUN_0045fc30 stop, FUN_0045fc60 pause, FUN_0045fc90 resume (play TO only), FUN_0045fcd0/FUN_0045fe30
// mixer volume (0..0xffdc).
#include "hogsfix.h"

// ---- stb_vorbis without the CRT: all decoder memory comes from a per-stream buffer (stb_vorbis_alloc) ----
#pragma warning(push, 1)
#pragma warning(disable: 4002 4005 4035 4244 4245 4701 4703)
#include <math.h>
#include <string.h>
#include <stdlib.h>
#pragma function(sin, cos, exp, log, pow, floor, memcmp)
double sin(double x) { __asm fld x __asm fsin }
double cos(double x) { __asm fld x __asm fcos }
double log(double x) { __asm fldln2 __asm fld x __asm fyl2x }
double exp(double x) {
    __asm {
        fld x
        fldl2e
        fmulp st(1), st
        fld st
        frndint
        fsub st(1), st
        fxch st(1)
        f2xm1
        fld1
        faddp st(1), st
        fscale
        fstp st(1)
    }
}
double pow(double x, double y) { return x <= 0 ? 0 : exp(y * log(x)); }
double floor(double x) { int i = (int)x; return (double)i > x ? i - 1 : i; }
double ldexp(double x, int e) { __asm fild e __asm fld x __asm fscale __asm fstp st(1) }
__declspec(naked) void _dtol3(void) {                 // double (xmm0) -> __int64 (edx:eax), truncating (seek code)
    __asm {
        sub esp, 8
        movsd qword ptr [esp], xmm0
        fld qword ptr [esp]
        fisttp qword ptr [esp]
        pop eax
        pop edx
        ret
    }
}
int memcmp(const void *a, const void *b, size_t n) {
    const BYTE *p = a, *q = b; for (; n--; p++, q++) if (*p != *q) return *p - *q; return 0;
}
void __cdecl qsort(void *base, size_t n, size_t w, int (__cdecl *cmp)(const void *, const void *)) {
    BYTE *b = base;                                   // shell sort, only used while opening a stream
    for (size_t gap = n / 2; gap; gap /= 2)
        for (size_t i = gap; i < n; i++)
            for (size_t j = i; j >= gap && cmp(b + (j - gap) * w, b + j * w) > 0; j -= gap)
                for (size_t k = 0; k < w; k++) { BYTE t = b[(j - gap) * w + k]; b[(j - gap) * w + k] = b[j * w + k]; b[j * w + k] = t; }
}
#define STB_VORBIS_NO_CRT
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define assert(x) ((void)0)
#define alloca(n) ((void *)0)
#include "stb_vorbis.c"
#undef NULL
#define NULL ((void *)0)
#pragma warning(pop)

int cfg_music = 1, cfg_music_vol = 100;

// ---- disc layout: absolute disc time in ms; track t covers tr_start[t] .. tr_start[t+1] ----
#define MAXTR 99
static int ntr;
static DWORD tr_start[MAXTR + 2], tr_len[MAXTR + 1];
static BYTE tr_audio[MAXTR + 1];
static char musdir[MAX_PATH];

static void track_path(char *p, int t) { wsprintfA(p, "%sTrack%02d.ogg", musdir, t); }
// length from the Ogg headers: sample rate (identification header) and granule position of the last page
static DWORD ogg_len_ms(int t) {
    char p[MAX_PATH]; track_path(p, t);
    HANDLE f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    static BYTE b[65536]; DWORD n = 0, rate = 0, gran = 0, sz = GetFileSize(f, NULL);
    ReadFile(f, b, 4096, &n, NULL);
    for (DWORD i = 0; i + 16 <= n; i++) if (b[i] == 1 && !memcmp(b + i + 1, "vorbis", 6)) { rate = *(DWORD *)(b + i + 12); break; }
    DWORD off = sz > sizeof b ? sz - sizeof b : 0;
    SetFilePointer(f, off, NULL, FILE_BEGIN); n = 0; ReadFile(f, b, sizeof b, &n, NULL); CloseHandle(f);
    for (int i = (int)n - 27; i >= 0; i--) if (!memcmp(b + i, "OggS", 4) && b[i + 4] == 0) { gran = *(DWORD *)(b + i + 6); break; }
    return rate ? MulDiv(gran, 1000, rate) : 0;
}
static void scan_tracks(void) {
    DWORD n = GetModuleFileNameA(NULL, musdir, MAX_PATH);
    while (n && musdir[n - 1] != '\\') n--;
    lstrcpyA(musdir + n, "MUSIC\\");
    ntr = 1;
    for (int t = 1; t <= MAXTR; t++) {
        tr_len[t] = ogg_len_ms(t);
        tr_audio[t] = tr_len[t] > 0;
        if (tr_audio[t]) ntr = t;
    }
    tr_start[1] = 2000;                               // 2 s lead-in (MSF 00:02:00)
    for (int t = 1; t <= ntr; t++) {
        if (!tr_audio[t]) tr_len[t] = 4000;           // data track / missing file: 4 s, not playable
        tr_start[t + 1] = tr_start[t] + tr_len[t];
    }
    dlog("Music: %d tracks in %s, track 2 %u ms, disc %u ms", ntr, musdir, tr_len[2], tr_start[ntr + 1]);
}
static int track_at(DWORD ms) { int t = 1; while (t < ntr && tr_start[t + 1] <= ms) t++; return t; }

// ---- MCI time formats ----
static DWORD tfmt = MCI_FORMAT_MSF;
static DWORD msf(DWORD ms) { DWORD f = MulDiv(ms, 75, 1000); return MCI_MAKE_MSF(f / 4500, f / 75 % 60, f % 75); }
static DWORD msf_ms(DWORD m, DWORD s, DWORD f) { return (m * 60 + s) * 1000 + MulDiv(f, 1000, 75); }
static DWORD from_abs(DWORD ms) {
    if (tfmt == MCI_FORMAT_MILLISECONDS) return ms;
    if (tfmt == MCI_FORMAT_MSF) return msf(ms);
    int t = track_at(ms); DWORD m = msf(ms - tr_start[t]);
    return MCI_MAKE_TMSF(t, MCI_MSF_MINUTE(m), MCI_MSF_SECOND(m), MCI_MSF_FRAME(m));
}
static DWORD len_fmt(DWORD ms) { return tfmt == MCI_FORMAT_MILLISECONDS ? ms : msf(ms); }   // TMSF lengths are MSF
static int to_abs(DWORD v, DWORD *ms) {
    if (tfmt == MCI_FORMAT_MILLISECONDS) *ms = v;
    else if (tfmt == MCI_FORMAT_MSF) *ms = msf_ms(MCI_MSF_MINUTE(v), MCI_MSF_SECOND(v), MCI_MSF_FRAME(v));
    else {
        int t = MCI_TMSF_TRACK(v);
        if (t < 1 || t > ntr) return 0;
        DWORD o = msf_ms(MCI_TMSF_MINUTE(v), MCI_TMSF_SECOND(v), MCI_TMSF_FRAME(v));
        *ms = tr_start[t] + (o < tr_len[t] ? o : tr_len[t]);
    }
    if (*ms > tr_start[ntr + 1]) *ms = tr_start[ntr + 1];
    return 1;
}

// ---- player: one waveOut stream fed by a worker thread; everything under cs ----
#define NBUF 4
#define BUF_FRAMES 4096
static CRITICAL_SECTION cs; static HANDLE wake, worker;
static HWAVEOUT wo; static DWORD wo_rate, wo_ch;
static WAVEHDR hdr[NBUF]; static short pcm[NBUF][BUF_FRAMES * 2]; static BYTE queued[NBUF];
static stb_vorbis *dec; static BYTE *dec_file, *dec_mem; static int dec_track, dec_ch; static DWORD dec_rate, dec_left;
static int mode;                                      // 0 stopped, 1 playing, 2 paused
static int decoding;                                  // more audio of the play range to queue
static DWORD play_abs0, play_end, pos_abs;            // disc ms at the last waveOutReset, end of range, stopped position
static HWND notify_wnd; static int cd_open;
static DWORD cd_vol = 0xffff;                         // CD line volume 0..0xffff

static void *halloc(DWORD n) { return HeapAlloc(GetProcessHeap(), 0, n); }
static void hfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static void dec_close(void) {
    if (dec) stb_vorbis_close(dec);
    hfree(dec_file); hfree(dec_mem); dec = NULL; dec_file = dec_mem = NULL;
}
// open track t at off ms; dec_left = samples up to min(track end, play_end)
static int dec_open(int t, DWORD off) {
    dec_close();
    char p[MAX_PATH]; track_path(p, t);
    HANDLE f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { dlog("Music: cannot open %s", p); return 0; }
    DWORD sz = GetFileSize(f, NULL), n = 0;
    dec_file = halloc(sz);
    if (dec_file) ReadFile(f, dec_file, sz, &n, NULL);
    CloseHandle(f);
    if (!dec_file || n != sz) { dlog("Music: cannot read %s", p); dec_close(); return 0; }
    int err = 0;
    for (int memsz = 256 << 10; memsz <= 8 << 20; memsz *= 2) {
        stb_vorbis_alloc a; a.alloc_buffer = dec_mem = halloc(memsz); a.alloc_buffer_length_in_bytes = memsz;
        if (!dec_mem) break;
        dec = stb_vorbis_open_memory(dec_file, sz, &err, &a);
        if (dec || err != VORBIS_outofmem) break;
        hfree(dec_mem); dec_mem = NULL;
    }
    if (!dec) { dlog("Music: %s: stb_vorbis error %d", p, err); dec_close(); return 0; }
    stb_vorbis_info i = stb_vorbis_get_info(dec);
    dec_rate = i.sample_rate; dec_ch = i.channels > 1 ? 2 : 1; dec_track = t;
    DWORD end = tr_start[t] + tr_len[t]; if (end > play_end) end = play_end;
    DWORD s0 = MulDiv(off, dec_rate, 1000), s1 = MulDiv(end - tr_start[t], dec_rate, 1000);
    if (s0 && !stb_vorbis_seek(dec, s0)) { stb_vorbis_seek_start(dec); s0 = 0; }
    dec_left = s1 > s0 ? s1 - s0 : 0;
    return 1;
}
static void wo_close(void) {
    if (!wo) return;
    waveOutReset(wo);
    for (int i = 0; i < NBUF; i++) if (hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]);
    waveOutClose(wo); wo = NULL;
}
static void wo_reset(void) {
    if (!wo) return;
    waveOutReset(wo); waveOutRestart(wo);             // (a paused device stays paused after a reset)
    for (int i = 0; i < NBUF; i++) {
        if (hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]);
        queued[i] = 0;
    }
}
static int wo_format(void) {
    if (wo && wo_rate == dec_rate && wo_ch == (DWORD)dec_ch) { wo_reset(); return 1; }
    wo_close();
    WAVEFORMATEX w = { WAVE_FORMAT_PCM, (WORD)dec_ch, dec_rate, dec_rate * dec_ch * 2, (WORD)(dec_ch * 2), 16, 0 };
    MMRESULT r = waveOutOpen(&wo, WAVE_MAPPER, &w, (DWORD_PTR)wake, 0, CALLBACK_EVENT);
    if (r) { dlog("Music: waveOutOpen %u Hz x%d failed (%u)", dec_rate, dec_ch, r); wo = NULL; return 0; }
    wo_rate = dec_rate; wo_ch = dec_ch;
    for (int i = 0; i < NBUF; i++) queued[i] = 0;
    return 1;
}
static DWORD cur_pos(void) {
    if (mode == 0 || !wo) return pos_abs;
    MMTIME t; t.wType = TIME_SAMPLES;
    if (waveOutGetPosition(wo, &t, sizeof t) || t.wType != TIME_SAMPLES) return play_abs0;
    DWORD p = play_abs0 + MulDiv(t.u.sample, 1000, wo_rate);
    return p < play_end ? p : play_end;
}
static void notify(UINT code) {
    if (notify_wnd) PostMessageA(notify_wnd, MM_MCINOTIFY, code, 0x2a51);
    notify_wnd = NULL;
}
// decode the next buffer of the play range into hdr[i]; continues into the next track (same format)
static int fill(int i) {
    short *o = pcm[i]; DWORD got = 0;
    while (got < BUF_FRAMES && decoding) {
        DWORD want = BUF_FRAMES - got; if (want > dec_left) want = dec_left;
        int n = want ? stb_vorbis_get_samples_short_interleaved(dec, dec_ch, o + got * dec_ch, want * dec_ch) : 0;
        if (n > 0) { got += n; dec_left -= n; continue; }
        int t = dec_track + 1; DWORD rate = dec_rate; int ch = dec_ch;
        if (t <= ntr && tr_audio[t] && tr_start[t] < play_end && dec_open(t, 0) && dec_rate == rate && dec_ch == ch) continue;
        decoding = 0; dec_close();
    }
    if (!got) return 0;
    DWORD v = MulDiv(cd_vol, cfg_music_vol, 100), ns = got * dec_ch;
    if (v < 0xffff) for (DWORD k = 0; k < ns; k++) o[k] = (short)((o[k] * (int)v) >> 16);
    WAVEHDR *h = &hdr[i]; ZeroMemory(h, sizeof *h);
    h->lpData = (char *)o; h->dwBufferLength = ns * 2;
    if (waveOutPrepareHeader(wo, h, sizeof *h) || waveOutWrite(wo, h, sizeof *h)) { decoding = 0; return 0; }
    queued[i] = 1;
    return 1;
}
static DWORD WINAPI worker_proc(void *u) {
    for (;;) {
        WaitForSingleObject(wake, 250);
        EnterCriticalSection(&cs);
        if (mode == 1 && wo) {
            int busy = 0;
            for (int i = 0; i < NBUF; i++) {
                if (queued[i] && (hdr[i].dwFlags & WHDR_DONE)) { waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]); queued[i] = 0; }
                if (!queued[i] && decoding) fill(i);
                busy |= queued[i];
            }
            if (!busy && !decoding) { mode = 0; pos_abs = play_end; notify(MCI_NOTIFY_SUCCESSFUL); }
        }
        LeaveCriticalSection(&cs);
    }
}
static void stop(void) {
    if (mode) pos_abs = cur_pos();
    wo_reset(); dec_close(); mode = 0; decoding = 0;
}
static MCIERROR play(DWORD from, DWORD to) {
    stop();
    int t = track_at(from);
    while (t <= ntr && !tr_audio[t]) { t++; from = tr_start[t]; }       // skip data tracks
    if (t > ntr || from >= to) { pos_abs = from; return 0; }
    play_end = to;
    if (!dec_open(t, from - tr_start[t]) || !wo_format()) { dec_close(); return MCIERR_HARDWARE; }
    play_abs0 = from; mode = 1; decoding = 1;
    for (int i = 0; i < NBUF; i++) fill(i);
    SetEvent(wake);
    return 0;
}

// ---- mciSendCommandA ----
#define CD_ID 0x2a51
typedef MCIERROR (WINAPI *mci_t)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
static mci_t real_mci;
static MCIERROR cd_command(UINT msg, DWORD fl, DWORD_PTR prm) {
    MCIERROR r = 0;
    switch (msg) {
    case MCI_CLOSE:
        stop(); wo_close(); notify(MCI_NOTIFY_ABORTED); cd_open = 0;
        break;
    case MCI_PLAY: {
        MCI_PLAY_PARMS *p = (MCI_PLAY_PARMS *)prm; DWORD from, to = tr_start[ntr + 1];
        if (fl & MCI_TO && (!p || !to_abs(p->dwTo, &to))) { r = MCIERR_OUTOFRANGE; break; }
        if (mode == 2 && !(fl & MCI_FROM)) {          // resume after pause
            if (fl & MCI_TO) play_end = to;
            waveOutRestart(wo); mode = 1; SetEvent(wake);
            dlog("Music: resume at %u ms (play to %u)", cur_pos(), play_end);
        } else {
            if (fl & MCI_FROM) { if (!p || !to_abs(p->dwFrom, &from)) { r = MCIERR_OUTOFRANGE; break; } }
            else from = cur_pos();
            notify(MCI_NOTIFY_SUPERSEDED);
            r = play(from, to);
            dlog("Music: play %u..%u ms (track %d) -> %u", from, to, track_at(from), r);
        }
        if (!r && fl & MCI_NOTIFY && p) notify_wnd = (HWND)p->dwCallback;
        if (!r && fl & MCI_WAIT) {
            while (mode == 1) { LeaveCriticalSection(&cs); Sleep(20); EnterCriticalSection(&cs); }
        }
        return r;
    }
    case MCI_STOP:
        if (mode) dlog("Music: stop at %u ms", cur_pos());
        if (mode) notify(MCI_NOTIFY_ABORTED);
        stop();
        break;
    case MCI_PAUSE:
        if (mode == 1) { pos_abs = cur_pos(); waveOutPause(wo); mode = 2; dlog("Music: pause at %u ms", pos_abs); }
        break;
    case MCI_RESUME:
        if (mode == 2) { waveOutRestart(wo); mode = 1; SetEvent(wake); }
        break;
    case MCI_SEEK: {
        MCI_SEEK_PARMS *p = (MCI_SEEK_PARMS *)prm; DWORD to;
        if (fl & MCI_SEEK_TO_START) to = tr_start[1];
        else if (fl & MCI_SEEK_TO_END) to = tr_start[ntr + 1];
        else if (fl & MCI_TO && p && to_abs(p->dwTo, &to)) ;
        else { r = MCIERR_OUTOFRANGE; break; }
        if (mode) notify(MCI_NOTIFY_ABORTED);
        stop(); pos_abs = to;
        break;
    }
    case MCI_SET: {
        MCI_SET_PARMS *p = (MCI_SET_PARMS *)prm;
        if (fl & MCI_SET_TIME_FORMAT) {
            if (!p || (p->dwTimeFormat != MCI_FORMAT_MILLISECONDS && p->dwTimeFormat != MCI_FORMAT_MSF && p->dwTimeFormat != MCI_FORMAT_TMSF)) { r = MCIERR_BAD_TIME_FORMAT; break; }
            tfmt = p->dwTimeFormat;
        }
        break;                                        // door, audio channels: accepted, ignored
    }
    case MCI_STATUS: {
        MCI_STATUS_PARMS *p = (MCI_STATUS_PARMS *)prm;
        if (!p || !(fl & MCI_STATUS_ITEM)) { r = MCIERR_MISSING_PARAMETER; break; }
        int t = (fl & MCI_TRACK) ? (int)p->dwTrack : 0;
        if ((fl & MCI_TRACK) && (t < 1 || t > ntr)) { r = MCIERR_OUTOFRANGE; break; }
        switch (p->dwItem) {
        case MCI_STATUS_LENGTH:         p->dwReturn = len_fmt(t ? tr_len[t] : tr_start[ntr + 1]); break;
        case MCI_STATUS_POSITION:       p->dwReturn = from_abs((fl & MCI_STATUS_START) ? tr_start[1] : t ? tr_start[t] : cur_pos()); break;
        case MCI_STATUS_NUMBER_OF_TRACKS: p->dwReturn = ntr; break;
        case MCI_STATUS_MODE:           p->dwReturn = mode == 1 ? MCI_MODE_PLAY : mode == 2 ? MCI_MODE_PAUSE : MCI_MODE_STOP; break;
        case MCI_STATUS_MEDIA_PRESENT:
        case MCI_STATUS_READY:          p->dwReturn = TRUE; break;
        case MCI_STATUS_TIME_FORMAT:    p->dwReturn = tfmt; break;
        case MCI_STATUS_CURRENT_TRACK:  p->dwReturn = track_at(cur_pos()); break;
        case MCI_CDA_STATUS_TYPE_TRACK:
            if (!t) { r = MCIERR_MISSING_PARAMETER; break; }
            p->dwReturn = tr_audio[t] ? MCI_CDA_TRACK_AUDIO : MCI_CDA_TRACK_OTHER; break;
        default: r = MCIERR_BAD_CONSTANT;
        }
        break;
    }
    case MCI_GETDEVCAPS: {
        MCI_GETDEVCAPS_PARMS *p = (MCI_GETDEVCAPS_PARMS *)prm;
        if (!p || !(fl & MCI_GETDEVCAPS_ITEM)) { r = MCIERR_MISSING_PARAMETER; break; }
        switch (p->dwItem) {
        case MCI_GETDEVCAPS_DEVICE_TYPE: p->dwReturn = MCI_DEVTYPE_CD_AUDIO; break;
        case MCI_GETDEVCAPS_HAS_AUDIO: case MCI_GETDEVCAPS_CAN_EJECT: case MCI_GETDEVCAPS_CAN_PLAY: p->dwReturn = TRUE; break;
        default: p->dwReturn = FALSE;
        }
        break;
    }
    case MCI_INFO: {
        MCI_INFO_PARMSA *p = (MCI_INFO_PARMSA *)prm;
        if (p && p->lpstrReturn && p->dwRetSize) lstrcpynA(p->lpstrReturn, "HogsFix CD Audio", p->dwRetSize);
        break;
    }
    default:
        r = MCIERR_UNSUPPORTED_FUNCTION;
    }
    if (!r && fl & MCI_NOTIFY && prm)
        PostMessageA((HWND)((MCI_GENERIC_PARMS *)prm)->dwCallback, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, CD_ID);
    return r;
}
static MCIERROR WINAPI my_mci(MCIDEVICEID id, UINT msg, DWORD_PTR fl, DWORD_PTR prm) {
    if (msg == MCI_OPEN) {
        MCI_OPEN_PARMSA *p = (MCI_OPEN_PARMSA *)prm; int cd = 0;
        if (p && (fl & MCI_OPEN_TYPE))
            cd = (fl & MCI_OPEN_TYPE_ID) ? LOWORD((DWORD_PTR)p->lpstrDeviceType) == MCI_DEVTYPE_CD_AUDIO
                                         : p->lpstrDeviceType && !lstrcmpiA(p->lpstrDeviceType, "cdaudio");
        if (!cd) return real_mci(id, msg, fl, prm);
        EnterCriticalSection(&cs);
        if (!ntr) scan_tracks();
        if (!worker) worker = CreateThread(NULL, 0, worker_proc, NULL, 0, NULL);
        if (!cd_open) tfmt = MCI_FORMAT_MSF;
        cd_open = 1; p->wDeviceID = CD_ID;
        LeaveCriticalSection(&cs);
        if (fl & MCI_NOTIFY) PostMessageA((HWND)p->dwCallback, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, CD_ID);
        return 0;
    }
    if (id == CD_ID || (id == MCI_ALL_DEVICE_ID && cd_open && (msg == MCI_CLOSE || msg == MCI_STOP))) {
        EnterCriticalSection(&cs);
        MCIERROR r = cd_open ? cd_command(msg, (DWORD)fl, prm) : MCIERR_INVALID_DEVICE_ID;
        LeaveCriticalSection(&cs);
        if (id == CD_ID) return r;
    }
    return real_mci(id, msg, fl, prm);
}

// ---- mixer: one fake mixer with a CD line and its volume control (the game only uses the mixer for this) ----
#define MIX_H ((HMIXER)0x4d58)
#define CD_LINE 0x10000
#define CD_CTRL 1
static UINT WINAPI my_mixerGetNumDevs(void) { return 1; }
static MMRESULT WINAPI my_mixerGetDevCapsA(UINT_PTR id, MIXERCAPSA *c, UINT n) {
    if (!c || n < sizeof *c) return MMSYSERR_INVALPARAM;
    ZeroMemory(c, sizeof *c); c->wMid = 1; c->wPid = 1; c->vDriverVersion = 0x100;
    lstrcpyA(c->szPname, "HogsFix CD Audio"); c->cDestinations = 1;
    return 0;
}
static MMRESULT WINAPI my_mixerOpen(HMIXER *h, UINT id, DWORD_PTR cb, DWORD_PTR inst, DWORD fl) { if (h) *h = MIX_H; return 0; }
static MMRESULT WINAPI my_mixerClose(HMIXER h) { return 0; }
static MMRESULT WINAPI my_mixerGetLineInfoA(HMIXEROBJ h, MIXERLINEA *l, DWORD fl) {
    if (!l || l->cbStruct < sizeof *l) return MMSYSERR_INVALPARAM;
    switch (fl & MIXER_GETLINEINFOF_QUERYMASK) {
    case MIXER_GETLINEINFOF_COMPONENTTYPE: if (l->dwComponentType != MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC) return MIXERR_INVALLINE; break;
    case MIXER_GETLINEINFOF_LINEID: if (l->dwLineID != CD_LINE) return MIXERR_INVALLINE; break;
    case MIXER_GETLINEINFOF_SOURCE: if (l->dwDestination || l->dwSource) return MIXERR_INVALLINE; break;
    default: return MIXERR_INVALLINE;
    }
    ZeroMemory(l, sizeof *l); l->cbStruct = sizeof *l;
    l->dwLineID = CD_LINE; l->fdwLine = MIXERLINE_LINEF_ACTIVE | MIXERLINE_LINEF_SOURCE;
    l->dwComponentType = MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC; l->cChannels = 1; l->cControls = 1;
    lstrcpyA(l->szShortName, "CD"); lstrcpyA(l->szName, "CD Audio");
    return 0;
}
static MMRESULT WINAPI my_mixerGetLineControlsA(HMIXEROBJ h, MIXERLINECONTROLSA *lc, DWORD fl) {
    if (!lc || !lc->pamxctrl || lc->cbmxctrl < sizeof(MIXERCONTROLA)) return MMSYSERR_INVALPARAM;
    DWORD q = fl & MIXER_GETLINECONTROLSF_QUERYMASK;
    if ((q == MIXER_GETLINECONTROLSF_ONEBYTYPE && (lc->dwLineID != CD_LINE || lc->dwControlType != MIXERCONTROL_CONTROLTYPE_VOLUME))
        || (q == MIXER_GETLINECONTROLSF_ONEBYID && lc->dwControlID != CD_CTRL)
        || (q == MIXER_GETLINECONTROLSF_ALL && lc->dwLineID != CD_LINE) || q > MIXER_GETLINECONTROLSF_ONEBYTYPE)
        return MIXERR_INVALCONTROL;
    MIXERCONTROLA *c = lc->pamxctrl; ZeroMemory(c, sizeof *c);
    c->cbStruct = sizeof *c; c->dwControlID = CD_CTRL; c->dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
    c->fdwControl = MIXERCONTROL_CONTROLF_UNIFORM; lstrcpyA(c->szShortName, "Volume"); lstrcpyA(c->szName, "CD Volume");
    c->Bounds.dwMaximum = 0xffff; c->Metrics.cSteps = 0xffff;
    lc->dwLineID = CD_LINE;
    return 0;
}
static MMRESULT WINAPI my_mixerGetControlDetailsA(HMIXEROBJ h, MIXERCONTROLDETAILS *d, DWORD fl) {
    if (!d || d->dwControlID != CD_CTRL || !d->paDetails || d->cbDetails < 4) return MIXERR_INVALCONTROL;
    DWORD *v = d->paDetails;
    for (DWORD i = 0; i < (d->cChannels ? d->cChannels : 1); i++) v[i] = cd_vol;
    return 0;
}
static MMRESULT WINAPI my_mixerSetControlDetails(HMIXEROBJ h, MIXERCONTROLDETAILS *d, DWORD fl) {
    if (!d || d->dwControlID != CD_CTRL || !d->paDetails || d->cbDetails < 4) return MIXERR_INVALCONTROL;
    DWORD v = *(DWORD *)d->paDetails;
    v = v > 0xffff ? 0xffff : v;
    if (v != cd_vol) dlog("Music: CD volume %u", v);
    cd_vol = v;
    return 0;
}

void cdaudio_init(void) {
    HMODULE exe = GetModuleHandleA(NULL);
    void **m = find_iat(exe, "winmm.dll", "mciSendCommandA");
    if (!m) { dlog("Music: exe mciSendCommandA import not found"); return; }
    InitializeCriticalSection(&cs);
    wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    real_mci = (mci_t)*m;
    patch_iat(m, (void *)my_mci);
    static const struct { const char *name; void *fn; } mx[] = {
        { "mixerGetNumDevs", my_mixerGetNumDevs }, { "mixerGetDevCapsA", my_mixerGetDevCapsA }, { "mixerOpen", my_mixerOpen },
        { "mixerClose", my_mixerClose }, { "mixerGetLineInfoA", my_mixerGetLineInfoA }, { "mixerGetLineControlsA", my_mixerGetLineControlsA },
        { "mixerGetControlDetailsA", my_mixerGetControlDetailsA }, { "mixerSetControlDetails", my_mixerSetControlDetails },
    };
    for (int i = 0; i < ARRAYSIZE(mx); i++) patch_iat(find_iat(exe, "winmm.dll", mx[i].name), mx[i].fn);
    dlog("Music: CD audio emulation installed (MUSIC\\TrackNN.ogg, volume %d%%)", cfg_music_vol);
}
