/* Xbox controller support: pad.c. */
#pragma once
#include "hover_port.h"

void pad_start(const char* ini, HWND frame);   /* load [pad], start polling */
int  pad_held(int vk);                         /* is the pad holding this key? */
void pad_menu(HMENU recomp);                   /* the Controller submenu */
void pad_reload(void);                         /* reload [pad] */
void pad_update_menu(HMENU popup);             /* after MFC's WM_INITMENUPOPUP */
int  pad_command(UINT id);                     /* 1 if the WM_COMMAND id was ours */
int  pad_read(int user, XINPUT_GAMEPAD* out);  /* controller `user` (0-3) now; 0 = none */
void pad_first_only(int on);                   /* several players: controller 1 is player 1's */
int  pad_selftest(void);                       /* --pad-selftest; 0 = pass */
