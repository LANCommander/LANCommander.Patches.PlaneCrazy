/* Research aid: with env PC_TRACE set, logs pcrazy.exe's registry and file system calls to pcpatch.log
 * (who reads/writes what, for the registry and saves phases). Inert otherwise. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pcpatch.h"

static LONG (WINAPI *real_RegCreateKeyExA)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, void *, PHKEY, LPDWORD);
static LONG (WINAPI *real_RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
static LONG (WINAPI *real_RegSetValueExA)(HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);
static HANDLE (WINAPI *real_CreateFileA)(LPCSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
static HFILE (WINAPI *real__lopen)(LPCSTR, int);
static HANDLE (WINAPI *real_FindFirstFileA)(LPCSTR, LPWIN32_FIND_DATAA);
static BOOL (WINAPI *real_DeleteFileA)(LPCSTR);
static BOOL (WINAPI *real_MoveFileA)(LPCSTR, LPCSTR);

#define CALLER ((uint32_t)(uintptr_t)__builtin_return_address(0))

static LONG WINAPI t_RegCreateKeyExA(HKEY key, LPCSTR sub, DWORD r, LPSTR cls, DWORD opt, REGSAM sam, void *sa,
                                     PHKEY out, LPDWORD disp)
{
    LONG rc = real_RegCreateKeyExA(key, sub, r, cls, opt, sam, sa, out, disp);
    pc_log("trace %08x RegCreateKeyExA(%p, \"%s\") = %ld -> %p", CALLER, (void *)key, sub ? sub : "", rc,
           out ? (void *)*out : NULL);
    return rc;
}

static LONG WINAPI t_RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD r, LPDWORD type, LPBYTE data, LPDWORD size)
{
    LONG rc = real_RegQueryValueExA(key, name, r, type, data, size);
    pc_log("trace %08x RegQueryValueExA(%p, \"%s\") = %ld type %lu size %lu", CALLER, (void *)key, name ? name : "",
           rc, type ? *type : 0, size ? *size : 0);
    return rc;
}

static LONG WINAPI t_RegSetValueExA(HKEY key, LPCSTR name, DWORD r, DWORD type, const BYTE *data, DWORD size)
{
    LONG rc = real_RegSetValueExA(key, name, r, type, data, size);
    pc_log("trace %08x RegSetValueExA(%p, \"%s\", type %lu, size %lu) = %ld", CALLER, (void *)key, name ? name : "",
           type, size, rc);
    return rc;
}

static HANDLE WINAPI t_CreateFileA(LPCSTR name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags,
                                   HANDLE tmpl)
{
    HANDLE h = real_CreateFileA(name, access, share, sa, disp, flags, tmpl);
    pc_log("trace %08x CreateFileA(\"%s\", %08lx, disp %lu) = %s", CALLER, name ? name : "", access, disp,
           h == INVALID_HANDLE_VALUE ? "fail" : "ok");
    return h;
}

static HFILE WINAPI t__lopen(LPCSTR name, int mode)
{
    HFILE h = real__lopen(name, mode);
    pc_log("trace %08x _lopen(\"%s\", %d) = %d", CALLER, name ? name : "", mode, h);
    return h;
}

static HANDLE WINAPI t_FindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA fd)
{
    HANDLE h = real_FindFirstFileA(name, fd);
    pc_log("trace %08x FindFirstFileA(\"%s\") = %s", CALLER, name ? name : "", h == INVALID_HANDLE_VALUE ? "none" : "ok");
    return h;
}

static BOOL WINAPI t_DeleteFileA(LPCSTR name)
{
    pc_log("trace %08x DeleteFileA(\"%s\")", CALLER, name ? name : "");
    return real_DeleteFileA(name);
}

static BOOL WINAPI t_MoveFileA(LPCSTR from, LPCSTR to)
{
    pc_log("trace %08x MoveFileA(\"%s\", \"%s\")", CALLER, from ? from : "", to ? to : "");
    return real_MoveFileA(from, to);
}

#define HOOK(dll, fn) (*(void **)&real_##fn = pc_hook_import(dll, #fn, (void *)t_##fn))

/* the game's own debug log (0x49ffd0, printf-style, an empty "ret" in the release build: "setting mode to
   %dx%dx%d", D3D errors...), with env PC_GAMELOG set */
static void game_log(const char *fmt, ...)
{
    char text[512];
    size_t n;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    n = strlen(text);
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) {
        text[--n] = '\0';
    }
    pc_log("game %08x: %s", CALLER, text);
}

void pc_trace_install(void)
{
    char v[8];
    if (GetEnvironmentVariableA("PC_GAMELOG", v, sizeof v) && pc_expect(0x49ffd0, "\xc3", 1)) {
        pc_jmp(0x49ffd0, (void *)game_log);
        pc_log("trace: game debug log on");
    }
    if (!GetEnvironmentVariableA("PC_TRACE", v, sizeof v)) {
        return;
    }
    HOOK("ADVAPI32.dll", RegCreateKeyExA);
    HOOK("ADVAPI32.dll", RegQueryValueExA);
    HOOK("ADVAPI32.dll", RegSetValueExA);
    HOOK("KERNEL32.dll", CreateFileA);
    HOOK("KERNEL32.dll", _lopen);
    HOOK("KERNEL32.dll", FindFirstFileA);
    HOOK("KERNEL32.dll", DeleteFileA);
    HOOK("KERNEL32.dll", MoveFileA);
    pc_log("trace: registry and file calls are logged");
}
