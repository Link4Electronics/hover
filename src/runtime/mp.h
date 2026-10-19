/* Multiplayer seats and views: mp.c. */
#pragma once
#include "hover_port.h"
#include <stdint.h>
#include "recomp_types.h"

#define MP_MAX_SEATS 16             /* seats in one game, every PC */
#define MP_MAX_LOCAL 4              /* views on one PC */
enum { MP_JUMP = 1, MP_WALL = 2, MP_CLOAK = 4 };

void          mp_init(const char* ini, int cli_players, int (*key_down)(int vk));
void          mp_set_seats(int total, const int* local, int nlocal);
int           mp_seats(void);
int           mp_local_count(void);
int           mp_local_seat(int li);            /* this PC's li-th seat, or -1 */
void          mp_local_input(int li, double* turn, double* thrust, int* buttons);
uint32_t      mp_world_hash(void);              /* every craft's state, for the desync check */
void          mp_apply_table(void);             /* after the image is mapped: seats' robots */
long          mp_layout(void);                  /* changes when the view grid does */
void          mp_grid(int* cols, int* rows);
int           mp_view(void);                    /* the render pass now running, 0 = first local seat */
int           mp_in_views(void);                /* is a render pass running on this thread? */
recomp_func_t mp_lookup(uint32_t va);           /* for recomp_lookup_manual */
void          mp_menu(HMENU recomp);
void          mp_update_menu(HMENU popup);
int           mp_command(UINT id);

/* Called from the lifted frame draw (patched in by run_lift.py). */
uint32_t hover_cam_obj(uint32_t doc);
void     hover_render_views(void);
int      mp_is_seat(uint32_t craft);                       /* seat 1.. whose craft this is, or 0 */
uint32_t hover_heard(uint32_t craft);                      /* are this craft's sounds played here? */
uint32_t hover_pod_hud(uint32_t view, uint32_t owner);     /* pod gauges: the view, or a seat's block */
void     hover_sprite_visible(void);
void     hover_seen_mark(uint32_t obj);           /* the radar's seen marks, per seat */
uint32_t hover_seen_byte(uint32_t obj);
void     hover_seen_forget(uint32_t obj);
void     mp_level_reset(void);                     /* levels.c: a level is loading */
uint32_t hover_team_flags(uint32_t craft);
uint32_t hover_quarry(uint32_t doc, uint32_t state);
void     mp_get_teams(char* out);              /* MP_MAX_SEATS letters and a NUL: the online session's */
void     mp_set_teams(const char* teams);    /* one letter a seat, h or r; "hr" is the default for two */
