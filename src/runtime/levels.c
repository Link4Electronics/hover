/*
 * Level seeds: pin them, show them, save them, share them.
 * docs/levels.md has the reasoning.
 *
 * Every level load does srand(time(NULL)) (0x004154DC), and the seed places
 * everything that is not a wall: the player's and robots' starts, the 28
 * pods and the flags. So a level is (level number, seed), and pinning the
 * seed replays it exactly. time() (0x0043E9C2) reads the clock through
 * GetLocalTime and nothing else; host.c's GetLocalTime shim asks
 * levels_seed() for the seed and hands time() the moment that many seconds
 * after 1970, and its GetTimeZoneInformation shim says UTC, so time()
 * returns exactly the seed. An unpinned game gets the real time, as before,
 * and that is recorded as its seed, so any game can be saved or shared.
 *
 * [levels] in hover.ini: `seed=random` or a number; [saved]: `1=level,seed`
 * and so on, up to MAX_SAVED.
 */
#define WIN32_LEAN_AND_MEAN
#include "hover_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "levels.h"
#include "net.h"
#include "mp.h"
#include "recomp_types.h"

#define G_LEVEL      0x0046049Cu   /* current level, 0-based */
#define G_START_AT   0x0046048Cu   /* Options > Start At, 1-based: a new game starts here */
#define MAX_SAVED 20

static char     g_ini[MAX_PATH];
static int      g_pinned;
static uint32_t g_pin, g_seed;
static int      g_level = -1;              /* the last real level played, 0-based */
static int      g_demo;                    /* the attract-mode demo (table entry 20) is up */
static int      g_restore_start;           /* a saved level borrowed Start At: put it back */
static uint32_t g_start_was;
static HWND     g_frame;
static void   (*g_title)(const char*);
static struct { int level; uint32_t seed; } g_saved[MAX_SAVED];
static int      g_nsaved;

/* ------------------------------------------------------------ settings */

void levels_load(void) {
    char b[32];
    if (!GetPrivateProfileStringA("levels", "seed", "", b, sizeof b, g_ini)) {
        WritePrivateProfileStringA("levels", "seed", "random", g_ini);
        strcpy(b, "random");
    }
    g_pinned = strcmp(b, "random") != 0;
    g_pin = (uint32_t)strtoul(b, NULL, 10);
    g_nsaved = 0;
    for (int i = 1; i <= MAX_SAVED; i++) {
        char k[8];
        _snprintf(k, sizeof k, "%d", i);
        if (!GetPrivateProfileStringA("saved", k, "", b, sizeof b, g_ini)) break;
        if (sscanf(b, "%d,%u", &g_saved[g_nsaved].level, &g_saved[g_nsaved].seed) == 2) g_nsaved++;
    }
}

static void save_list(void) {
    WritePrivateProfileStringA("saved", NULL, NULL, g_ini);
    for (int i = 0; i < g_nsaved; i++) {
        char k[8], v[32];
        _snprintf(k, sizeof k, "%d", i + 1);
        _snprintf(v, sizeof v, "%d,%u", g_saved[i].level, g_saved[i].seed);
        WritePrivateProfileStringA("saved", k, v, g_ini);
    }
}

static void pin(int on, uint32_t seed) {
    char b[16];
    _snprintf(b, sizeof b, "%u", seed);
    WritePrivateProfileStringA("levels", "seed", on ? b : "random", g_ini);
    levels_load();
}

void levels_init(const char* ini, uint32_t cli_seed, int cli_pinned) {
    strncpy(g_ini, ini, sizeof g_ini - 1);
    levels_load();
    if (cli_pinned) {                          /* --seed: this run only, the ini untouched */
        g_pinned = 1;
        g_pin = cli_seed;
    }
}

void levels_attach(HWND frame, void (*set_title)(const char*)) {
    g_frame = frame;
    g_title = set_title;
}

/* ------------------------------------------------------------ the seed */

static void show(void) {
    char t[160];
    if (g_level < 0 || g_demo) _snprintf(t, sizeof t, "Hover!");
    else _snprintf(t, sizeof t, "Hover!  -  Level %d  -  Seed %u%s", g_level + 1, g_seed,
                   g_pinned ? " (pinned)" : "");
    if (g_title) g_title(t);
}

/* Called from GetLocalTime, which only time() calls: the level loader's
 * srand(time(NULL)) (and one other srand site, 0x00433EAE). */
uint32_t levels_seed(void) {
    g_seed = g_pinned ? g_pin : (uint32_t)time(NULL);
    int level = (int)MEM32(G_LEVEL);
    g_demo = level == 20;                      /* small.maz, the attract loop: not a level to keep */
    net_level_loading(g_demo);
    mp_level_reset();                          /* the radar's seen marks start over */
    if (level >= 0 && level < 20) g_level = level;
    if (g_restore_start) {                     /* the saved level is loading: Start At back */
        MEM32(G_START_AT) = g_start_was;
        g_restore_start = 0;
    }
    if (g_demo) fprintf(stderr, "[level] demo, seed %u\n", g_seed);
    else fprintf(stderr, "[level] level %d, seed %u%s\n", g_level + 1, g_seed, g_pinned ? " (pinned)" : "");
    show();
    return g_seed;
}

/* The share code: level and seed, easy to read out loud. */
static void code_of(int level, uint32_t seed, char* out, size_t n) {
    _snprintf(out, n, "L%d-%u", level + 1, seed);
}

static int parse_code(const char* s, int* level, uint32_t* seed) {
    int l;
    unsigned v;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (sscanf(s, "L%d-%u", &l, &v) != 2 && sscanf(s, "l%d-%u", &l, &v) != 2) return 0;
    if (l < 1 || l > 20) return 0;
    *level = l - 1;
    *seed = v;
    return 1;
}

/* Start a new game at `level` with `seed` pinned: borrow Start At for the
 * one new game (levels_seed puts it back as the level loads) and press F2. */
static void start(int level, uint32_t seed) {
    if (!g_restore_start) g_start_was = MEM32(G_START_AT);
    MEM32(G_START_AT) = (uint32_t)level + 1;
    g_restore_start = 1;
    PostMessageA(g_frame, WM_KEYDOWN, VK_F2, 1);
    PostMessageA(g_frame, WM_KEYUP, VK_F2, 0xC0000001u);
    fprintf(stderr, "[level] new game: level %d, seed %u\n", level + 1, seed);
}

static void play(int level, uint32_t seed) {
    pin(1, seed);
    start(level, seed);
}

/* Online (net.c): every PC plays the host's level on the host's seed, for
 * this session only (hover.ini is left alone). */
void levels_session(int level, uint32_t seed) {
    g_pinned = 1;
    g_pin = seed;
    start(level, seed);
}

uint32_t levels_current_seed(void) { return g_pinned ? g_pin : (uint32_t)time(NULL); }

/* ------------------------------------------------------------ menu */

enum { ID_NOW = 0x6D00, ID_PIN, ID_RANDOM, ID_SAVE, ID_COPY, ID_PASTE, ID_CLEAR,
       ID_SAVED = 0x6D20 };   /* + 0..MAX_SAVED-1 */
static HMENU g_menu, g_saved_menu;

void levels_menu(HMENU recomp) {
    g_menu = CreatePopupMenu();
    g_saved_menu = CreatePopupMenu();
    AppendMenuA(g_menu, MF_STRING | MF_GRAYED, ID_NOW, "No level yet");
    AppendMenuA(g_menu, MF_STRING, ID_PIN, "&Pin this seed (replay this layout)");
    AppendMenuA(g_menu, MF_STRING, ID_RANDOM, "&Random seed every level");
    AppendMenuA(g_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_menu, MF_STRING, ID_SAVE, "&Save this level");
    AppendMenuA(g_menu, MF_POPUP, (UINT_PTR)g_saved_menu, "Saved &levels");
    AppendMenuA(g_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_menu, MF_STRING, ID_COPY, "&Copy share code");
    AppendMenuA(g_menu, MF_STRING, ID_PASTE, "Play share code from clip&board");
    AppendMenuA(recomp, MF_POPUP, (UINT_PTR)g_menu, "&Level");
}

void levels_update_menu(HMENU popup) {
    char t[96], code[32];
    if (!g_menu) return;
    if (popup == g_saved_menu) {               /* rebuilt each time it opens */
        while (GetMenuItemCount(g_saved_menu) > 0) DeleteMenu(g_saved_menu, 0, MF_BYPOSITION);
        for (int i = 0; i < g_nsaved; i++) {
            code_of(g_saved[i].level, g_saved[i].seed, code, sizeof code);
            _snprintf(t, sizeof t, "&%c  Level %d, seed %u   (%s)", i < 9 ? '1' + i : 'A' + i - 9,
                      g_saved[i].level + 1, g_saved[i].seed, code);
            AppendMenuA(g_saved_menu, MF_STRING, ID_SAVED + i, t);
        }
        if (!g_nsaved) AppendMenuA(g_saved_menu, MF_STRING | MF_GRAYED, ID_SAVED, "(none yet: Save this level)");
        else {
            AppendMenuA(g_saved_menu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(g_saved_menu, MF_STRING, ID_CLEAR, "C&lear the list");
        }
        return;
    }
    if (g_level < 0) _snprintf(t, sizeof t, "No level yet%s", g_pinned ? " (seed pinned)" : "");
    else {
        code_of(g_level, g_seed, code, sizeof code);
        _snprintf(t, sizeof t, "Level %d, seed %u  (%s)", g_level + 1, g_seed, code);
    }
    ModifyMenuA(g_menu, ID_NOW, MF_BYCOMMAND | MF_STRING | MF_GRAYED, ID_NOW, t);
    UINT on = g_level >= 0 ? MF_ENABLED : MF_GRAYED;
    EnableMenuItem(popup, ID_PIN, MF_BYCOMMAND | on);
    EnableMenuItem(popup, ID_SAVE, MF_BYCOMMAND | on);
    EnableMenuItem(popup, ID_COPY, MF_BYCOMMAND | on);
    EnableMenuItem(popup, ID_RANDOM, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(popup, ID_PASTE, MF_BYCOMMAND | MF_ENABLED);
    CheckMenuItem(popup, ID_PIN, MF_BYCOMMAND | (g_pinned ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(popup, ID_RANDOM, MF_BYCOMMAND | (g_pinned ? MF_UNCHECKED : MF_CHECKED));
}

static void clip_put(const char* s) {
    size_t n = strlen(s) + 1;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!g) return;
    memcpy(GlobalLock(g), s, n);
    GlobalUnlock(g);
    if (OpenClipboard(0)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_TEXT, g)) GlobalFree(g);
        CloseClipboard();
    } else GlobalFree(g);
}

static int clip_get(char* out, size_t n) {
    int ok = 0;
    if (!OpenClipboard(0)) return 0;
    HANDLE h = GetClipboardData(CF_TEXT);
    const char* p = h ? (const char*)GlobalLock(h) : NULL;
    if (p) {
        strncpy(out, p, n - 1);
        out[n - 1] = 0;
        GlobalUnlock(h);
        ok = 1;
    }
    CloseClipboard();
    return ok;
}

int levels_command(UINT id) {
    char code[64];
    if (id == ID_PIN) pin(!g_pinned || g_pin != g_seed ? 1 : 0, g_seed);
    else if (id == ID_RANDOM) pin(0, 0);
    else if (id == ID_SAVE) {
        for (int i = 0; i < g_nsaved; i++)
            if (g_saved[i].level == g_level && g_saved[i].seed == g_seed) return 1;
        if (g_nsaved == MAX_SAVED) memmove(g_saved, g_saved + 1, sizeof g_saved[0] * --g_nsaved);
        g_saved[g_nsaved].level = g_level;
        g_saved[g_nsaved++].seed = g_seed;
        save_list();
    }
    else if (id == ID_CLEAR) { g_nsaved = 0; save_list(); }
    else if (id >= ID_SAVED && id < ID_SAVED + MAX_SAVED && (int)(id - ID_SAVED) < g_nsaved)
        play(g_saved[id - ID_SAVED].level, g_saved[id - ID_SAVED].seed);
    else if (id == ID_COPY) { code_of(g_level, g_seed, code, sizeof code); clip_put(code); }
    else if (id == ID_PASTE) {
        int level;
        uint32_t seed;
        if (clip_get(code, sizeof code) && parse_code(code, &level, &seed)) play(level, seed);
        else MessageBoxA(g_frame, "The clipboard does not hold a Hover! share code.\n"
                                  "Share codes look like L3-1789123456 (level 3, seed 1789123456).",
                         "Hover!", MB_OK | MB_ICONINFORMATION);
    }
    else return 0;
    show();
    return 1;
}

/* --levels-selftest: the parts with no game in them. */
int levels_selftest(void) {
    int fails = 0, l;
    uint32_t s;
    char code[32];
    code_of(2, 1789123456u, code, sizeof code);
    if (strcmp(code, "L3-1789123456")) { fprintf(stderr, "[level] code %s\n", code); fails++; }
    if (!parse_code(" L3-1789123456\r\n", &l, &s) || l != 2 || s != 1789123456u) fails++;
    if (parse_code("L21-5", &l, &s) || parse_code("hello", &l, &s)) fails++;
    fprintf(stderr, "[level] selftest %s\n", fails ? "FAILED" : "OK");
    return fails;
}
