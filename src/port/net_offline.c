/*
 * net_offline.c - the net.h interface for the portable host.
 *
 * Online play (net.c) is Windows-only for now: it is winsock plus a second
 * lockstep thread, and neither is ported yet. Everything here answers "no
 * session", so mp.c and levels.c compile and run unchanged; split screen and
 * single player do not touch these answers.
 */
#include <stdio.h>
#include <string.h>

#include "net.h"

void net_init(const char *ini, HWND frame, int headless) {
    (void)ini; (void)frame; (void)headless;
}
int net_host(int port, int clients, int nlocal, int seats) {
    (void)port; (void)clients; (void)nlocal; (void)seats;
    fprintf(stderr, "[net] online play is not available on this host yet\n");
    return 0;
}
int net_join(const char *addr, int nlocal) {
    (void)addr; (void)nlocal;
    fprintf(stderr, "[net] online play is not available on this host yet\n");
    return 0;
}
void net_leave(void) {}
int  net_active(void) { return 0; }
int  net_is_host(void) { return 0; }
int  net_playing(void) { return 0; }
int  net_catching_up(void) { return 0; }
void net_desync_test(int tick) { (void)tick; }

void net_seat_input(int seat, double *turn, double *thrust, int *buttons) {
    (void)seat;
    if (turn) *turn = 0;
    if (thrust) *thrust = 0;
    if (buttons) *buttons = 0;
}
int net_seat_ai(int seat) { (void)seat; return 1; }
int net_key(int vk, int *down) { (void)vk; (void)down; return 0; }

int  net_timer_set(uint32_t cb, uint32_t user, uint32_t *id) { (void)cb; (void)user; (void)id; return 0; }
int  net_timer_kill(uint32_t id) { (void)id; return 0; }
void net_saw_post(HWND h, UINT msg) { (void)h; (void)msg; }
void net_level_loading(int demo) { (void)demo; }

void net_menu(HMENU multiplayer) { (void)multiplayer; }
void net_update_menu(HMENU popup) { (void)popup; }
int  net_command(UINT id) { (void)id; return 0; }

int net_selftest(void) {
    fprintf(stderr, "[net] offline host: online play is not built in\n");
    return 0;
}
