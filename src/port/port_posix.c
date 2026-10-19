/*
 * port_posix.c - the host-side Win32 the portable build needs.
 *
 * Not a Win32 layer for the game (win32hle is that): just the pieces the
 * host's own C calls - an .ini file, menus that do not exist on this host,
 * the clipboard through SDL, locks, atomics and the clock.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#include "hover_port.h"
#include "win32hle.h"

/* ------------------------------------------------------------ locks/atomics */

void InitializeCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->m, &a);
    pthread_mutexattr_destroy(&a);
}
void DeleteCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_destroy(&cs->m); }
void EnterCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_lock(&cs->m); }
void LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock(&cs->m); }

LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedDecrement(volatile LONG *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedCompareExchange(volatile LONG *p, LONG exchange, LONG comparand) {
    LONG old = comparand;
    __atomic_compare_exchange_n(p, &old, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old;
}

DWORD GetCurrentThreadId(void) { return (DWORD)(uintptr_t)pthread_self(); }
DWORD GetTickCount(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
void Sleep(DWORD ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

/* -------------------------------------------------------------- hover.ini
 * A whole-file parser: sections, key=value, comments and unknown lines kept.
 * Keys and sections match without case, as GetPrivateProfileString does. */
typedef struct { char **line; int n, cap; } ini_t;

static pthread_mutex_t g_ini_lock = PTHREAD_MUTEX_INITIALIZER;

static void ini_add(ini_t *f, const char *line) {
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 32;
        f->line = (char **)realloc(f->line, sizeof(char *) * (size_t)f->cap);
    }
    f->line[f->n++] = strdup(line);
}
static void ini_free(ini_t *f) {
    for (int i = 0; i < f->n; i++) free(f->line[i]);
    free(f->line);
    memset(f, 0, sizeof *f);
}
static void ini_read(ini_t *f, const char *path) {
    FILE *fp = fopen(path, "r");
    char buf[4096];
    memset(f, 0, sizeof *f);
    if (!fp) return;
    while (fgets(buf, sizeof buf, fp)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        ini_add(f, buf);
    }
    fclose(fp);
}
static int ini_save(const ini_t *f, const char *path) {
    char tmp[MAX_PATH + 8];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return 0;
    FILE *fp = fopen(tmp, "w");
    if (!fp) return 0;
    for (int i = 0; i < f->n; i++) fprintf(fp, "%s\n", f->line[i]);
    if (fclose(fp) != 0) { remove(tmp); return 0; }
    if (rename(tmp, path) != 0) { remove(tmp); return 0; }
    return 1;
}
static const char *trim(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}
static int is_section(const char *line, const char *name) {
    size_t n = strlen(name);
    const char *s = trim(line);
    if (*s != '[') return 0;
    s++;
    return strncasecmp(s, name, n) == 0 && s[n] == ']';
}
/* The line's key, or NULL; *eq is the '=' after it. */
static const char *line_key(const char *line, const char **eq) {
    const char *s = trim(line);
    const char *e;
    if (*s == '[' || *s == ';' || *s == 0) return NULL;
    e = strchr(s, '=');
    if (!e) return NULL;
    *eq = e;
    return s;
}
static int key_is(const char *line, const char *key, const char **eq) {
    const char *s = line_key(line, eq);
    size_t n = strlen(key);
    return s && strncasecmp(s, key, n) == 0 && (size_t)(*eq - s) == n;
}

static long find_section(const ini_t *f, const char *section, long after) {
    for (int i = (int)after; i < f->n; i++) if (is_section(f->line[i], section)) return i;
    return -1;
}
static long section_end(const ini_t *f, long start) {
    for (int i = (int)start + 1; i < f->n; i++) if (trim(f->line[i])[0] == '[') return i;
    return f->n;
}

UINT GetPrivateProfileStringA(const char *section, const char *key, const char *dflt,
                              char *out, UINT n, const char *path) {
    ini_t f;
    const char *got = NULL;
    pthread_mutex_lock(&g_ini_lock);
    ini_read(&f, path);
    long s = find_section(&f, section, 0);
    if (s >= 0) {
        long e = section_end(&f, s);
        for (long i = s + 1; i < e; i++) {
            const char *eq;
            if (!key_is(f.line[i], key, &eq)) continue;
            got = trim(eq + 1);
            break;
        }
    }
    if (got) {
        size_t k = strlen(got);
        if (k >= n) k = n ? n - 1 : 0;
        if (n) { memcpy(out, got, k); out[k] = 0; }
        ini_free(&f);
        pthread_mutex_unlock(&g_ini_lock);
        return (UINT)k;
    }
    size_t k = dflt ? strlen(dflt) : 0;
    if (k >= n) k = n ? n - 1 : 0;
    if (n) { if (dflt) memcpy(out, dflt, k); out[k] = 0; }
    ini_free(&f);
    pthread_mutex_unlock(&g_ini_lock);
    return (UINT)k;
}

UINT GetPrivateProfileIntA(const char *section, const char *key, int dflt, const char *path) {
    char b[64];
    if (!GetPrivateProfileStringA(section, key, "", b, sizeof b, path) || !b[0]) return (UINT)dflt;
    return (UINT)strtol(b, NULL, 10);
}

BOOL WritePrivateProfileStringA(const char *section, const char *key, const char *value,
                                const char *path) {
    ini_t f;
    BOOL ok = TRUE;
    pthread_mutex_lock(&g_ini_lock);
    ini_read(&f, path);
    long s = find_section(&f, section, 0);
    if (!key && !value) {                          /* clear the section's keys */
        if (s >= 0) {
            long e = section_end(&f, s);
            for (long i = e - 1; i > s; i--) {
                free(f.line[i]);
                memmove(&f.line[i], &f.line[i + 1], sizeof(char *) * (size_t)(f.n - i - 1));
                f.n--;
            }
        }
    } else if (!value) {                           /* delete one key */
        if (s >= 0) {
            long e = section_end(&f, s);
            for (long i = s + 1; i < e; i++) {
                const char *eq;
                if (!key_is(f.line[i], key, &eq)) continue;
                free(f.line[i]);
                memmove(&f.line[i], &f.line[i + 1], sizeof(char *) * (size_t)(f.n - i - 1));
                f.n--;
                break;
            }
        }
    } else {
        char line[4096];
        if (s < 0) {                               /* a new section at the end */
            if (f.n) ini_add(&f, "");
            snprintf(line, sizeof line, "[%s]", section);
            ini_add(&f, line);
            s = f.n - 1;
        }
        snprintf(line, sizeof line, "%s=%s", key, value);
        long e = section_end(&f, s), at = -1;
        for (long i = s + 1; i < e; i++) {
            const char *eq;
            if (key_is(f.line[i], key, &eq)) { at = i; break; }
        }
        if (at >= 0) { free(f.line[at]); f.line[at] = strdup(line); }
        else {
            if (f.n == f.cap) {
                f.cap = f.cap ? f.cap * 2 : 32;
                f.line = (char **)realloc(f.line, sizeof(char *) * (size_t)f.cap);
            }
            memmove(&f.line[e + 1], &f.line[e], sizeof(char *) * (size_t)(f.n - e));
            f.line[e] = strdup(line);
            f.n++;
        }
    }
    ok = ini_save(&f, path) ? TRUE : FALSE;
    ini_free(&f);
    pthread_mutex_unlock(&g_ini_lock);
    return ok;
}

/* ------------------------------------------------------------- menus
 * The portable host has no Win32 menu bar. Distinct non-zero handles keep
 * the level code's g_saved_menu comparisons meaningful; every call is a
 * no-op. */
static LONG g_menu_next;
HMENU CreatePopupMenu(void) { return (HMENU)(uintptr_t)(InterlockedIncrement(&g_menu_next) + 1); }
BOOL AppendMenuA(HMENU m, UINT f, UINT_PTR id, const char *t) { (void)m; (void)f; (void)id; (void)t; return TRUE; }
BOOL ModifyMenuA(HMENU m, UINT id, UINT f, UINT_PTR n, const char *t) { (void)m; (void)id; (void)f; (void)n; (void)t; return TRUE; }
BOOL EnableMenuItem(HMENU m, UINT id, UINT f) { (void)m; (void)id; (void)f; return TRUE; }
BOOL CheckMenuItem(HMENU m, UINT id, UINT f) { (void)m; (void)id; (void)f; return TRUE; }
int  GetMenuItemCount(HMENU m) { (void)m; return 0; }
BOOL DeleteMenu(HMENU m, UINT id, UINT f) { (void)m; (void)id; (void)f; return TRUE; }
UINT GetMenuItemID(HMENU m, int pos) { (void)m; (void)pos; return (UINT)-1; }
BOOL DrawMenuBar(HWND w) { (void)w; return TRUE; }

/* ------------------------------------------------------- text and clipboard */

BOOL MessageBoxA(HWND wnd, const char *text, const char *caption, UINT type) {
    (void)wnd; (void)type;
    fprintf(stderr, "[messagebox] %s: %s\n", caption ? caption : "", text ? text : "");
    return IDOK;
}

BOOL PostMessageA(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    hle_post_message(wnd, msg, wparam, lparam);
    return TRUE;
}

HGLOBAL GlobalAlloc(UINT flags, size_t n) { (void)flags; return malloc(n ? n : 1); }
void   *GlobalLock(HGLOBAL h) { return h; }
BOOL    GlobalUnlock(HGLOBAL h) { (void)h; return TRUE; }
HGLOBAL GlobalFree(HGLOBAL h) { free(h); return NULL; }

static char g_clip[8192];
BOOL OpenClipboard(HWND wnd) { (void)wnd; return TRUE; }
BOOL EmptyClipboard(void) { g_clip[0] = 0; return TRUE; }
HANDLE SetClipboardData(UINT fmt, HANDLE h) {
    if (fmt != CF_TEXT || !h) return NULL;
    snprintf(g_clip, sizeof g_clip, "%s", (const char *)h);
    SDL_SetClipboardText(g_clip);
    free(h);
    return (HANDLE)g_clip;
}
HANDLE GetClipboardData(UINT fmt) {
    char *t;
    if (fmt != CF_TEXT) return NULL;
    t = SDL_GetClipboardText();
    if (!t) return NULL;
    snprintf(g_clip, sizeof g_clip, "%s", t);
    SDL_free(t);
    return (HANDLE)g_clip;
}
BOOL CloseClipboard(void) { return TRUE; }

DWORD GetTempPathA(DWORD n, char *out) {
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    snprintf(out, n, "%s%s", t, t[strlen(t) - 1] == '/' ? "" : "/");
    return (DWORD)strlen(out);
}
BOOL DeleteFileA(const char *path) { return remove(path) == 0; }
