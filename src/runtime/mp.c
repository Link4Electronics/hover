/*
 * Multiplayer seats and views: split screen on one PC, and the local half of
 * online play (net.c). docs/multiplayer.md has the research this rests on.
 *
 * Hover! has one human craft (CHumanPlayer, inside the document at
 * [0x00460970]+0x818C) and robots (CRobotPlayer). Humans and robots drive
 * through the same physics: each tick every craft's Think writes a turn
 * (+0x1F0) and a thrust (+0x1F4), 16.16 fixed point in -1.0..+1.0, and
 * CPlayer::Think (0x00409110) integrates them. So:
 *
 *   seats   seat 0 is the game's own human; seats 1..15 each take over a
 *           robot of their own (every level gets one extra flag-runner per
 *           seat, and the seats are the last robots created). The robots'
 *           Think (0x00409300) is only ever reached through a virtual call,
 *           so recomp_lookup_manual hands it to seat_think here, which writes
 *           the seat's controls into turn/thrust and runs the physics.
 *   views   this PC shows its own seats: the frame draw (0x00402250) runs
 *           once per local seat on the render thread, with the camera reads
 *           patched at lift time (run_lift.py) to ask hover_cam_obj() whose
 *           eyes to use, and so are the dashboard's reads of the human (radar,
 *           speed, height); the numbers the view keeps are swapped for each
 *           pass (hover_render_views). host.c puts each pass's blits in its
 *           cell of the capture buffer.
 *
 * Split screen: every seat is local. Online (net.c): the seats are spread
 * over the PCs, and every seat's controls come from the tick's input record
 * instead of a device, so every PC runs the same world.
 *
 * Players in robot seats play for the robots' team (the flag rules go by
 * class), with their own powerups, radar and sounds.
 */
#define WIN32_LEAN_AND_MEAN
#include "hover_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp.h"
#include "net.h"
#include "pad.h"
#include "recomp_types.h"

/* Lifted functions this calls (the generated recomp_funcs.h declares them
 * too; CI compiles this file without the generated tree). */
void sub_00402250(void);   /* the frame draw */
void sub_00409110(void);   /* CPlayer::Think: physics */
void sub_00409300(void);   /* CRobotPlayer::Think: AI, then physics */
void sub_0040A4C0(void);   /* CRobotPlayer slot 15: sprite frame, then CPlayer's */
void sub_0040A590(void);   /* CRobotPlayer slot 16: sprite create/move, then CPlayer's */
void sub_004089D0(void);   /* CHumanPlayer slot 15 */
void sub_004089F0(void);   /* CHumanPlayer slot 16 */
void sub_00414470(void);   /* CFlag slot 11: may this object take the flag? */
void sub_0040CB40(void);   /* ...the distance test it ends with */
void sub_00405B90(void);   /* renderer: is this sprite in view? */
void sub_004386D0(void);   /* angle of (dy, dx) */

#define G_DOC         0x00460970u
#define DOC_HUMAN     0x818Cu
#define DOC_THINKERS  0x83ACu            /* CObList of everything that thinks */
#define VT_ROBOT      0x004BCCE0u
#define VT_HUMAN      0x004BCBE0u
#define SPRITE_FRAMES 0x004A4620u        /* the craft's 32 views, by angle */
#define G_GAME_STATE  0x00460728u        /* 3 = playing */
#define KEY_FORWARD   0x004606ECu        /* the game's key table (docs/controller.md) */
#define KEY_REVERSE   0x004606F0u
#define KEY_LEFT      0x004606F4u
#define KEY_RIGHT     0x004606F8u
#define KEY_FWD_LEFT  0x004606FCu
#define KEY_FWD_RIGHT 0x00460700u
#define KEY_REV_LEFT  0x00460704u
#define KEY_REV_RIGHT 0x00460708u
#define KEY_JUMP      0x004606E8u
#define KEY_WALL      0x004606E4u
#define KEY_CLOAK     0x004606E0u

static char g_ini[MAX_PATH];
static volatile int g_seats = 1;         /* seats in the game, every PC */
static int g_local[MP_MAX_LOCAL] = { 0 };/* this PC's seats, in view order */
static volatile int g_nlocal = 1;
static volatile LONG g_layout;           /* bumped when the view layout changes */
static volatile int g_view;              /* the render pass now running */
static volatile int g_in_views;          /* inside hover_render_views... */
static volatile DWORD g_views_thread;    /* ...on this thread (the render thread) */
static int (*g_key)(int vk);

/* ------------------------------------------------------------ seats */

/* Every level's table entry gets one more flag-runner per robot seat (level
 * table 0x004C4000, 0x58 a level: +0x28 hunters, +0x2C flag-runners), so the
 * level keeps the AI robots it was designed with. The loader creates
 * hunters, then runners, so the seats are the last robots in the list. The
 * table is read at each level load, so a change applies from the next one. */
#define LEVEL_TABLE   0x004C4000u
#define LEVEL_SIZE    0x58u
#define LEVELS        20                 /* entry 20 is the attract demo: no robots */
static uint32_t g_runners[LEVELS];      /* the shipped counts, before any seats */
static int g_table_saved;

void mp_apply_table(void) {
    for (int l = 0; l < LEVELS; l++) {
        uint32_t at = LEVEL_TABLE + l * LEVEL_SIZE + 0x2C;
        if (!g_table_saved) g_runners[l] = MEM32(at);
        MEM32(at) = g_runners[l] + (uint32_t)(g_seats - 1);
    }
    g_table_saved = 1;
}

/* The robot in seat `s` (s >= 1), or 0: none this level, or no level. */
static uint32_t seat_craft(int s) {
    uint32_t doc = MEM32(G_DOC), robots[64];
    int n = 0;
    if (!doc || s < 1) return 0;
    for (uint32_t node = MEM32(doc + DOC_THINKERS); node && n < 64; node = MEM32(node)) {
        uint32_t o = MEM32(node + 8);
        if (MEM32(o) == VT_ROBOT) robots[n++] = o;
    }
    int i = n - (g_seats - 1) + (s - 1);
    return i >= 0 && i < n ? robots[i] : 0;
}

/* The render thread must not walk the thinkers list: the game's thread adds
 * and frees nodes in it as pod effects start and end, and a walk from the
 * other thread read freed memory (a rare fault, single player included).
 * So at the start of every tick, on the game's thread (CHumanPlayer::Think,
 * the first in the list, is wrapped in mp_lookup), the host copies what the
 * render passes need: every craft, its sprite, its seat. Render-thread code
 * reads only this. */
typedef struct { uint32_t craft, sprite; int seat; } snap_t;
static snap_t g_snap[64];
static int g_nsnap;
static CRITICAL_SECTION g_snap_lock;

static void snapshot(void) {
    snap_t t[64];
    int n = 0;
    uint32_t doc = MEM32(G_DOC);
    for (uint32_t node = doc ? MEM32(doc + DOC_THINKERS) : 0; node && n < 64; node = MEM32(node)) {
        uint32_t o = MEM32(node + 8);
        if (MEM32(o) != VT_ROBOT && MEM32(o) != VT_HUMAN) continue;
        t[n].craft = o;
        t[n].sprite = MEM32(o + 0x78);
        t[n].seat = MEM32(o) == VT_HUMAN ? 0 : -1;
        n++;
    }
    for (int s = 1; s < g_seats; s++) {
        uint32_t c = seat_craft(s);
        for (int i = 0; i < n; i++) if (t[i].craft == c) t[i].seat = s;
    }
    EnterCriticalSection(&g_snap_lock);
    memcpy(g_snap, t, sizeof(snap_t) * n);
    g_nsnap = n;
    LeaveCriticalSection(&g_snap_lock);
}

/* The render thread's seat_craft. */
static uint32_t snap_seat_craft(int seat) {
    uint32_t c = 0;
    EnterCriticalSection(&g_snap_lock);
    for (int i = 0; i < g_nsnap && !c; i++) if (g_snap[i].seat == seat && seat > 0) c = g_snap[i].craft;
    LeaveCriticalSection(&g_snap_lock);
    return c;
}

static int snap_copy(snap_t* out) {
    EnterCriticalSection(&g_snap_lock);
    int n = g_nsnap;
    memcpy(out, g_snap, sizeof(snap_t) * n);
    LeaveCriticalSection(&g_snap_lock);
    return n;
}

void sub_00409180(void);   /* CHumanPlayer::Think */
static void human_think(void) { snapshot(); sub_00409180(); }

/* The seat whose craft this is (1..), or 0: AI robots and the human. */
int mp_is_seat(uint32_t craft) {
    for (int s = 1; s < g_seats && craft; s++)
        if (seat_craft(s) == craft) return s;
    return 0;
}

/* Sounds: the game plays a craft's sounds (pickups, wall hits, skids,
 * speed and invincibility) only when its +0x1F8 is set, which only the
 * human's is (the gates are patched in run_lift.py). With seats: the crafts
 * of the seats on this PC, the human included only if it is one of them, so
 * online every PC hears its own players. Sound is a side effect: no tick
 * reads it. */
uint32_t hover_heard(uint32_t craft) {
    if (g_seats < 2) return MEM32(craft + 0x1F8);
    int seat = MEM32(craft) == VT_HUMAN ? 0 : mp_is_seat(craft);
    if (!seat && MEM32(craft) != VT_HUMAN) return 0;
    for (int i = 0; i < g_nlocal; i++) if (g_local[i] == seat) return 1;
    return 0;
}

/* Pod gauges (the view's +0x21C..+0x420, written by the pods' pickup, Use
 * and Think through 0x00401140; patched in run_lift.py): a seat's go to a
 * block of its own, so they never touch player 1's HUD.
 * ponytail: nothing draws these yet; the per-seat HUD reads them from here. */
static uint8_t g_seat_hud[MP_MAX_SEATS][0x600];
uint32_t hover_pod_hud(uint32_t view, uint32_t owner) {
    int s = mp_is_seat(owner);
    return s ? (uint32_t)(uintptr_t)g_seat_hud[s] : view;
}

/* Use the last pod in one of `craft`'s pod lists (CObList at +0xD0 walls,
 * +0xEC cloaks, +0x108 jumps: tail +8, count +0xC), as the human's key
 * handler 0x0040B540 does: the pod's Use (vtable slot 9) with the user. */
static void pod_use(uint32_t craft, uint32_t list) {
    if (!MEM32(craft + list + 0xC)) return;
    uint32_t pod = MEM32(MEM32(craft + list + 8) + 8);
    recomp_func_t use = recomp_lookup(MEM32(MEM32(pod) + 0x24));
    if (!use) return;
    PUSH32(g_esp, craft);
    PUSH32(g_esp, RECOMP_RETADDR);
    g_ecx = pod;
    use();                    /* thiscall, ret 4 */
}

/* A seat's jump/wall/cloak: one use per press, with the human's checks
 * (0x0040B540: jump only when +0x148 <= 0, on the ground (+0xA0 == +0xA4)
 * and not already jumping (+0xA8)). Presses come from the tick's inputs. */
static int g_prev_buttons[MP_MAX_SEATS];
static void seat_buttons(int seat, uint32_t craft, int b) {
    int press = b & ~g_prev_buttons[seat];
    g_prev_buttons[seat] = b;
    if (press & MP_WALL) pod_use(craft, 0xD0);
    if (press & MP_CLOAK) pod_use(craft, 0xEC);
    if ((press & MP_JUMP) && (int32_t)MEM32(craft + 0x148) <= 0 &&
        MEM32(craft + 0xA0) == MEM32(craft + 0xA4) && !MEM32(craft + 0xA8))
        pod_use(craft, 0x108);
}

static double clamp1(double v) { return v < -1 ? -1 : v > 1 ? 1 : v; }

/* This PC's player `li` (0 = the first local player) as turn, thrust and
 * buttons (MP_JUMP/WALL/CLOAK). Local player 1 has the keyboard (the game's
 * own keys: arrows, Home/PgUp/End/PgDn, A/S/D) and controller 1; player 2
 * controller 2 and I/J/K/L; players 3 and 4 controllers 3 and 4. */
void mp_local_input(int li, double* turn, double* thrust, int* buttons) {
    XINPUT_GAMEPAD g;
    double t = 0, f = 0;
    int b = 0;
    if (li > 0 && pad_read(li, &g)) {         /* player 1's pad already presses the game's keys */
        double dz = 0.25;
        double x = g.sThumbLX / 32767.0, y = g.sThumbLY / 32767.0;
        if (x > dz || x < -dz) t += (x - (x > 0 ? dz : -dz)) / (1 - dz);
        if (y > dz || y < -dz) f += (y - (y > 0 ? dz : -dz)) / (1 - dz);
        f += (g.bRightTrigger - g.bLeftTrigger) / 255.0;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) t -= 1;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) t += 1;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) f += 1;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) f -= 1;
        if (g.wButtons & XINPUT_GAMEPAD_A) b |= MP_JUMP;
        if (g.wButtons & XINPUT_GAMEPAD_X) b |= MP_WALL;
        if (g.wButtons & XINPUT_GAMEPAD_B) b |= MP_CLOAK;
    }
    if (li == 0 && g_key) {                   /* the game's keys, wherever they are bound */
        int fl = g_key(MEM32(KEY_FWD_LEFT)), fr = g_key(MEM32(KEY_FWD_RIGHT));
        int rl = g_key(MEM32(KEY_REV_LEFT)), rr = g_key(MEM32(KEY_REV_RIGHT));
        if (g_key(MEM32(KEY_LEFT)) || fl || rl) t -= 1;
        if (g_key(MEM32(KEY_RIGHT)) || fr || rr) t += 1;
        if (g_key(MEM32(KEY_FORWARD)) || fl || fr) f += 1;
        if (g_key(MEM32(KEY_REVERSE)) || rl || rr) f -= 1;
        if (g_key(MEM32(KEY_JUMP))) b |= MP_JUMP;
        if (g_key(MEM32(KEY_WALL))) b |= MP_WALL;
        if (g_key(MEM32(KEY_CLOAK))) b |= MP_CLOAK;
    }
    if (li == 1 && g_key) {
        if (g_key('J')) t -= 1;
        if (g_key('L')) t += 1;
        if (g_key('I')) f += 1;
        if (g_key('K')) f -= 1;
        if (g_key('U')) b |= MP_JUMP;     /* player 2's jump/wall/cloak: U, O, P */
        if (g_key('O')) b |= MP_WALL;
        if (g_key('P')) b |= MP_CLOAK;
    }
    *turn = clamp1(t);
    *thrust = clamp1(f);
    *buttons = b;
}

/* Replaces CRobotPlayer::Think (vtable slot 7, 0x00409300) for every robot:
 * a robot in a seat gets its player's controls, the rest their AI. The
 * spin-out and airborne handling is CRobotPlayer::Think's own (0x00409300:
 * +0x5C, +0x158, +0x15C), then the shared physics. */
static void seat_think(void) {
    uint32_t self = g_ecx;
    int seat = mp_is_seat(self);
    if (!seat || (net_active() && net_seat_ai(seat))) { sub_00409300(); return; }   /* nobody in it: AI */
    if (MEM32(self + 0x5C) == 0) {
        MEM32(self + 0x15C) = MEM32(self + 0x158) ? 0 : 0xFFFFFFFFu;
        if (MEM32(G_GAME_STATE) == 3) {
            double t = 0, f = 0;
            int b = 0;
            if (net_active()) net_seat_input(seat, &t, &f, &b);
            else
                for (int li = 0; li < g_nlocal; li++)
                    if (g_local[li] == seat) mp_local_input(li, &t, &f, &b);
            MEM32(self + 0x1F0) = (uint32_t)(int32_t)(t * 65536);
            MEM32(self + 0x1F4) = (uint32_t)(int32_t)(f * 65536);
            seat_buttons(seat, self, b);
        }
    }
    g_ecx = self;
    sub_00409110();          /* CPlayer::Think: the physics; its ret pops our return address */
}

/* ------------------------------------------------------------ teams */

/* The game has two sides and decides them by class: CFlag::CanTake
 * (0x00414470) gives robot flags (+0xCC = 1) only to a CHumanPlayer and
 * human flags (+0xCC = 0) only to a CRobotPlayer; the flag's own side then
 * decides the scoring (0x0041AE40: +0xCC = 1 adds to the human's score,
 * doc+0x83E4, and ends the level when the taker carries them all; 0 counts
 * doc+0x83E8 for the robots). So a seat's team is a side here: 0 the
 * human's, 1 the robots'. Seat 0 is always 0, AI robots always 1; players
 * in robot seats default to 1 ([mp] teams= / --teams, one letter a seat,
 * h or r: "hh" puts player 2 with player 1). */
static int g_team[MP_MAX_SEATS] = { 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

void mp_get_teams(char* out) {         /* MP_MAX_SEATS letters and a NUL */
    for (int s = 0; s < MP_MAX_SEATS; s++) out[s] = g_team[s] ? 'r' : 'h';
    out[MP_MAX_SEATS] = 0;
}

void mp_set_teams(const char* t) {
    for (int s = 1; s < MP_MAX_SEATS; s++) g_team[s] = 1;
    for (int s = 1; t && s < MP_MAX_SEATS && s < (int)strlen(t); s++) g_team[s] = t[s] == 'h' || t[s] == 'H' ? 0 : 1;
    fprintf(stderr, "[mp] teams %s\n", t ? t : "");
}


/* 0 the human's side, 1 the robots', -1 not a craft. */
static int craft_team(uint32_t o) {
    uint32_t doc = MEM32(G_DOC);
    if (doc && o == doc + DOC_HUMAN) return 0;
    if (MEM32(o) != VT_ROBOT) return -1;
    int s = mp_is_seat(o);
    return s ? g_team[s] : 1;
}

/* The flags a side carries, for the scoring's "all flags taken" test and
 * the flag gauge (0x0041B05C, 0x0041AFF6, patched at lift time): the
 * original reads the taker's own count (+0xC0), so two players on one side
 * would each need every flag. */
uint32_t hover_team_flags(uint32_t craft) {
    uint32_t doc = MEM32(G_DOC), n = 0;
    int t = craft_team(craft);
    if (!doc || t < 0 || g_seats < 2) return MEM32(craft + 0xC0);
    for (uint32_t node = MEM32(doc + DOC_THINKERS); node; node = MEM32(node)) {
        uint32_t o = MEM32(node + 8);
        if (craft_team(o) == t) n += MEM32(o + 0xC0);
    }
    return n;
}

/* Hunters hunt "the human": their sighting test (0x0042ED00) and the target
 * they then chase (0x0040A810) are doc+0x818C, patched at lift time to ask
 * this instead: the nearest craft on the human's side to the hunter
 * (`state`+0x88, the AI state's robot). Deterministic: the tick's state. */
uint32_t hover_quarry(uint32_t doc, uint32_t state) {
    uint32_t robot = MEM32(state + 0x88), best = doc + DOC_HUMAN;
    if (g_seats < 2 || !robot) return best;
    int64_t bd = -1;
    for (uint32_t node = MEM32(doc + DOC_THINKERS); node; node = MEM32(node)) {
        uint32_t o = MEM32(node + 8);
        if (craft_team(o) != 0) continue;
        int64_t dx = (int32_t)(MEM32(o + 0x44) - MEM32(robot + 0x44)) >> 16;
        int64_t dy = (int32_t)(MEM32(o + 0x48) - MEM32(robot + 0x48)) >> 16;
        if (bd < 0 || dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; best = o; }
    }
    return best;
}

/* Replaces CFlag::CanTake (vtable 0x004BC9F0 slot 11): a craft never takes
 * its own side's flag, and may take the other side's (the original's
 * distance test, 0x0040CB40, which ends the original too). thiscall, one
 * argument, ret 4: the stack is already what 0x0040CB40 expects. */
static void flag_can_take(void) {
    uint32_t flag = g_ecx, obj = MEM32(g_esp + 4);
    int t = craft_team(obj);
    if (t < 0) { sub_00414470(); return; }
    if ((int)MEM32(flag + 0xCC) == t) { g_eax = 0; g_esp += 8; return; }
    sub_0040CB40();
}

/* ------------------------------------------------------------ player 1's craft, seen */

/* The human has no sprite: its slots 15/16 (0x004089D0/0x004089F0) are
 * CPlayer's, where a robot's (0x0040A4C0/0x0040A590) also pick the sprite
 * frame and create and move a sprite in the world (+0x78, a 0x70-byte
 * object in doc+0x188's list). Both robot versions use only CPlayer fields
 * and end in the CPlayer version, so with more than one seat the human runs
 * them. The level teardown forgets the sprite with the world (run_lift.py,
 * 0x004149C4). */
static void human_frame(void) { if (g_seats > 1) sub_0040A4C0(); else sub_004089D0(); }
/* Slot 16 runs on the render thread (0x00406C60, the dirty list); before
 * the first slot 15 there is no frame to show. */
static void human_sprite(void) {
    if (g_seats > 1 && MEM32(g_ecx + 0x1E8)) sub_0040A590(); else sub_004089F0();
}

recomp_func_t mp_lookup(uint32_t va) {
    switch (va) {
    case 0x00409300u: return g_seats > 1 ? seat_think : NULL;
    case 0x00409180u: return g_seats > 1 ? human_think : NULL;
    case 0x004089D0u: return human_frame;
    case 0x004089F0u: return human_sprite;
    case 0x00414470u: return flag_can_take;
    }
    return NULL;
}

/* A hash of every craft's state, for net.c's desync check: position,
 * heading and controls of everything that thinks. */
uint32_t mp_world_hash(void) {
    uint32_t doc = MEM32(G_DOC), h = 2166136261u;
    if (!doc) return 0;
    int n = 0;
    for (uint32_t node = MEM32(doc + DOC_THINKERS); node && n < 256; node = MEM32(node), n++) {
        uint32_t o = MEM32(node + 8);
        /* Pods think too while their effect runs, and are 0x100 bytes: past
         * +0x84 (CGameObj) only a craft's fields are the object's own. */
        int craft = MEM32(o) == VT_ROBOT || MEM32(o) == VT_HUMAN;
        uint32_t v[6] = { MEM32(o), MEM32(o + 0x44), MEM32(o + 0x48), craft ? MEM32(o + 0x19C) : 0,
                          craft ? MEM32(o + 0x1F0) : 0, craft ? MEM32(o + 0x1F4) : 0 };
        for (int i = 0; i < 6; i++) h = (h ^ v[i]) * 16777619u;
    }
    return h ^ (uint32_t)n;
}

/* ------------------------------------------------------------ views */

/* The camera reads in 0x00402250, patched at lift time: `doc` is the
 * document, and the craft returned is the one whose eyes this pass uses. */
uint32_t hover_cam_obj(uint32_t doc) {
    int seat = g_view < g_nlocal ? g_local[g_view] : 0;
    uint32_t craft = seat ? snap_seat_craft(seat) : 0;
    return craft ? craft : doc + DOC_HUMAN;
}

/* The dashboard blit (0x00407430) composes the whole dashboard off screen
 * when the game's full-dashboard option is on; several views always do, so
 * every pass blits a whole dashboard of its own into its cell. */
uint32_t hover_hud_full(void) { return MEM32(0x004C4CA8u) || g_nlocal > 1; }

/* The dashboard's numbers that are not read from the craft live in the view,
 * written by the game as they change: the A/S/D pod counters (+0x240/+0x268/
 * +0x290, value at +8), the score (+0x2B8, +8), the flag rows (+0x430 the
 * player's flags, +0x480 the robots', value at +0x44 of at most +0x3C) and
 * the pod-effect gauges (value at +0x24 of 0x310/0x340/0x370/0x3D0/0x400;
 * the speed and height gauges are worked out each draw). Counters and rows
 * are drawn only while their dirty word (+0) is set, into the one off-screen
 * dashboard every pass shares, so with several views each pass marks them
 * dirty; a robot seat's pass shows its own numbers: the pods its craft
 * holds (+0x114 jumps, +0xF8 cloaks, +0xDC walls: the counters are jump,
 * cloak, wall at +0x240/+0x268/+0x290), its pod-effect gauges from its own
 * block (hover_pod_hud), the robots' flags as its score, and the flags it
 * carries. */
enum { HUD_VALS = 10 };
static const uint16_t hud_val[HUD_VALS] = { 0x248, 0x270, 0x298, 0x2C0, 0x474,
                                            0x334, 0x364, 0x394, 0x3F4, 0x424 };
static const uint16_t hud_dirty[] = { 0x240, 0x268, 0x290, 0x2B8, 0x430, 0x480 };

static void hud_mark(uint32_t view) {
    for (int i = 0; i < (int)(sizeof hud_dirty / sizeof *hud_dirty); i++) MEM32(view + hud_dirty[i]) = 1;
}

/* Put `robot`'s numbers in the view; `was` keeps the game's for hud_put_back. */
static void hud_swap_in(uint32_t view, uint32_t robot, int seat, uint32_t* was, uint32_t* mine) {
    int32_t flags = (int32_t)MEM32(robot + 0xC0), most = (int32_t)MEM32(view + 0x46C);
    uint32_t block = (uint32_t)(uintptr_t)g_seat_hud[seat];   /* the pass knows its seat: no list walk here */
    for (int i = 0; i < HUD_VALS; i++) {
        was[i] = MEM32(view + hud_val[i]);
        mine[i] = i >= 5 ? MEM32(block + hud_val[i]) : 0;   /* the gauges: the seat's own */
    }
    mine[0] = MEM32(robot + 0x114);                   /* jumps */
    mine[1] = MEM32(robot + 0xF8);                    /* cloaks */
    mine[2] = MEM32(robot + 0xDC);                    /* walls */
    mine[3] = MEM32(MEM32(G_DOC) + 0x83E8);
    mine[4] = (uint32_t)(flags > most ? most : flags > 0 ? flags : 0);
    for (int i = 0; i < HUD_VALS; i++) MEM32(view + hud_val[i]) = mine[i];
}

/* The game's thread may have changed a number while the pass ran (the lock is
 * let go at every native call): keep a change, put back the rest.
 * ponytail: a change to exactly the robot's number is lost until the next one. */
static void hud_put_back(uint32_t view, const uint32_t* was, const uint32_t* mine) {
    for (int i = 0; i < HUD_VALS; i++)
        if (MEM32(view + hud_val[i]) == mine[i]) MEM32(view + hud_val[i]) = was[i];
}

/* Replaces the render thread's call to the frame draw (0x0041F012): the draw
 * once per local seat, the first local seat last, so what the game sees
 * afterwards (registers, its frame state) is exactly one normal draw.
 * 0x00402250 is thiscall with no stack arguments and a plain ret, so each
 * pass starts from the same registers and the same return-address slot. */
/* The sprite of the craft whose eyes this pass uses: its centre is the
 * camera, so it is not drawn (0x00406343, patched at lift time: the
 * renderer's visibility test, thiscall, one argument, ret 4). */
static volatile uint32_t g_own_sprite;

/* A craft's cloak: +0x134 is its cloak pod while one is held, active while
 * the pod's +0x28 is set (the robots' line-of-sight test, 0x0042ED2F, reads
 * the same). */
static int cloaked(uint32_t craft) {
    uint32_t c = MEM32(craft + 0x134);
    return c && MEM32(c + 0x28);
}

void hover_sprite_visible(void) {
    uint32_t sprite = MEM32(g_esp + 4);
    int hide = g_own_sprite && sprite == g_own_sprite;
    /* A cloaked craft is not drawn in anyone else's view (the original's
     * cloak only hid the human from the robots: nobody else ever saw it). */
    if (!hide && g_seats > 1) {
        snap_t crafts[64];
        int n = snap_copy(crafts);
        for (int k = 0; k < n && !hide; k++)
            if (crafts[k].sprite == sprite) hide = cloaked(crafts[k].craft);
    }
    if (hide) { g_eax = 0; g_esp += 8; return; }
    sub_00405B90();
}

/* ------------------------------------------------------------ the radar, per player */

/* What the radar shows is what has been seen: bit 0x08 of byte +0x24 of a
 * wall, and of an object's sprite, set by the renderer as it draws them
 * (0x00402671, 0x004029ED, 0x00402F4B) and by 0x0040E3F4 outside a draw, and
 * tested by the radar (0x00401C3A, 0x00401D47, 0x00404C05, 0x00404EFE). One
 * bit for everyone: with several players, every pass explored for all of
 * them. So the host keeps which seats have seen what, by address: a mark
 * made in a render pass is that pass's seat's, one made outside a pass is
 * everyone's, and the radar asks about the seat it is drawn for. The game's
 * own bit is still set and cleared as before; with one seat it is all
 * there is. Render-side only: no tick reads it, so it never desyncs. */
#define SEEN_SLOTS 16384                 /* a maze has ~700 walls, and the objects */
static struct { uint32_t key; uint16_t seats; } g_seen[SEEN_SLOTS];
static CRITICAL_SECTION g_seen_lock;
static volatile LONG g_seen_ready;

static void seen_init(void) {
    if (InterlockedCompareExchange(&g_seen_ready, 1, 0) == 0) InitializeCriticalSection(&g_seen_lock);
}

/* Callers hold g_seen_lock. */
static uint16_t* seen_at(uint32_t key, int add) {
    uint32_t i = (key >> 2) * 2654435761u % SEEN_SLOTS;
    for (int n = 0; n < SEEN_SLOTS; n++, i = (i + 1) % SEEN_SLOTS) {
        if (g_seen[i].key == key) return &g_seen[i].seats;
        if (!g_seen[i].key) {
            if (!add) return NULL;
            g_seen[i].key = key;
            return &g_seen[i].seats;
        }
    }
    return NULL;                         /* full: the radar shows less, nothing breaks */
}

static uint16_t pass_seats(void) {
    if (mp_in_views()) return (uint16_t)(1u << (g_view < g_nlocal ? g_local[g_view] : 0));
    return 0xFFFF;                       /* outside a draw: a reveal for everyone */
}

void hover_seen_mark(uint32_t obj) {
    if (g_seats < 2) return;
    seen_init();
    EnterCriticalSection(&g_seen_lock);
    uint16_t* m = seen_at(obj, 1);
    if (m) *m |= pass_seats();
    LeaveCriticalSection(&g_seen_lock);
}

uint32_t hover_seen_byte(uint32_t obj) {
    uint8_t b = MEM8(obj + 0x24);
    if (g_seats < 2 || !mp_in_views()) return b;
    seen_init();
    EnterCriticalSection(&g_seen_lock);
    uint16_t* m = seen_at(obj, 0);
    int seen = m && (*m & pass_seats());
    LeaveCriticalSection(&g_seen_lock);
    return (b & ~8u) | (seen ? 8u : 0u);
}

/* The map eraser (0x004225D9/0x004225F4): player 1's map; seats' pickups
 * never reach it (0x00422589). */
void hover_seen_forget(uint32_t obj) {
    if (g_seats < 2) return;
    seen_init();
    EnterCriticalSection(&g_seen_lock);
    uint16_t* m = seen_at(obj, 0);
    if (m) *m &= (uint16_t)~1u;
    LeaveCriticalSection(&g_seen_lock);
}

/* A level is loading (levels.c): its walls and objects are new, and an old
 * address may come back as one of them. */
void mp_level_reset(void) {
    EnterCriticalSection(&g_snap_lock);
    g_nsnap = 0;                         /* the old crafts are going */
    LeaveCriticalSection(&g_snap_lock);
    seen_init();
    EnterCriticalSection(&g_seen_lock);
    memset(g_seen, 0, sizeof g_seen);
    LeaveCriticalSection(&g_seen_lock);
}

/* A craft's sprite frame is picked on the tick, facing the human
 * (0x0040A4C0); each pass faces them to its own camera instead, the same
 * way: the angle from craft to camera against the craft's heading, 32
 * frames and their mirror images. */
static void face_sprites(uint32_t cam) {
    uint32_t sp = g_esp, eax = g_eax, ecx = g_ecx, edx = g_edx;
    snap_t crafts[64];
    int n = snap_copy(crafts);
    for (int k = 0; k < n; k++) {
        uint32_t o = crafts[k].craft, spr = crafts[k].sprite;
        if (!spr || o == cam) continue;
        uint32_t out = sp - 16;
        g_esp = sp - 32;
        MEM32(g_esp) = RECOMP_RETADDR;
        MEM32(g_esp + 4) = out;
        MEM32(g_esp + 8) = MEM32(cam + 0x48) - MEM32(o + 0x48);
        MEM32(g_esp + 12) = MEM32(cam + 0x44) - MEM32(o + 0x44);
        sub_004386D0();
        int rel = ((((int16_t)MEM16(o + 0x19E) - (int16_t)MEM16(out + 2)) & 0x1F8) << 6) >> 9;
        int mirror = rel >= 32;
        MEM32(spr + 0x50) = MEM32(SPRITE_FRAMES + 4 * (mirror ? 63 - rel : rel));
        MEM32(spr + 0x68) = (uint32_t)mirror;
    }
    g_esp = sp; g_eax = eax; g_ecx = ecx; g_edx = edx;   /* it keeps ebx/esi/edi/ebp itself */
}

void hover_render_views(void) {
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ebx = g_ebx, esp = g_esp, ebp = g_ebp,
             esi = g_esi, edi = g_edi;
    int n = g_nlocal;
    g_views_thread = GetCurrentThreadId();
    g_in_views = 1;
    for (int v = n - 1; v >= 0; v--) {
        uint32_t robot = g_local[v] ? snap_seat_craft(g_local[v]) : 0, was[HUD_VALS], mine[HUD_VALS];
        g_eax = eax; g_ecx = ecx; g_edx = edx; g_ebx = ebx;
        g_esp = esp; g_ebp = ebp; g_esi = esi; g_edi = edi;
        g_view = v;
        if (n > 1 || robot) hud_mark(ecx);
        if (robot) hud_swap_in(ecx, robot, g_local[v], was, mine);
        if (g_seats > 1) {
            uint32_t cam = hover_cam_obj(MEM32(G_DOC));
            g_own_sprite = MEM32(cam + 0x78);
            face_sprites(cam);
        }
        sub_00402250();
        if (robot) hud_put_back(ecx, was, mine);
    }
    g_own_sprite = 0;
    g_view = 0;
    g_in_views = 0;
}

int mp_view(void) { return g_view; }

/* Blits outside the render passes (the dashboard's background, drawn only in
 * WM_PAINT on the UI thread) belong to every view's cell. */
int mp_in_views(void) { return g_in_views && GetCurrentThreadId() == g_views_thread; }

/* ------------------------------------------------------------ layout */

int  mp_seats(void) { return g_seats; }
int  mp_local_count(void) { return g_nlocal; }
int  mp_local_seat(int li) { return li >= 0 && li < g_nlocal ? g_local[li] : -1; }
long mp_layout(void) { return g_layout; }

void mp_grid(int* cols, int* rows) {
    int n = g_nlocal;
    *cols = n >= 2 ? 2 : 1;           /* 2: side by side; 3-4: two by two */
    *rows = n >= 3 ? 2 : 1;
}

/* Seats and which of them are shown here. Split screen: seats 0..n-1, all
 * local. Online (net.c): the session's total, and this PC's block. */
void mp_set_seats(int total, const int* local, int nlocal) {
    if (total < 1) total = 1;
    if (total > MP_MAX_SEATS) total = MP_MAX_SEATS;
    if (nlocal < 1) nlocal = 1;
    if (nlocal > MP_MAX_LOCAL) nlocal = MP_MAX_LOCAL;
    g_seats = total;
    for (int i = 0; i < nlocal; i++) g_local[i] = local[i];
    g_nlocal = nlocal;
    InterlockedIncrement(&g_layout);
    mp_apply_table();
    pad_first_only(nlocal > 1);       /* controller 1 is local player 1's; 2-4 the others' */
}

static void set_players(int n) {
    int local[MP_MAX_LOCAL];
    char b[8];
    if (n < 1) n = 1;
    if (n > MP_MAX_LOCAL) n = MP_MAX_LOCAL;
    for (int i = 0; i < n; i++) local[i] = i;
    mp_set_seats(n, local, n);
    _snprintf(b, sizeof b, "%d", n);
    WritePrivateProfileStringA("mp", "players", b, g_ini);
    fprintf(stderr, "[mp] %d player%s\n", n, n > 1 ? "s, split screen" : "");
}

void mp_init(const char* ini, int cli_players, int (*key_down)(int vk)) {
    int local[MP_MAX_LOCAL];
    strncpy(g_ini, ini, sizeof g_ini - 1);
    g_key = key_down;
    InitializeCriticalSection(&g_snap_lock);
    if (!GetPrivateProfileIntA("mp", "players", 0, g_ini)) WritePrivateProfileStringA("mp", "players", "1", g_ini);
    int n = cli_players ? cli_players : GetPrivateProfileIntA("mp", "players", 1, g_ini);
    n = n < 1 ? 1 : n > MP_MAX_LOCAL ? MP_MAX_LOCAL : n;
    for (int i = 0; i < n; i++) local[i] = i;
    g_seats = n;
    for (int i = 0; i < n; i++) g_local[i] = local[i];
    g_nlocal = n;
    pad_first_only(n > 1);
    if (n > 1) fprintf(stderr, "[mp] %d players, split screen\n", n);
    char t[MP_MAX_SEATS + 1];
    if (GetPrivateProfileStringA("mp", "teams", "", t, sizeof t, g_ini)) mp_set_teams(t);
}

/* ------------------------------------------------------------ menu */

enum { ID_PLAYERS = 0x6B00 };   /* + 1..4 */

void mp_menu(HMENU recomp) {
    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, ID_PLAYERS + 1, "&1 player");
    AppendMenuA(m, MF_STRING, ID_PLAYERS + 2, "&2 players (split screen)");
    AppendMenuA(m, MF_STRING, ID_PLAYERS + 3, "&3 players");
    AppendMenuA(m, MF_STRING, ID_PLAYERS + 4, "&4 players");
    net_menu(m);
    AppendMenuA(recomp, MF_POPUP, (UINT_PTR)m, "&Multiplayer");
}

void mp_update_menu(HMENU popup) {
    UINT on = net_active() ? MF_GRAYED : MF_ENABLED;   /* the session decides, online */
    for (int i = 1; i <= MP_MAX_LOCAL; i++) {
        EnableMenuItem(popup, ID_PLAYERS + i, MF_BYCOMMAND | on);
        CheckMenuItem(popup, ID_PLAYERS + i,
                      MF_BYCOMMAND | (!net_active() && g_nlocal == i ? MF_CHECKED : MF_UNCHECKED));
    }
    net_update_menu(popup);
}

int mp_command(UINT id) {
    if (net_command(id)) return 1;
    if (id < ID_PLAYERS + 1 || id > ID_PLAYERS + MP_MAX_LOCAL || net_active()) return 0;
    set_players((int)(id - ID_PLAYERS));
    return 1;
}
