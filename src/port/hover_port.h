/*
 * hover_port.h - what the host's own C uses from Win32, on both hosts.
 *
 * On Windows this is windows.h and xinput.h, as before, and nothing changes.
 * On the portable host (win32hle, Linux/macOS/Android) the same names are
 * implemented on top of libc, POSIX and SDL2 in src/port/port_posix.c: an
 * .ini file layer, no-op menus, the clipboard, critical sections and the
 * interlocked operations. The game's *imports* are not here: win32hle answers
 * those. This is only the host code (mp.c, levels.c, pad_sdl.c).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#else

#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#define MAX_PATH 260
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _stricmp strcasecmp

typedef uint32_t DWORD;
typedef uint32_t ULONG;
typedef int32_t  LONG;
typedef uint32_t UINT;
typedef uintptr_t UINT_PTR;
typedef int      BOOL;
typedef uint16_t WORD;
typedef int16_t  SHORT;
typedef uint8_t  BYTE;
typedef uint32_t HWND;
typedef uint32_t HMENU;
typedef uint32_t WPARAM;
typedef uint32_t LPARAM;
typedef void    *HANDLE;
typedef void    *HGLOBAL;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define IDOK 1

/* ---- keyboard and messages ---- */
#define WM_KEYDOWN        0x0100
#define WM_KEYUP          0x0101
#define VK_BACK           0x08
#define VK_TAB            0x09
#define VK_RETURN         0x0D
#define VK_SHIFT          0x10
#define VK_CONTROL        0x11
#define VK_ESCAPE         0x1B
#define VK_SPACE          0x20
#define VK_PRIOR          0x21
#define VK_NEXT           0x22
#define VK_END            0x23
#define VK_HOME           0x24
#define VK_LEFT           0x25
#define VK_UP             0x26
#define VK_RIGHT          0x27
#define VK_DOWN           0x28
#define VK_F1             0x70
#define VK_F2             0x71
#define VK_F3             0x72
#define VK_F4             0x73
#define VK_F5             0x74
#define VK_F6             0x75
#define VK_F7             0x76
#define VK_F8             0x77
#define VK_F9             0x78
#define VK_F10            0x79
#define VK_F11            0x7A
#define VK_F12            0x7B

/* ---- menus ---- */
#define MF_STRING         0x0000
#define MF_POPUP          0x0010
#define MF_SEPARATOR      0x0800
#define MF_ENABLED        0x0000
#define MF_GRAYED         0x0001
#define MF_CHECKED        0x0008
#define MF_UNCHECKED      0x0000
#define MF_BYCOMMAND      0x0000
#define MF_BYPOSITION     0x0400

/* ---- clipboard and message boxes ---- */
#define CF_TEXT           1
#define GMEM_MOVEABLE     0x0002
#define MB_OK             0x0000
#define MB_ICONINFORMATION 0x0040
#define MB_ICONWARNING    0x0030

/* ---- display caps (the startup 256-colour check) ---- */
#define RASTERCAPS        38
#define RC_PALETTE        0x0100
#define SIZEPALETTE      104

/* ---- SYSTEMTIME, as GetLocalTime's shim writes it ---- */
typedef struct {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;
typedef struct { BYTE b[172]; } TIME_ZONE_INFORMATION;
#define TIME_ZONE_ID_UNKNOWN 0

/* ---- locks, atomics, threads, time ---- */
typedef struct { pthread_mutex_t m; } CRITICAL_SECTION;
void  InitializeCriticalSection(CRITICAL_SECTION *cs);
void  DeleteCriticalSection(CRITICAL_SECTION *cs);
void  EnterCriticalSection(CRITICAL_SECTION *cs);
void  LeaveCriticalSection(CRITICAL_SECTION *cs);
LONG  InterlockedIncrement(volatile LONG *p);
LONG  InterlockedDecrement(volatile LONG *p);
LONG  InterlockedCompareExchange(volatile LONG *p, LONG exchange, LONG comparand);
DWORD GetCurrentThreadId(void);
DWORD GetTickCount(void);
void  Sleep(DWORD ms);

/* ---- hover.ini ---- */
UINT GetPrivateProfileStringA(const char *section, const char *key, const char *dflt,
                              char *out, UINT n, const char *file);
UINT GetPrivateProfileIntA(const char *section, const char *key, int dflt, const char *file);
BOOL WritePrivateProfileStringA(const char *section, const char *key, const char *value,
                                const char *file);

/* ---- menus (there is no menu bar on the portable host: no-ops) ---- */
HMENU CreatePopupMenu(void);
BOOL  AppendMenuA(HMENU menu, UINT flags, UINT_PTR id, const char *text);
BOOL  ModifyMenuA(HMENU menu, UINT id, UINT flags, UINT_PTR newid, const char *text);
BOOL  EnableMenuItem(HMENU menu, UINT id, UINT flags);
BOOL  CheckMenuItem(HMENU menu, UINT id, UINT flags);
int   GetMenuItemCount(HMENU menu);
BOOL  DeleteMenu(HMENU menu, UINT id, UINT flags);
UINT  GetMenuItemID(HMENU menu, int pos);
BOOL  DrawMenuBar(HWND wnd);

/* ---- text, clipboard, files ---- */
BOOL     MessageBoxA(HWND wnd, const char *text, const char *caption, UINT type);
BOOL     PostMessageA(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
HGLOBAL  GlobalAlloc(UINT flags, size_t n);
void    *GlobalLock(HGLOBAL h);
BOOL     GlobalUnlock(HGLOBAL h);
HGLOBAL  GlobalFree(HGLOBAL h);
BOOL     OpenClipboard(HWND wnd);
BOOL     EmptyClipboard(void);
HANDLE   SetClipboardData(UINT fmt, HANDLE h);
HANDLE   GetClipboardData(UINT fmt);
BOOL     CloseClipboard(void);
DWORD    GetTempPathA(DWORD n, char *out);
BOOL     DeleteFileA(const char *path);

/* ---- Xbox controller input, from SDL2 on this host ---- */
typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XINPUT_GAMEPAD;
#define XINPUT_GAMEPAD_DPAD_UP        0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN      0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT      0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT     0x0008
#define XINPUT_GAMEPAD_START          0x0010
#define XINPUT_GAMEPAD_BACK           0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB     0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB    0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER  0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A              0x1000
#define XINPUT_GAMEPAD_B              0x2000
#define XINPUT_GAMEPAD_X              0x4000
#define XINPUT_GAMEPAD_Y              0x8000

#endif /* _WIN32 */
