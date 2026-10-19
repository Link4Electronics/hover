/* Online play, lockstep over UDP: net.c. */
#pragma once
#include "hover_port.h"
#include <stdint.h>

void net_init(const char* ini, HWND frame, int headless);
int  net_host(int port, int clients, int nlocal, int seats);   /* start after `clients` PCs (0: now) */
int  net_join(const char* addr, int nlocal);        /* "host[:port]" or a join code */
void net_leave(void);                               /* tell the others, before quitting */
int  net_active(void);                              /* a session is running */
int  net_is_host(void);
int  net_playing(void);
int  net_catching_up(void);                         /* a joiner replaying the game so far */                             /* the session's level has started */
void net_desync_test(int tick);                     /* a deliberate one-unit nudge: the check's test */
int  net_selftest(void);

/* Inputs for the tick the game is handling. */
void net_seat_input(int seat, double* turn, double* thrust, int* buttons);   /* robot seats (mp.c) */
int  net_seat_ai(int seat);                         /* nobody drives this seat now: its AI does */
int  net_key(int vk, int* down);                    /* the key shims: 1 = answered from the record */

/* The game's timer and its tick message (host.c's shims). */
int  net_timer_set(uint32_t cb, uint32_t user, uint32_t* id);   /* 1 = taken over */
int  net_timer_kill(uint32_t id);
void net_saw_post(HWND h, UINT msg);
void net_level_loading(int demo);                  /* levels.c: a level (or the demo) is loading */

void net_menu(HMENU multiplayer);
void net_update_menu(HMENU popup);
int  net_command(UINT id);
