/*
 * pad_sdl.c - the Xbox-controller support of pad.c, on SDL2.
 *
 * The design is the same as on Windows (docs/controller.md): the pad is a
 * keyboard. Each action names the key the game has bound to it right now and
 * holds it through win32hle's key state, so the game's own polling sees it;
 * presses also post WM_KEYDOWN/UP for the menu keys (F2/F3). SDL's game
 * controller API supplies the same XInput-shaped state pad.c polls, so the
 * mapping code below is the same.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL.h>

#include "pad.h"
#include "net.h"
#include "recomp_types.h"
#include "win32hle.h"

#define KEY_FORWARD 0x004606ECu
#define KEY_REVERSE 0x004606F0u
#define KEY_LEFT    0x004606F4u
#define KEY_RIGHT   0x004606F8u
#define KEY_JUMP    0x004606E8u
#define KEY_WALL    0x004606E4u
#define KEY_CLOAK   0x004606E0u

enum { IN_LT = 1u << 16, IN_RT = 1u << 17,
       IN_LUP = 1u << 18, IN_LDOWN = 1u << 19, IN_LLEFT = 1u << 20, IN_LRIGHT = 1u << 21,
       IN_RUP = 1u << 22, IN_RDOWN = 1u << 23, IN_RLEFT = 1u << 24, IN_RRIGHT = 1u << 25 };

static const struct { const char *name; uint32_t bit; } g_inputs[] = {
    {"DPAD_UP", XINPUT_GAMEPAD_DPAD_UP}, {"DPAD_DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
    {"DPAD_LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"DPAD_RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
    {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK},
    {"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {"RS", XINPUT_GAMEPAD_RIGHT_THUMB},
    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
    {"LT", IN_LT}, {"RT", IN_RT},
    {"LSTICK_UP", IN_LUP}, {"LSTICK_DOWN", IN_LDOWN}, {"LSTICK_LEFT", IN_LLEFT}, {"LSTICK_RIGHT", IN_LRIGHT},
    {"RSTICK_UP", IN_RUP}, {"RSTICK_DOWN", IN_RDOWN}, {"RSTICK_LEFT", IN_RLEFT}, {"RSTICK_RIGHT", IN_RRIGHT},
};

static struct {
    const char *name; const char *dflt; uint32_t table; int fixed_vk; uint32_t mask;
} g_act[] = {
    {"forward",  "RT,LSTICK_UP,DPAD_UP",     KEY_FORWARD, 0,      0},
    {"reverse",  "LT,LSTICK_DOWN,DPAD_DOWN", KEY_REVERSE, 0,      0},
    {"left",     "LSTICK_LEFT,DPAD_LEFT",    KEY_LEFT,    0,      0},
    {"right",    "LSTICK_RIGHT,DPAD_RIGHT",  KEY_RIGHT,   0,      0},
    {"jump",     "A",                        KEY_JUMP,    0,      0},
    {"wall",     "X",                        KEY_WALL,    0,      0},
    {"cloak",    "B",                        KEY_CLOAK,   0,      0},
    {"pause",    "START",                    0,           VK_F3,  0},
    {"new_game", "BACK",                     0,           VK_F2,  0},
};
#define NACT ((int)(sizeof g_act / sizeof g_act[0]))

static char  g_ini[MAX_PATH];
static int   g_enabled = 1, g_slot, g_deadzone = 30, g_trigger = 20;
static volatile LONG g_held[256];
static volatile int  g_connected = -1;
static int   g_first_only;
static SDL_GameController *g_pad[4];

static uint32_t parse_inputs(const char *s) {
    uint32_t m = 0;
    char buf[256], *tok, *save = NULL;
    snprintf(buf, sizeof buf, "%s", s ? s : "");
    for (tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
        int found = 0;
        for (int i = 0; i < (int)(sizeof g_inputs / sizeof g_inputs[0]); i++)
            if (!strcasecmp(tok, g_inputs[i].name)) { m |= g_inputs[i].bit; found = 1; }
        if (!found) fprintf(stderr, "[pad] hover.ini: unknown input \"%s\"\n", tok);
    }
    return m;
}

static void put_int(const char *key, int v) {
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    WritePrivateProfileStringA("pad", key, b, g_ini);
}

static void load(void) {
    char b[256];
    if (!GetPrivateProfileStringA("pad", "enabled", "", b, sizeof b, g_ini)) {
        WritePrivateProfileStringA("pad", NULL, NULL, g_ini);
        put_int("enabled", 1);
        put_int("controller", 0);
        put_int("deadzone", 30);
        put_int("trigger", 20);
    }
    g_enabled  = (int)GetPrivateProfileIntA("pad", "enabled", 1, g_ini);
    g_slot     = (int)GetPrivateProfileIntA("pad", "controller", 0, g_ini);
    g_deadzone = (int)GetPrivateProfileIntA("pad", "deadzone", 30, g_ini);
    g_trigger  = (int)GetPrivateProfileIntA("pad", "trigger", 20, g_ini);
    if (g_slot < 0 || g_slot > 4) g_slot = 0;
    if (g_deadzone < 1 || g_deadzone > 95) g_deadzone = 30;
    if (g_trigger < 1 || g_trigger > 95) g_trigger = 20;
    for (int i = 0; i < NACT; i++) {
        if (!GetPrivateProfileStringA("pad", g_act[i].name, "", b, sizeof b, g_ini)) {
            WritePrivateProfileStringA("pad", g_act[i].name, g_act[i].dflt, g_ini);
            snprintf(b, sizeof b, "%s", g_act[i].dflt);
        }
        g_act[i].mask = parse_inputs(b);
    }
}

/* ------------------------------------------------------------ SDL polling */

static void fill_pad(SDL_GameController *c, XINPUT_GAMEPAD *g) {
    memset(g, 0, sizeof *g);
#define BTN(bit, sdlenum) do { if (SDL_GameControllerGetButton(c, sdlenum)) g->wButtons |= (bit); } while (0)
    BTN(XINPUT_GAMEPAD_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_UP);
    BTN(XINPUT_GAMEPAD_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    BTN(XINPUT_GAMEPAD_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    BTN(XINPUT_GAMEPAD_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    BTN(XINPUT_GAMEPAD_START, SDL_CONTROLLER_BUTTON_START);
    BTN(XINPUT_GAMEPAD_BACK, SDL_CONTROLLER_BUTTON_BACK);
    BTN(XINPUT_GAMEPAD_LEFT_THUMB, SDL_CONTROLLER_BUTTON_LEFTSTICK);
    BTN(XINPUT_GAMEPAD_RIGHT_THUMB, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    BTN(XINPUT_GAMEPAD_LEFT_SHOULDER, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    BTN(XINPUT_GAMEPAD_RIGHT_SHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    BTN(XINPUT_GAMEPAD_A, SDL_CONTROLLER_BUTTON_A);
    BTN(XINPUT_GAMEPAD_B, SDL_CONTROLLER_BUTTON_B);
    BTN(XINPUT_GAMEPAD_X, SDL_CONTROLLER_BUTTON_X);
    BTN(XINPUT_GAMEPAD_Y, SDL_CONTROLLER_BUTTON_Y);
#undef BTN
    g->bLeftTrigger  = (BYTE)((SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) * 255 + 16383) / 32767);
    g->bRightTrigger = (BYTE)((SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) * 255 + 16383) / 32767);
    g->sThumbLX = (SHORT)SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    g->sThumbRX = (SHORT)SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    int ly = -SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);    /* SDL +Y is down, XInput +Y up */
    int ry = -SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);
    g->sThumbLY = (SHORT)(ly > 32767 ? 32767 : ly);
    g->sThumbRY = (SHORT)(ry > 32767 ? 32767 : ry);
}

static uint32_t read_inputs(const XINPUT_GAMEPAD *g) {
    uint32_t m = g->wButtons;
    int dz = 32767 * g_deadzone / 100, tr = 255 * g_trigger / 100;
    if (g->bLeftTrigger > tr) m |= IN_LT;
    if (g->bRightTrigger > tr) m |= IN_RT;
    if (g->sThumbLY >  dz) m |= IN_LUP;
    if (g->sThumbLY < -dz) m |= IN_LDOWN;
    if (g->sThumbLX < -dz) m |= IN_LLEFT;
    if (g->sThumbLX >  dz) m |= IN_LRIGHT;
    if (g->sThumbRY >  dz) m |= IN_RUP;
    if (g->sThumbRY < -dz) m |= IN_RDOWN;
    if (g->sThumbRX < -dz) m |= IN_RLEFT;
    if (g->sThumbRX >  dz) m |= IN_RRIGHT;
    return m;
}

static void set_key(int vk, int down) {
    if (vk <= 0 || vk > 255) return;
    if (down) __atomic_add_fetch(&g_held[vk], 1, __ATOMIC_SEQ_CST);
    else      __atomic_sub_fetch(&g_held[vk], 1, __ATOMIC_SEQ_CST);
    hle_input_key_state((uint32_t)vk, down);
    if (!net_active()) {
        uint32_t w = hle_focus_window();
        if (!w) w = hle_first_hwnd();
        if (w) PostMessageA((HWND)w, down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)vk,
                            down ? 1u : 0xC0000001u);
    }
}

static int g_was[NACT], g_vk_was[NACT];

static void pad_step(const XINPUT_GAMEPAD *g) {
    uint32_t in = g ? read_inputs(g) : 0;
    for (int i = 0; i < NACT; i++) {
        int on = (in & g_act[i].mask) != 0;
        int vk = g_act[i].table ? (int)(MEM32(g_act[i].table) & 0xFF) : g_act[i].fixed_vk;
        if (!g_act[i].mask) on = 0;
        if (on && g_was[i] && vk != g_vk_was[i]) {
            set_key(g_vk_was[i], 0);
            g_was[i] = 0;
        }
        if (on != g_was[i]) {
            set_key(on ? vk : g_vk_was[i], on);
            g_vk_was[i] = vk;
            g_was[i] = on;
        }
    }
}

static void pad_open_all(void) {
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n && i < 4; i++) {
        if (g_pad[i] && !SDL_GameControllerGetAttached(g_pad[i])) {
            SDL_GameControllerClose(g_pad[i]);
            g_pad[i] = NULL;
        }
        if (!g_pad[i] && SDL_IsGameController(i)) g_pad[i] = SDL_GameControllerOpen(i);
    }
}

static SDL_GameController *pick(int slot0) {
    if (slot0 >= 0 && slot0 < 4) return g_pad[slot0] && SDL_GameControllerGetAttached(g_pad[slot0]) ? g_pad[slot0] : NULL;
    for (int i = 0; i < 4; i++) if (g_pad[i] && SDL_GameControllerGetAttached(g_pad[i])) return g_pad[i];
    return NULL;
}

static int poll_thread(void *unused) {
    int logged = -2;
    unsigned tick = 0;
    (void)unused;
    for (;;) {
        SDL_Delay(8);
        if (++tick % 125 == 0) pad_open_all();       /* a hotplug, once a second */
        SDL_GameControllerUpdate();
        int slot = g_first_only ? 0 : g_slot ? g_slot - 1 : -1;
        SDL_GameController *c = g_enabled ? pick(slot) : NULL;
        int user = c ? (slot >= 0 ? slot : 0) : -1;
        if (user != logged) {
            if (user >= 0) fprintf(stderr, "[pad] controller %d connected\n", user + 1);
            else if (logged >= 0) fprintf(stderr, "[pad] controller disconnected\n");
            logged = user;
        }
        g_connected = user;
        if (c) { XINPUT_GAMEPAD g; fill_pad(c, &g); pad_step(&g); }
        else pad_step(NULL);
    }
    return 0;
}

int pad_held(int vk) { return __atomic_load_n(&g_held[vk & 0xFF], __ATOMIC_SEQ_CST) > 0; }

int pad_read(int user, XINPUT_GAMEPAD *out) {
    if (!g_enabled) return 0;
    pad_open_all();
    SDL_GameController *c = pick(user & 3);
    if (!c) return 0;
    fill_pad(c, out);
    return 1;
}

void pad_first_only(int on) { g_first_only = on; }

static int g_started;

void pad_start(const char *ini, HWND wnd) {
    (void)wnd;
    snprintf(g_ini, sizeof g_ini, "%s", ini);
    load();
    if (g_started++) return;
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        fprintf(stderr, "[pad] no game controller support: %s\n", SDL_GetError());
    pad_open_all();
    fprintf(stderr, "[pad] %s, settings in %s\n", g_enabled ? "on" : "off", g_ini);
    SDL_Thread *t = SDL_CreateThread(poll_thread, "hover-pad", NULL);
    if (t) SDL_DetachThread(t);
}

/* ------------------------------------------------------------ menu (none) */

void pad_menu(HMENU recomp) {
    (void)recomp;
}
void pad_update_menu(HMENU popup) {
    (void)popup;
}
int pad_command(UINT id) {
    (void)id;
    return 0;
}
void pad_reload(void) { load(); }

/* ------------------------------------------------------------ selftest */

int pad_selftest(void) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    strcat(tmp, "hover-pad-selftest.ini");
    DeleteFileA(tmp);
    snprintf(g_ini, sizeof g_ini, "%s", tmp);
    load();
    MEM32(KEY_FORWARD) = VK_UP;
    MEM32(KEY_LEFT) = VK_LEFT;
    MEM32(KEY_JUMP) = 'A';
    XINPUT_GAMEPAD g = { 0 };
    int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "[pad] selftest FAILED: %s\n", #c); fails++; } } while (0)
    g.bRightTrigger = 200;
    g.sThumbLX = -20000;
    pad_step(&g);
    CHECK(pad_held(VK_UP) && pad_held(VK_LEFT) && !pad_held(VK_RIGHT));
    g.sThumbLX = -5000;
    pad_step(&g);
    CHECK(pad_held(VK_UP) && !pad_held(VK_LEFT));
    MEM32(KEY_FORWARD) = 'W';
    pad_step(&g);
    CHECK(pad_held('W') && !pad_held(VK_UP));
    g.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START;
    pad_step(&g);
    CHECK(pad_held('A') && pad_held(VK_F3));
    pad_step(NULL);
    CHECK(!pad_held('W') && !pad_held('A') && !pad_held(VK_F3));
    CHECK(parse_inputs("rt, DPAD_UP") == (IN_RT | XINPUT_GAMEPAD_DPAD_UP));
    WritePrivateProfileStringA("pad", "jump", "Y,LB", g_ini);
    load();
    CHECK(g_act[4].mask == (XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_LEFT_SHOULDER));
    CHECK(GetPrivateProfileIntA("pad", "deadzone", 0, g_ini) == 30);
#undef CHECK
    DeleteFileA(tmp);
    fprintf(stderr, "[pad] selftest %s\n", fails ? "FAILED" : "OK");
    return fails;
}
