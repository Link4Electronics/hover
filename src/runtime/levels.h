/* Level seeds: levels.c. */
#pragma once
#include "hover_port.h"
#include <stdint.h>

void     levels_init(const char* ini, uint32_t cli_seed, int cli_pinned);
void     levels_load(void);                                  /* reload [levels], [saved] */
void     levels_attach(HWND frame, void (*set_title)(const char*));
uint32_t levels_seed(void);                                  /* from GetLocalTime: the seed for time() */
void     levels_menu(HMENU recomp);                          /* the Level submenu */
void     levels_update_menu(HMENU popup);
int      levels_command(UINT id);
int      levels_selftest(void);
void     levels_session(int level, uint32_t seed);           /* online: the host's level and seed */
uint32_t levels_current_seed(void);                          /* the pinned seed, or the time */
