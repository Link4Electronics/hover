/*
 * main.c - the native host, for Linux, macOS and Android.
 *
 * The same lifted C as the Windows build (src/recomp/gen), on pcrecomp's
 * runtime/win32hle instead of runtime/native32: the game's Win32 imports are
 * answered by win32hle's own implementation (files, windows and dialogs,
 * GDI, DirectSound on SDL2), so no Windows and no Wine.
 *
 * What is here is what is specific to this game: the guest image, the files
 * it expects beside it, the command line, the level-seed clock, the pad, and
 * headless capture. Online play is not ported yet (src/port/net_offline.c).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#if defined(__linux__) && defined(__x86_64__)
#include <ucontext.h>
#endif
#include <unistd.h>

#include <SDL.h>

#include "hover_port.h"
#include "win32hle.h"
#include "pe_loader.h"
#include "host.h"
#include "recomp_types.h"
#include "levels.h"
#include "mp.h"
#include "net.h"
#include "pad.h"

extern const uint32_t hover_entry_va;   /* recomp_dispatch.c */

#define HOVER_BASE 0x00400000u

static int g_headless, g_fullscreen, g_run;
static const char *g_scale;
static const char *g_record;
static long g_record_frames;
static unsigned g_watchdog_s;
static int g_players;
static const char *g_teams;
static const char *g_ini_opt;

/* Hover! is a GDI game: it renders its own 8-bit DIBs and blits them to its
 * window DC. gdi32's blits land in a host-owned ARGB framebuffer. The screen
 * module's window surface is pointed at the same pixels, so its presenter
 * (scaling, letterboxing, F11/F12) shows them; every pump tells it the pixels
 * changed, since the guest's blit does not go through its tracking. */
#define HOVER_SCREEN_W 512                 /* the game's window, from its first blit */
#define HOVER_SCREEN_H 384
#define HOVER_SCREEN_MAX_W 1280            /* room for a mode the game asks for */
#define HOVER_SCREEN_MAX_H 1024
static uint32_t g_gdi_fb[HOVER_SCREEN_MAX_W * HOVER_SCREEN_MAX_H];
static int g_gdi_w = HOVER_SCREEN_W, g_gdi_h = HOVER_SCREEN_H;

static void wo_retire(void);   /* time-paced waveOut; defined with the shims below */

static void hover_pump(void) {
    wo_retire();               /* due audio blocks: mark done, post MM_WOM_DONE */
    int w, h;                  /* the game's own window is 512x384: follow it, so
                                * the surface carries no unused black border */
    if (hle_primary_window_size(&w, &h) && (w != g_gdi_w || h != g_gdi_h) &&
        w > 0 && h > 0 && w <= HOVER_SCREEN_MAX_W && h <= HOVER_SCREEN_MAX_H) {
        /* Keep what the game drew before the window existed: its first blit
         * lands in the boot-size surface, and the resize used to wipe it,
         * leaving the window black until something else redrew it. */
        static uint32_t saved[HOVER_SCREEN_MAX_W * HOVER_SCREEN_MAX_H];
        int ow = g_gdi_w, oh = g_gdi_h;
        int cw = w < ow ? w : ow, ch = h < oh ? h : oh;
        memcpy(saved, g_gdi_fb, (size_t)ow * (size_t)oh * 4);
        memset(g_gdi_fb, 0, (size_t)w * (size_t)h * 4);
        for (int y = 0; y < ch; y++)
            memcpy(&g_gdi_fb[(size_t)y * w], &saved[(size_t)y * ow], (size_t)cw * 4);
        g_gdi_w = w, g_gdi_h = h;
        hle_gdi_set_target(g_gdi_fb, w, h);
        hle_gdi_window_surface((uint8_t *)g_gdi_fb, w, h, w * 4, 32);
    }
    hle_gdi_window_touched();
    hle_screen_pump();
}

/* ------------------------------------------------------------ shims
 * The imports win32hle does not answer the way this game needs. Registering
 * first wins (win32hle_register); recomp_host_init adds the rest. */

/* Level seeds (levels.c): time() reads the clock only through GetLocalTime,
 * so the level loader's srand(time(NULL)) gets the seed levels_seed() picks.
 * GetTimeZoneInformation says UTC, so time() returns the seed exactly. */
static void shim_GetLocalTime(void) {
    SYSTEMTIME *st = (SYSTEMTIME *)hle_ptr(A32(0));
    time_t t = (time_t)levels_seed();
    struct tm tm;
    gmtime_r(&t, &tm);
    st->wYear = (WORD)(tm.tm_year + 1900);
    st->wMonth = (WORD)(tm.tm_mon + 1);
    st->wDayOfWeek = (WORD)tm.tm_wday;
    st->wDay = (WORD)tm.tm_mday;
    st->wHour = (WORD)tm.tm_hour;
    st->wMinute = (WORD)tm.tm_min;
    st->wSecond = (WORD)tm.tm_sec;
    st->wMilliseconds = 0;
    RET(0, 1);
}

static void shim_GetTimeZoneInformation(void) {
    memset(hle_ptr(A32(0)), 0, sizeof(TIME_ZONE_INFORMATION));
    RET(TIME_ZONE_ID_UNKNOWN, 1);
}

/* The startup display check (0x0041CEE3, and 0x0041CF6D) asks the screen DC
 * for RC_PALETTE and a 256-entry palette, and on every true-colour display
 * since the late 90s says "We have detected that you are not running a 256
 * color video driver". The game renders into its own 8-bit DIBs either way,
 * so only that check is told what it wants to hear. */
static void shim_GetDeviceCaps(void) {
    int w = 640, h = 480;
    uint32_t v = 0;
    hle_gdi_framebuffer(&w, &h);
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    int palette_check = g_cur_func == 0x0041CEE3u || g_cur_func == 0x0041CF6Du;
    switch (A32(1)) {
    case 8:   v = (uint32_t)w; break;                          /* HORZRES   */
    case 10:  v = (uint32_t)h; break;                          /* VERTRES   */
    case 12:  v = 32; break;                                   /* BITSPIXEL */
    case 14:  v = 1; break;                                    /* PLANES    */
    case 104: v = palette_check ? 256 : 0; break;              /* SIZEPALETTE */
    case 38:  v = palette_check ? RC_PALETTE : 0; break;       /* RASTERCAPS  */
    }
    RET(v, 2);
}

static void shim_MessageBoxA(void) {
    const char *text = ASTR(1), *caption = ASTR(2);
    fprintf(stderr, "[messagebox] %s: %s\n", caption ? caption : "", text ? text : "");
    RET(IDOK, 4);
}

/* Dialogs are real here (win32hle's dialog manager runs the game's dialog
 * procs, which is where Start Game and the help pages keep their state). On
 * headless runs nobody can click OK, so press Enter in the dialog as soon as
 * it exists. */
static void on_dialog(uint32_t hwnd, uint32_t id) {
    fprintf(stderr, "[dialog] %u at 0x%08X\n", id, hwnd);
    if (!g_headless) return;
    hle_post_message(hwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001u);
    hle_post_message(hwnd, WM_KEYUP, VK_RETURN, 0xC01C0001u);
}

/* Hover! plays its music through MIDI (winmm), which this HLE has no device
 * for. No output devices makes the game keep its samples but drop the music;
 * the MIDI calls that follow a device count of zero are answered so a failed
 * open cannot be mistaken for a working one. */
#define MMSYSERR_BADDEVICEID 2u
#define MMSYSERR_NODRIVER    6u
static void shim_midiOutGetNumDevs(void)   { RET(0, 0); }
static void shim_midiOutGetDevCapsA(void)  { RET(MMSYSERR_BADDEVICEID, 3); }   /* (id, caps, size) */
static void shim_midiOutOpen(void)         { RET(MMSYSERR_NODRIVER, 5); }      /* (phmo, id, cb, inst, flags) */
static void shim_midiOutClose(void)        { RET(MMSYSERR_NODRIVER, 1); }
static void shim_midiOutPrepareHeader(void){ RET(MMSYSERR_NODRIVER, 3); }
static void shim_midiOutUnprepareHeader(void) { RET(MMSYSERR_NODRIVER, 3); }
static void shim_midiOutReset(void)        { RET(MMSYSERR_NODRIVER, 1); }
static void shim_midiOutShortMsg(void)     { RET(MMSYSERR_NODRIVER, 2); }

/* WAVEMIX still asks this; the OLE-style sound is a no-op that claims success. */
static void shim_sndPlaySoundA(void)       { RET(TRUE, 2); }

/* --- time-paced waveOut, played by the host -----------------------------
 * win32hle's waveOut retires every buffer the moment it is written, and
 * WAVEMIX (the game's sample mixer) drives itself from waveOutGetPosition in
 * TIME_BYTES: with instant completion every block counts as played as soon as
 * it is queued, so the mixer re-mixes as fast as the CPU allows and floods its
 * own window with WM_USER 0x400 "block done" messages. That starves the game's
 * frame step (the picture froze right after a level loaded, and the watchdog
 * sat in the mixer with waveOutWrite as the last import). Pace playback by the
 * host clock instead: the position counts bytes actually played, and a
 * WAVEHDR is only marked done when its bytes are due. Real winmm also posts
 * MM_WOM_DONE to the callback window each time a buffer finishes, and WAVEMIX
 * uses that as its mixer heartbeat -- without it the "Please Wait" level
 * loader waited forever on the level-start sound. Post it from the pump hook
 * (wait_message runs it every millisecond while the queue is empty).
 *
 * The same clock keeps the audio in step, while the samples themselves go to
 * the host mixer (dsound.c): WAVEMIX renders unsigned 8-bit PCM, so convert
 * each block to s16 and push it into a stream. That mixer already honours
 * --mute through its master gain. */
#define WO_HANDLE        0x5A5E0001u
#define WO_WHDR_DONE     0x00000001u
#define WO_WHDR_PREPARED 0x00000002u
#define WO_WHDR_INQUEUE  0x00000010u
#define MM_WOM_OPEN      0x03BBu
#define MM_WOM_DONE      0x03BDu
static double   g_wo_bytes_per_ms = 22.05;         /* nAvgBytesPerSec / 1000 */
static uint64_t g_wo_written, g_wo_played0;        /* bytes played at t0 */
static uint32_t g_wo_t0;
static uint32_t g_wo_hwnd;                         /* CALLBACK_WINDOW target */
static int      g_wo_stream = -1;                  /* host mixer stream, or -1 */
static int      g_wo_rate = 11025, g_wo_ch = 1, g_wo_bits = 8;
static int      g_wo_mute;                         /* --mute: open no device */
static int16_t *g_wo_pcm;
static size_t   g_wo_pcm_n;                        /* capacity in samples */
#define WO_PENDING 64
static uint32_t g_wo_pend_hdr[WO_PENDING];
static uint64_t g_wo_pend_end[WO_PENDING];         /* written total when due */
static unsigned g_wo_pend_n;

static void wo_stream_close(void) {
    if (g_wo_stream >= 0) hle_audio_stream_close(g_wo_stream);
    g_wo_stream = -1;
}
static void wo_stream_open(void) {
    wo_stream_close();
    if (!g_wo_mute && g_wo_rate > 0 && (g_wo_ch == 1 || g_wo_ch == 2))
        g_wo_stream = hle_audio_stream_open(g_wo_rate, g_wo_ch);
}

static uint64_t wo_played(void) {
    uint32_t now = hle_ticks_ms();
    if (!g_wo_t0) { g_wo_t0 = now; return 0; }
    uint64_t played = g_wo_played0 + (uint64_t)((double)(now - g_wo_t0) * g_wo_bytes_per_ms);
    return played < g_wo_written ? played : g_wo_written;
}
static void wo_retire(void) {
    uint64_t played = wo_played();
    for (unsigned i = 0; i < g_wo_pend_n; ) {
        if (g_wo_pend_end[i] <= played) {
            uint32_t h = g_wo_pend_hdr[i];
            if (h) {
                MEM32(h + 16) = (MEM32(h + 16) | WO_WHDR_DONE) & ~WO_WHDR_INQUEUE;
                MEM32(h + 8) = MEM32(h + 4);           /* dwBytesRecorded */
                if (g_wo_hwnd) hle_post_message(g_wo_hwnd, MM_WOM_DONE, WO_HANDLE, h);
            }
            g_wo_pend_hdr[i] = g_wo_pend_hdr[g_wo_pend_n - 1];
            g_wo_pend_end[i] = g_wo_pend_end[g_wo_pend_n - 1];
            g_wo_pend_n--;
        } else {
            i++;
        }
    }
}
static void shim_waveOutOpen(void) {                  /* (&hwo, id, &fmt, cb, inst, flags) */
    uint32_t fx = A32(2);
    g_wo_rate = 0;
    if (fx) {
        uint32_t avg = MEM32(fx + 8);                 /* nAvgBytesPerSec */
        if (!avg) avg = (uint32_t)MEM16(fx + 4) * MEM16(fx + 2) * (uint32_t)(MEM16(fx + 14) / 8);
        if (avg) g_wo_bytes_per_ms = avg / 1000.0;
        /* WAVEMIX uses WAVE_FORMAT_PCM; anything else stays silent rather
         * than feeding the mixer a format it cannot convert. */
        uint16_t tag = MEM16(fx), ch = MEM16(fx + 2), bits = MEM16(fx + 14);
        if (tag == 1 && (ch == 1 || ch == 2) && (bits == 8 || bits == 16) &&
            MEM32(fx + 4) >= 4000 && MEM32(fx + 4) <= 192000) {
            g_wo_rate = (int)MEM32(fx + 4), g_wo_ch = ch, g_wo_bits = bits;
        }
    }
    g_wo_written = g_wo_played0 = 0;
    g_wo_pend_n = 0;
    g_wo_t0 = 0;
    g_wo_hwnd = (A32(5) & 0x70000u) == 0x10000u /* CALLBACK_WINDOW */ ? A32(3) : 0;
    if (!(A32(5) & 1u))                               /* WAVE_FORMAT_QUERY */
        wo_stream_open();
    if (A32(0)) MEM32(A32(0)) = WO_HANDLE;
    if (g_wo_hwnd) hle_post_message(g_wo_hwnd, MM_WOM_OPEN, WO_HANDLE, A32(4));
    RET(0, 6);
}
static void shim_waveOutWrite(void) {                 /* (hwo, &hdr, cbwh) */
    wo_retire();
    uint32_t h = A32(1);
    if (h) {
        if (!g_wo_t0) g_wo_t0 = hle_ticks_ms();
        uint32_t len = MEM32(h + 4);                  /* dwBufferLength */
        g_wo_written += len;
        MEM32(h + 16) = (MEM32(h + 16) | WO_WHDR_INQUEUE) & ~WO_WHDR_DONE;
        if (g_wo_pend_n < WO_PENDING) {
            g_wo_pend_hdr[g_wo_pend_n] = h;
            g_wo_pend_end[g_wo_pend_n] = g_wo_written;
            g_wo_pend_n++;
        }
        if (g_wo_stream >= 0) {
            const uint8_t *src = (const uint8_t *)hle_ptr(MEM32(h));
            size_t bpf = (size_t)(g_wo_bits / 8) * (size_t)g_wo_ch;
            size_t frames = bpf ? len / bpf : 0;
            if (frames > (1u << 20)) frames = 1u << 20;
            if (src && frames) {
                size_t n = frames * (size_t)g_wo_ch;
                if (g_wo_pcm_n < n) {
                    int16_t *p = (int16_t *)realloc(g_wo_pcm, n * sizeof *p);
                    if (p) g_wo_pcm = p, g_wo_pcm_n = n;
                    else frames = 0;
                }
                if (frames) {
                    if (g_wo_bits == 8) {
                        for (size_t i = 0; i < n; i++)
                            g_wo_pcm[i] = (int16_t)(((int)src[i] - 128) << 8);
                    } else {
                        for (size_t i = 0; i < n; i++)
                            memcpy(&g_wo_pcm[i], src + 2 * i, 2);
                    }
                    hle_audio_stream_push(g_wo_stream, g_wo_pcm, (int)frames);
                }
            }
        }
    }
    RET(0, 3);
}
static void shim_waveOutGetPosition(void) {           /* (hwo, &MMTIME, cb) */
    wo_retire();
    uint32_t mmt = A32(1);
    if (mmt && A32(2) >= 8) {
        uint64_t played = wo_played();
        MEM32(mmt + 4) = MEM32(mmt) == 0 /* TIME_MS */ ? (uint32_t)(played / g_wo_bytes_per_ms)
                                                       : (uint32_t)played;   /* TIME_BYTES */
        if (A32(2) >= 12) MEM32(mmt + 8) = 0;
    }
    RET(0, 3);
}
static void shim_waveOutPrepareHeader(void)   { uint32_t h = A32(1); if (h) MEM32(h + 16) |= WO_WHDR_PREPARED; RET(0, 3); }
static void shim_waveOutUnprepareHeader(void) { wo_retire(); uint32_t h = A32(1); if (h) MEM32(h + 16) &= ~WO_WHDR_PREPARED; RET(0, 3); }
static void shim_waveOutReset(void) {
    g_wo_played0 = g_wo_written;
    g_wo_t0 = hle_ticks_ms();
    for (unsigned i = 0; i < g_wo_pend_n; i++)
        if (g_wo_pend_hdr[i]) {
            MEM32(g_wo_pend_hdr[i] + 16) = (MEM32(g_wo_pend_hdr[i] + 16) | WO_WHDR_DONE) & ~WO_WHDR_INQUEUE;
            if (g_wo_hwnd) hle_post_message(g_wo_hwnd, MM_WOM_DONE, WO_HANDLE, g_wo_pend_hdr[i]);
        }
    g_wo_pend_n = 0;
    wo_stream_open();                                  /* drop queued samples */
    RET(0, 1);
}
static void shim_waveOutClose(void)           { wo_stream_close(); RET(0, 1); }
static void shim_waveOutPause(void)           { RET(0, 1); }
static void shim_waveOutRestart(void)         { RET(0, 1); }

/* The old Win16-style open, still used for a probe at 0x0041C7E0. Log what it
 * wants and fail like a missing file; if the game needs the file, the name
 * here says which one. */
static void shim_OpenFile(void) {
    fprintf(stderr, "[openfile] %s (style %X)\n", ASTR(0) ? ASTR(0) : "?", A32(2));
    RET(0xFFFFFFFFu, 3);                              /* HFILE_ERROR */
}

/* GDI batching hints: no batching here, and a flushed device is always flushed. */
static void shim_GdiSetBatchLimit(void) { RET(0, 1); }
static void shim_GdiFlush(void)         { RET(TRUE, 0); }

/* ExtTextOutA(hdc, x, y, opts, rect, str, count, dx): the game's "Please
 * Wait" and dialog text draws unclipped with no per-character spacing, which
 * is TextOutA. win32hle has that (SDL_ttf), so call it rather than keeping a
 * second text renderer here. */
static void shim_ExtTextOutA(void) {
    uint32_t va = hle_resolve("TextOutA");
    if (!va) { RET(FALSE, 8); return; }
    uint32_t args[5] = { A32(0), A32(1), A32(2), A32(5), A32(6) };
    hle_call_guest(va, 5, args);
    RET(TRUE, 8);
}

/* VkKeyScanA(ch): US-layout character -> (shift << 8) | virtual-key. The
 * in-game key handler maps printable binding characters ('+', '-', letters)
 * through this to compare against WM_KEYDOWN virtual keys. */
static void shim_VkKeyScanA(void) {
    unsigned ch = A32(0) & 0xFFu, vk = 0xFFFFu, shift = 0;
    if (ch == ' ') vk = 0x20;
    else if (ch >= '0' && ch <= '9') vk = ch;
    else if (ch >= 'a' && ch <= 'z') vk = ch - 'a' + 'A';
    else if (ch >= 'A' && ch <= 'Z') { vk = ch; shift = 1; }
    else switch (ch) {
    case ';': vk = 0xBA; break;           case ':': vk = 0xBA; shift = 1; break;
    case '=': vk = 0xBB; break;           case '+': vk = 0xBB; shift = 1; break;
    case ',': vk = 0xBC; break;           case '<': vk = 0xBC; shift = 1; break;
    case '-': vk = 0xBD; break;           case '_': vk = 0xBD; shift = 1; break;
    case '.': vk = 0xBE; break;           case '>': vk = 0xBE; shift = 1; break;
    case '/': vk = 0xBF; break;           case '?': vk = 0xBF; shift = 1; break;
    case '`': vk = 0xC0; break;           case '~': vk = 0xC0; shift = 1; break;
    case '[': vk = 0xDB; break;           case '{': vk = 0xDB; shift = 1; break;
    case '\\': vk = 0xDC; break;          case '|': vk = 0xDC; shift = 1; break;
    case ']': vk = 0xDD; break;           case '}': vk = 0xDD; shift = 1; break;
    case '\'': vk = 0xDE; break;          case '"': vk = 0xDE; shift = 1; break;
    }
    RET((shift << 8) | vk, 1);
}

/* Window state queries the game makes around its dialogs. There is one
 * window, never iconified, zoomed, a child, or parent of anything, and no
 * message queue position beyond the current message's zero. */
static void shim_GetLastActivePopup(void) { RET(A32(0), 1); }
static void shim_IsIconic(void)           { RET(FALSE, 1); }
static void shim_IsZoomed(void)           { RET(FALSE, 1); }
static void shim_IsChild(void)            { RET(FALSE, 2); }
static void shim_SetParent(void)          { RET(TRUE, 2); }
static void shim_ShowOwnedPopups(void)    { RET(TRUE, 2); }
static void shim_GetMessagePos(void)      { RET(0, 0); }
static void shim_GetMessageTime(void)     { RET(0, 0); }
static void shim_SystemParametersInfoA(void) { RET(TRUE, 4); }

/* Window properties: a tiny store keyed by window and name. The game uses
 * them to hang its per-window state off the window it owns; WAVEMIX keys
 * its properties by atom (an integer below 0x10000), everything else by
 * string. */
#define MAX_PROPS 64
static struct { uint32_t wnd, atom; char *name; uint32_t val; } g_props[MAX_PROPS];
static int prop_find(uint32_t wnd, uint32_t key) {
    int is_atom = key < 0x10000u;
    const char *name = is_atom ? NULL : (const char *)hle_ptr(key);
    for (int i = 0; i < MAX_PROPS; i++) {
        if (!g_props[i].name && !g_props[i].atom) continue;
        if (g_props[i].wnd != wnd) continue;
        if (g_props[i].atom || is_atom) { if (g_props[i].atom == key) return i; }
        else if (!strcmp(g_props[i].name, name)) return i;
    }
    return -1;
}
static void shim_SetPropA(void) {
    uint32_t key = A32(1);
    int is_atom = key < 0x10000u;
    const char *name = is_atom ? NULL : ASTR(1);
    if (!is_atom && !name) { RET(FALSE, 3); return; }
    int i = prop_find(A32(0), key);
    if (i < 0) for (i = 0; i < MAX_PROPS && (g_props[i].name || g_props[i].atom); i++) ;
    if (i >= MAX_PROPS) { RET(FALSE, 3); return; }
    if (!is_atom && !g_props[i].name) g_props[i].name = strdup(name);
    g_props[i].wnd = A32(0);
    g_props[i].atom = is_atom ? key : 0;
    g_props[i].val = A32(2);
    RET(TRUE, 3);
}
static void shim_GetPropA(void) {
    int i = prop_find(A32(0), A32(1));
    RET(i < 0 ? 0u : g_props[i].val, 2);
}
static void shim_RemovePropA(void) {
    int i = prop_find(A32(0), A32(1));
    if (i < 0) { RET(0, 2); return; }
    uint32_t v = g_props[i].val;
    free(g_props[i].name);
    g_props[i].name = NULL;
    g_props[i].atom = 0;
    RET(v, 2);
}

static const win32hle_shim g_shims[] = {
    { "GetLocalTime", shim_GetLocalTime },
    { "GetTimeZoneInformation", shim_GetTimeZoneInformation },
    { "GetDeviceCaps", shim_GetDeviceCaps },
    { "midiOutGetNumDevs", shim_midiOutGetNumDevs },
    { "midiOutGetDevCapsA", shim_midiOutGetDevCapsA },
    { "midiOutOpen", shim_midiOutOpen },
    { "midiOutClose", shim_midiOutClose },
    { "midiOutPrepareHeader", shim_midiOutPrepareHeader },
    { "midiOutUnprepareHeader", shim_midiOutUnprepareHeader },
    { "midiOutReset", shim_midiOutReset },
    { "midiOutShortMsg", shim_midiOutShortMsg },
    { "sndPlaySoundA", shim_sndPlaySoundA },
    { "waveOutOpen", shim_waveOutOpen },
    { "waveOutWrite", shim_waveOutWrite },
    { "waveOutPrepareHeader", shim_waveOutPrepareHeader },
    { "waveOutUnprepareHeader", shim_waveOutUnprepareHeader },
    { "waveOutGetPosition", shim_waveOutGetPosition },
    { "waveOutReset", shim_waveOutReset },
    { "waveOutClose", shim_waveOutClose },
    { "waveOutPause", shim_waveOutPause },
    { "waveOutRestart", shim_waveOutRestart },
    { "OpenFile", shim_OpenFile },
    { "GdiSetBatchLimit", shim_GdiSetBatchLimit },
    { "GdiFlush", shim_GdiFlush },
    { "ExtTextOutA", shim_ExtTextOutA },
    { "VkKeyScanA", shim_VkKeyScanA },
    { "GetLastActivePopup", shim_GetLastActivePopup },
    { "IsIconic", shim_IsIconic },
    { "IsZoomed", shim_IsZoomed },
    { "IsChild", shim_IsChild },
    { "SetParent", shim_SetParent },
    { "ShowOwnedPopups", shim_ShowOwnedPopups },
    { "GetMessagePos", shim_GetMessagePos },
    { "GetMessageTime", shim_GetMessageTime },
    { "SystemParametersInfoA", shim_SystemParametersInfoA },
    { "SetPropA", shim_SetPropA },
    { "GetPropA", shim_GetPropA },
    { "RemovePropA", shim_RemovePropA },
    { 0, 0 }
};
static const win32hle_shim g_headless_shims[] = {
    { "MessageBoxA", shim_MessageBoxA },
    { 0, 0 }
};

/* Host replacements for lifted functions reached by indirect calls: the
 * multiplayer seats take over CRobotPlayer::Think (mp.c). */
recomp_func_t recomp_lookup_manual(uint32_t va) { return mp_lookup(va); }

/* ------------------------------------------------------------ faults */
static void report(const char *why) {
    fprintf(stderr, "\n=== %s ===\n  in lifted sub_%08X, last import %s\n", why, g_cur_func,
            g_cur_import ? g_cur_import : "(none)");
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    fprintf(stderr, "guest stack at %08X:", g_esp);
    for (int k = 0; k < 64; k++) fprintf(stderr, "%s%08X", k % 8 ? " " : "\n  ", MEM32(g_esp + 4 * k));
    fprintf(stderr, "\n");
}
static void on_fault(int sig, siginfo_t *si, void *uc) {
    static volatile int once;
    if (__sync_lock_test_and_set(&once, 1)) _exit(3);
#if defined(__linux__) && defined(__x86_64__)
    ucontext_t *u = (ucontext_t *)uc;
    fprintf(stderr, "\n  (host pc %p, guest %08X)", (void *)u->uc_mcontext.gregs[REG_RIP], g_cur_func);
#endif
    char why[96];
    snprintf(why, sizeof why, "fault: signal %d, address %p", sig, si->si_addr);
    report(why);
    fflush(stderr);
    _exit(3);
}
static void *watchdog(void *arg) {
    (void)arg;
    sleep(g_watchdog_s);
    fprintf(stderr, "\n[watchdog] %u s: in sub_%08X, last import %s, %u indirect calls\n",
            g_watchdog_s, g_cur_func, g_cur_import ? g_cur_import : "(none)", g_icall_count);
    fflush(stderr);
    hle_exit(4);
    return NULL;
}

/* ------------------------------------------------------------ recording
 * --record out.mp4: the GDI window surface (the game blits its DIBs there),
 * 30 frames a second, piped to ffmpeg. --frames N stops after N frames. */
#define REC_MAX_W 1920
#define REC_MAX_H 1200

static pthread_mutex_t g_rec_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_ffmpeg;
static long g_recorded;
static void record_close(void) {
    pthread_mutex_lock(&g_rec_lock);
    if (g_ffmpeg) {
        pclose(g_ffmpeg);           /* A closed pipe is a finished mp4 */
        g_ffmpeg = NULL;
        fprintf(stderr, "[record] %ld frames -> %s\n", g_recorded, g_record);
    }
    pthread_mutex_unlock(&g_rec_lock);
}
static void on_exit_close(int code) { (void)code; record_close(); }

static uint32_t checksum(const uint8_t *px, int w, int h, int pitch, int bpp) {
    uint32_t sum = 0;
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w * bpp / 8; x += 16) sum = sum * 31 + px[(size_t)y * pitch + x];
    return sum;
}

static void *recorder(void *unused) {
    static uint32_t frame[REC_MAX_W * REC_MAX_H];
    int rec_w = 0, rec_h = 0;
    uint32_t start = hle_ticks_ms(), next = start;
    (void)unused;
    for (;;) {
        next += 33;
        int32_t wait = (int32_t)(next - hle_ticks_ms());
        if (wait > 0) usleep((useconds_t)wait * 1000u);
        const uint8_t *px;
        int w, h, pitch, bpp;
        unsigned seq;
        int ok;
        mach_enter();                                    /* the surface can go away under a mode change */
        ok = hle_gdi_window_frame(&px, &w, &h, &pitch, &bpp, &seq) &&
             (bpp == 32 || bpp == 16) && w >= 2 && h >= 2 && w <= REC_MAX_W && h <= REC_MAX_H;
        if (ok && !g_recorded) {                         /* the surface follows the game's window:
                                                          * latch its size, not the boot default's */
            int pw, ph;
            if (!(hle_primary_window_size(&pw, &ph) && pw == w && ph == h) &&
                hle_ticks_ms() - start < 2000) { mach_leave(); continue; }
        }
        for (int y = 0; ok && y < h; y++) {
            if (bpp == 32) memcpy(frame + (size_t)y * w, px + (size_t)y * pitch, (size_t)w * 4);
            else {
                const uint16_t *s = (const uint16_t *)(px + (size_t)y * pitch);
                uint32_t *d = frame + (size_t)y * w;
                for (int x = 0; x < w; x++) {
                    uint16_t p = s[x];
                    uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                    d[x] = (b << 3 | b >> 2) | (g << 2 | g >> 4) << 8 | (r << 3 | r >> 2) << 16;
                }
            }
        }
        uint32_t sum = ok ? checksum((const uint8_t *)frame, w, h, w * 4, 32) : 0;
        mach_leave();
        if (!ok) continue;
        pthread_mutex_lock(&g_rec_lock);
        if (!g_ffmpeg && g_recorded) { pthread_mutex_unlock(&g_rec_lock); return NULL; }
        if (!g_ffmpeg) {
            char cmd[PATH_MAX + 256];
            int k = (w + 1279) / 1280;
            rec_w = (w / k) & ~1, rec_h = (h / k) & ~1;
            if (rec_w < 2) rec_w = 2;
            if (rec_h < 2) rec_h = 2;
            snprintf(cmd, sizeof cmd, "ffmpeg -y -loglevel error -f rawvideo -pix_fmt bgr0 -s %dx%d -r 30 "
                     "-i - -c:v libx264 -pix_fmt yuv420p '%s'", rec_w, rec_h, g_record);
            g_ffmpeg = popen(cmd, "w");
            fprintf(stderr, "[record] %dx%d -> %s\n", rec_w, rec_h, g_record);
            if (!g_ffmpeg) { pthread_mutex_unlock(&g_rec_lock); return NULL; }
        }
        if (g_recorded % 100 == 0)
            fprintf(stderr, "[record] frame %ld checksum %08X\n", g_recorded, sum);
        /* Fit the frame into the recording's fixed size, nearest neighbour. */
        int fw = rec_w, fh = (int)((long long)rec_w * h / w);
        if (fh > rec_h) fh = rec_h, fw = (int)((long long)rec_h * w / h);
        int ox = (rec_w - fw) / 2, oy = (rec_h - fh) / 2;
        static uint32_t out[REC_MAX_W];
        for (int y = 0; y < rec_h; y++) {
            memset(out, 0, (size_t)rec_w * 4);
            if (y >= oy && y < oy + fh) {
                const uint32_t *src = frame + (size_t)((y - oy) * h / fh) * w;
                for (int x = 0; x < fw; x++) out[ox + x] = src[x * w / fw];
            }
            fwrite(out, 4, (size_t)rec_w, g_ffmpeg);
        }
        if (ferror(g_ffmpeg)) {
            fprintf(stderr, "[record] ffmpeg is not taking frames: not recording\n");
            pclose(g_ffmpeg);
            g_ffmpeg = NULL;
            pthread_mutex_unlock(&g_rec_lock);
            return NULL;
        }
        g_recorded++;
        int done = g_record_frames > 0 && g_recorded >= g_record_frames;
        pthread_mutex_unlock(&g_rec_lock);
        if (done) {
            record_close();
            hle_exit(0);
        }
    }
}

/* ------------------------------------------------------------ scripted keys
 * --key NAME@MS[+HOLD], as the Windows host: the game takes its menu keys
 * (F2, F3) as WM_KEYDOWN through the accelerator table and steers by
 * polling, so a scripted key is both. */
#define MAX_KEYS 64
static struct { int vk; unsigned at, hold; } g_keys[MAX_KEYS];
static int g_nkeys;

static int vk_of(const char *s) {
    static const struct { const char *n; int vk; } names[] = {
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"ESC", VK_ESCAPE}, {"SHIFT", VK_SHIFT},
        {"CTRL", VK_CONTROL}, {"TAB", VK_TAB}};
    for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
        if (!strcasecmp(s, names[i].n)) return names[i].vk;
    if ((s[0] == 'F' || s[0] == 'f') && atoi(s + 1) >= 1 && atoi(s + 1) <= 12) return VK_F1 + atoi(s + 1) - 1;
    if (s[0] && !s[1]) return toupper((unsigned char)s[0]);
    return (int)strtol(s, NULL, 0);
}
static int parse_key(const char *arg) {
    char name[16];
    unsigned at = 0, hold = 100;
    if (g_nkeys >= MAX_KEYS || sscanf(arg, "%15[^@]@%u+%u", name, &at, &hold) < 2) return 0;
    g_keys[g_nkeys].vk = vk_of(name) & 0xFF;
    g_keys[g_nkeys].at = at;
    g_keys[g_nkeys++].hold = hold;
    return 1;
}
static void *key_thread(void *p) {
    int i = (int)(intptr_t)p;
    uint32_t t0 = hle_ticks_ms();
    while (!hle_first_hwnd() && hle_ticks_ms() - t0 < 10000) usleep(5000);
    while (hle_ticks_ms() - t0 < g_keys[i].at) usleep(5000);
    int vk = g_keys[i].vk;
    uint32_t w = hle_focus_window();
    if (!w) w = hle_first_hwnd();
    fprintf(stderr, "[input] key 0x%02X down at %u ms\n", vk, hle_ticks_ms() - t0);
    mach_enter();
    hle_input_key_state((uint32_t)vk, 1);
    mach_leave();
    if (w) hle_post_message(w, WM_KEYDOWN, (uint32_t)vk, 1);
    usleep(g_keys[i].hold * 1000u);
    mach_enter();
    hle_input_key_state((uint32_t)vk, 0);
    mach_leave();
    if (w) hle_post_message(w, WM_KEYUP, (uint32_t)vk, 0xC0000001u);
    return NULL;
}

/* --menu ID@MS: a menu command straight to the main window (the menus are
 * real Win32 menus, so this is the automation that does not depend on
 * translating keys). */
#define MAX_CMDS 32
#define WM_COMMAND 0x0111u
static struct { unsigned id, at; } g_cmds[MAX_CMDS];
static int g_ncmds;
static int parse_menu(const char *arg) {
    unsigned id = 0, at = 0;
    if (g_ncmds >= MAX_CMDS || sscanf(arg, "%u@%u", &id, &at) != 2) return 0;
    g_cmds[g_ncmds].id = id;
    g_cmds[g_ncmds++].at = at;
    return 1;
}
static void *menu_thread(void *p) {
    int i = (int)(intptr_t)p;
    uint32_t t0 = hle_ticks_ms();
    while (!hle_first_hwnd() && hle_ticks_ms() - t0 < 10000) usleep(5000);
    while (hle_ticks_ms() - t0 < g_cmds[i].at) usleep(5000);
    uint32_t w = hle_first_hwnd();
    fprintf(stderr, "[input] menu %u at %u ms\n", g_cmds[i].id, hle_ticks_ms() - t0);
    if (w) hle_post_message(w, WM_COMMAND, g_cmds[i].id, 0);
    return NULL;
}

/* ------------------------------------------------------------ settings */

static char g_ini[PATH_MAX];

static void ini_path(void) {
    if (g_ini_opt) { snprintf(g_ini, sizeof g_ini, "%s", g_ini_opt); return; }
    const char *env = getenv("HOVER_INI");
    if (env && *env) { snprintf(g_ini, sizeof g_ini, "%s", env); return; }
    char *pref = SDL_GetPrefPath("hover", "hover");   /* Linux ~/.local/share, macOS Application Support, Android */
    if (pref) {
        snprintf(g_ini, sizeof g_ini, "%shover.ini", pref);
        SDL_free(pref);
        return;
    }
    if (hle_exe_path(g_ini, sizeof g_ini - 8)) {
        char *slash = strrchr(g_ini, '/');
        snprintf(slash ? slash + 1 : g_ini, 8, "hover.ini");
        return;
    }
    snprintf(g_ini, sizeof g_ini, "hover.ini");
}

static void settings_load(char *scale, size_t n, int *bars, int *full) {
    char b[64];
    snprintf(scale, n, "%s", "sharp");
    GetPrivateProfileStringA("present", "scale", "sharp", b, sizeof b, g_ini);
    snprintf(scale, n, "%s", b);
    GetPrivateProfileStringA("present", "bars", "blur", b, sizeof b, g_ini);
    *bars = strcasecmp(b, "black") != 0;
    *full = (int)GetPrivateProfileIntA("present", "fullscreen", 0, g_ini);
}
static void settings_save(const char *scale, int bars, int full) {
    WritePrivateProfileStringA("present", "scale", scale, g_ini);
    WritePrivateProfileStringA("present", "bars", bars ? "blur" : "black", g_ini);
    char b[8];
    snprintf(b, sizeof b, "%d", full);
    WritePrivateProfileStringA("present", "fullscreen", b, g_ini);
}

/* ------------------------------------------------------------ main */

/* Does dir directly hold HOVER.EXE? Looked up through the guest path code so
 * the name matches without case, as the game's own file opens do. */
static int has_hover_exe(const char *dir) {
    char exe[PATH_MAX];
    hle_set_drive('C', dir);
    return hle_host_path("C:\\HOVER.EXE", exe, sizeof exe);
}

static void usage(void) {
    printf("usage: hover [--run] [--headless] [--fullscreen] [--scale sharp|smooth|crt|nearest|integer]\n"
           "             [--game DIR] [--ini FILE] [--seed N] [--players N] [--teams hrr...]\n"
           "             [--record out.mp4] [--frames N] [--key NAME@MS[+HOLD]]\n"
           "             [--mute] [--watchdog S] [--trace]\n"
           "             [--pad-selftest] [--levels-selftest] [--net-selftest]\n");
}

int main(int argc, char **argv) {
    const char *game = "game";
    int game_opt = 0;
    int pad_test = 0, levels_test = 0, net_test = 0, seed_pinned = 0, mute = 0;
    uint32_t seed = 0;

    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--run")) g_run = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--fullscreen")) g_fullscreen = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) g_scale = argv[++i];
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) { game = argv[++i]; game_opt = 1; }
        else if (!strcmp(argv[i], "--ini") && i + 1 < argc) g_ini_opt = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) { seed = strtoul(argv[++i], NULL, 10); seed_pinned = 1; }
        else if (!strcmp(argv[i], "--players") && i + 1 < argc) g_players = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--teams") && i + 1 < argc) g_teams = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) g_record = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_record_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            if (!parse_key(argv[++i])) { fprintf(stderr, "bad --key %s\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--menu") && i + 1 < argc) {
            if (!parse_menu(argv[++i])) { fprintf(stderr, "bad --menu %s\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--mute")) mute = 1;
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--trace")) win32hle_trace = 1;
        else if (!strcmp(argv[i], "--pad-selftest")) pad_test = 1;
        else if (!strcmp(argv[i], "--levels-selftest")) levels_test = 1;
        else if (!strcmp(argv[i], "--net-selftest")) net_test = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); usage(); return 1; }
    }

    signal(SIGPIPE, SIG_IGN);
    {   /* faults report on their own stack: a guest stack overflow is one */
        static uint8_t alt[1 << 16];
        stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt };
        sigaltstack(&ss, NULL);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
    }

    win32hle_register(g_shims);                       /* the game's own answers win */
    if (g_headless) win32hle_register(g_headless_shims);
    hle_dialog_hook = on_dialog;
    recomp_host_init();

    if (pad_test || levels_test || net_test) {        /* these need no game files */
        hle_vm_reserve(0x00460000u, 0x1000u);         /* the key table the selftests poke */
        if (levels_test) return levels_selftest();
        if (pad_test) return pad_selftest();
        return net_selftest();
    }

    /* The game folder: DIR with HOVER.EXE in it, or DIR/hover when DIR is
     * the checkout-style layout. Without --game, look in the current
     * directory first (the AppImage hook starts in the per-user folder,
     * ~/.local/share/hover, with the player's files directly in it), then in
     * game/hover and game/ (the checkout and older layouts). On Android
     * there is nothing sensible next to the app, so the default also looks
     * in the app's own external files directory, which the player fills from
     * their copy of the game with no permission needed:
     *   /sdcard/Android/data/com.hover.game/files/game/hover/ */
    char game_full[PATH_MAX], exe_host[PATH_MAX] = "";
    int have_game = 0;
    if (game_opt) {
        have_game = realpath(game, game_full) != NULL;
    }
#ifdef __ANDROID__
    else {
        const char *ext = SDL_AndroidGetExternalStoragePath();
        char android_game[PATH_MAX];
        if (ext && snprintf(android_game, sizeof android_game, "%s/game/hover", ext) < (int)sizeof android_game)
            have_game = realpath(android_game, game_full) != NULL;
    }
#else
    else {
        /* The current directory is the AppImage hook's per-user folder
         * (~/.local/share/hover), with the player's files directly in it;
         * game/hover is the checkout layout and game/ an older one. */
        char cand[PATH_MAX];
        if (realpath(".", cand) && has_hover_exe(cand)) {
            snprintf(game_full, sizeof game_full, "%s", cand);
            have_game = 1;
        } else if (realpath("game/hover", cand) && has_hover_exe(cand)) {
            snprintf(game_full, sizeof game_full, "%s", cand);
            have_game = 1;
        } else if (realpath("game", game_full) || realpath(".", game_full)) {
            have_game = 1;
        }
    }
#endif
    if (!have_game) { fprintf(stderr, "no game folder at %s\n", game); return 1; }
    hle_set_drive('C', game_full);
    if (!hle_host_path("C:\\HOVER.EXE", exe_host, sizeof exe_host)) {
        char nested[PATH_MAX], resolved[PATH_MAX];
        snprintf(nested, sizeof nested, "%s/hover", game_full);
        if (realpath(nested, resolved)) {
            snprintf(game_full, sizeof game_full, "%s", resolved);
            hle_set_drive('C', game_full);
            hle_host_path("C:\\HOVER.EXE", exe_host, sizeof exe_host);
        }
    }

    pe_image img;
    int mapped = 0;
    if (exe_host[0] && recomp_pe_map(exe_host, &img) == 0 && img.base == HOVER_BASE) {
        mapped = 1;
        hle_module_add("C:\\HOVER.EXE", img.base, img.resource_rva, img.resource_size, 1);
        int unresolved = recomp_pe_bind(&img, hle_resolve_or_stub);
        hle_set_command_line("\"C:\\HOVER.EXE\"");
        mp_apply_table();
        printf("  mapped HOVER.EXE: 0x%08X-0x%08X, %d imports not implemented yet\n",
               img.base, img.base + img.span, unresolved);
    } else if (recomp_dispatch_count) {
        fprintf(stderr, "cannot map HOVER.EXE at 0x%08X from %s\n", HOVER_BASE, game_full);
        return 1;
    }

    if (!mapped) {
        fprintf(stderr, "no HOVER.EXE in %s\n", game_full);
        return 1;
    }
    printf("Hover! recomp host (native %s)\n  lifted functions in dispatch: %u\n",
#ifdef __APPLE__
           "macOS",
#else
           "Linux",
#endif
           recomp_dispatch_count);
    if (!recomp_dispatch_count) {
        fprintf(stderr, "hover: this build has no lifted game code (src/recomp/gen was empty).\n"
                        "  Run `python3 run_lift.py` with your copy of the game in game/hover/,\n"
                        "  then rebuild; the AppImage CI builds cannot contain it.\n");
        if (g_run) return 1;
    }
    if (!g_run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", hover_entry_va);
        return 0;
    }

    if (chdir(game_full) != 0) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    hle_gdi_set_target(g_gdi_fb, g_gdi_w, g_gdi_h);
    hle_gdi_window_surface((uint8_t *)g_gdi_fb, g_gdi_w, g_gdi_h, g_gdi_w * 4, 32);
    if (hle_screen_open("Hover!", g_fullscreen, g_headless) != 0) return 1;
    hle_set_pump_hook(hover_pump);
    g_wo_mute = mute;
    ini_path();
    char scale[32];
    int bars = 1, full = g_fullscreen;
    settings_load(scale, sizeof scale, &bars, &full);
    if (g_scale) snprintf(scale, sizeof scale, "%s", g_scale);
    if (!hle_screen_scale(scale))
        fprintf(stderr, "[host] no scaling \"%s\": sharp, smooth, crt, nearest or integer\n", scale);
    hle_screen_bars(bars);
    hle_screen_settings_hook = settings_save;
    levels_init(g_ini, seed, seed_pinned);
    levels_attach(0, hle_screen_title);
    mp_init(g_ini, g_players, pad_held);
    if (g_teams) mp_set_teams(g_teams);
    pad_start(g_ini, 0);
    if (mute) hle_dsound_set_master(0.0f);
    char regfile[PATH_MAX + 16];
    if (!g_headless) {                        /* high scores and options survive a run */
        snprintf(regfile, sizeof regfile, "%s.registry", g_ini);
        hle_reg_load(regfile);
    }

    hle_on_exit = on_exit_close;
    if (g_record) {
        pthread_t t;
        if (pthread_create(&t, NULL, recorder, NULL) == 0) pthread_detach(t);
    }
    for (int i = 0; i < g_nkeys; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, key_thread, (void *)(intptr_t)i) == 0) pthread_detach(t);
    }
    for (int i = 0; i < g_ncmds; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, menu_thread, (void *)(intptr_t)i) == 0) pthread_detach(t);
    }
    if (g_watchdog_s) {
        pthread_t t;
        if (pthread_create(&t, NULL, watchdog, NULL) == 0) pthread_detach(t);
    }

    printf("  entering 0x%08X\n\n", hover_entry_va);
    fflush(stdout);
    if (!hover_entry_va) {                    /* HOVER_GEN_STUB: no lifted code */
        fprintf(stderr, "this is a stub build (HOVER_GEN_STUB): there is no lifted code to run\n");
        return 1;
    }
    uint32_t rc = hle_call_guest(hover_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", rc);
    record_close();
    hle_exit((int)rc);
    return 0;
}
