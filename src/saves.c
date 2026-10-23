/* Saves in the user's Saved Games folder instead of the install folder (docs/saves.md).
 *
 * Folder: <Saved Games>\Inner Workings\Plane Crazy (SHGetKnownFolderPath), [Saves] Folder= overrides it.
 *
 * The game chdir()s around its data folders (SetCurrentDirectoryA, e.g. to data\DataMin\Ghosts\<course>) and
 * opens files relative to them, through the CRT (CreateFileA) and _lopen. The IAT hooks resolve every path to
 * a full path; a file inside the install folder has a "mirror" in the saves folder: the path relative to the
 * install, with a leading data\DataMin\ dropped (data\DataMin\Planes\plane1.pln -> Planes\plane1.pln).
 *   - opened for writing: always the mirror (folders created as needed)
 *   - opened for reading, deleted, attributes: the mirror if it exists, else the install file
 *   - FindFirstFileA: the mirror folder if it exists (the game lists Planes\*.pln and Ghosts\<course>\*.ghs)
 * Files the game writes: data\DataMin\Planes\plane<N>.pln (custom planes), data\DataMin\Ghosts\<course>\*.ghs,
 * data\DataMin\Bovine\<course>\hiscore_{s,g,c}.dat. On the first start (no saves folder yet) those are copied
 * over from the install (migration). Settings come from registry.c, kept in the same folder. */
#include <stdio.h>
#include <string.h>
#include <shlobj.h>
#include <shlwapi.h>
#include "pcpatch.h"

/* FOLDERID_SavedGames {4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}, defined here to avoid needing -luuid */
static const GUID PC_FOLDERID_SavedGames = {0x4C5C32FF, 0xBB9D, 0x43B0, {0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4}};
typedef HRESULT (WINAPI *SHGetKnownFolderPath_fn)(const GUID *, DWORD, HANDLE, PWSTR *);

static char g_dir[MAX_PATH];        /* saves folder, with trailing backslash */
static char g_game[MAX_PATH];       /* install folder (exe dir), with trailing backslash */
static int g_redirect;

static HANDLE (WINAPI *real_CreateFileA)(LPCSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
static HFILE (WINAPI *real__lopen)(LPCSTR, int);
static HANDLE (WINAPI *real_FindFirstFileA)(LPCSTR, LPWIN32_FIND_DATAA);
static BOOL (WINAPI *real_DeleteFileA)(LPCSTR);
static DWORD (WINAPI *real_GetFileAttributesA)(LPCSTR);
static BOOL (WINAPI *real_SetFileAttributesA)(LPCSTR, DWORD);

/* ---------------------------------------------------------------- folder */

static void add_slash(char *s)
{
    size_t n = strlen(s);
    if (n && s[n - 1] != '\\' && n + 1 < MAX_PATH) {
        s[n] = '\\';
        s[n + 1] = '\0';
    }
}

static void make_dirs(const char *path)   /* creates every missing folder of a file path's parents */
{
    char tmp[MAX_PATH], *p;
    lstrcpynA(tmp, path, MAX_PATH);
    for (p = tmp + 3; *p; p++) {          /* skip "C:\" */
        if (*p == '\\') {
            *p = '\0';
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
    }
}

static int saved_games_folder(char *out)
{
    SHGetKnownFolderPath_fn fn;
    HMODULE shell = LoadLibraryA("shell32.dll");
    PWSTR w = NULL;
    int ok = 0;
    fn = shell ? (SHGetKnownFolderPath_fn)(void (*)(void))GetProcAddress(shell, "SHGetKnownFolderPath") : NULL;
    if (fn && fn(&PC_FOLDERID_SavedGames, KF_FLAG_CREATE, NULL, &w) == S_OK && w) {
        ok = WideCharToMultiByte(CP_ACP, 0, w, -1, out, MAX_PATH - 40, NULL, NULL) > 0;
        CoTaskMemFree(w);
    }
    if (ok) {
        add_slash(out);
        strcat(out, "Inner Workings\\Plane Crazy");
    }
    return ok;
}

const char *pc_saves_dir(void)
{
    return g_dir;
}

/* ---------------------------------------------------------------- mapping */

/* mirror of a path inside the install folder; 0 if the path is outside it (or redirection is off) */
static int mirror_of(const char *path, char *out)
{
    char full[MAX_PATH];
    const char *rel;
    size_t n = strlen(g_game);
    if (!g_redirect || !path || !GetFullPathNameA(path, MAX_PATH, full, NULL)) {
        return 0;
    }
    if (_strnicmp(full, g_game, n) != 0 || !full[n]) {
        return 0;
    }
    rel = full + n;
    if (_strnicmp(rel, "data\\DataMin\\", 13) == 0) {
        rel += 13;
    }
    if (strlen(g_dir) + strlen(rel) >= MAX_PATH) {
        return 0;
    }
    strcpy(out, g_dir);
    strcat(out, rel);
    return 1;
}

static int exists(const char *path)
{
    return real_GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* the file to read: the mirror if there is one */
static const char *for_read(const char *path, char *buf)
{
    return mirror_of(path, buf) && exists(buf) ? buf : path;
}

/* the file to write: always the mirror */
static const char *for_write(const char *path, char *buf)
{
    if (!mirror_of(path, buf)) {
        return path;
    }
    make_dirs(buf);
    return buf;
}

/* ---------------------------------------------------------------- hooks */

static HANDLE WINAPI h_CreateFileA(LPCSTR name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags,
                                   HANDLE tmpl)
{
    char buf[MAX_PATH];
    const char *path;
    int write = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) || disp == CREATE_ALWAYS
        || disp == CREATE_NEW || disp == TRUNCATE_EXISTING;
    if (write) {
        path = for_write(name, buf);
        /* updating a file in place that so far only exists in the install: start from a copy */
        if (path == buf && (disp == OPEN_EXISTING || disp == OPEN_ALWAYS) && !exists(buf)) {
            CopyFileA(name, buf, TRUE);
        }
        if (path == buf) {
            pc_log("saves: writing %s", buf);
        }
    } else {
        path = for_read(name, buf);
    }
    return real_CreateFileA(path, access, share, sa, disp, flags, tmpl);
}

static HFILE WINAPI h__lopen(LPCSTR name, int mode)
{
    char buf[MAX_PATH];
    return real__lopen((mode & (OF_WRITE | OF_READWRITE)) ? for_write(name, buf) : for_read(name, buf), mode);
}

static HANDLE WINAPI h_FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA fd)
{
    char buf[MAX_PATH], dir[MAX_PATH], *slash;
    if (mirror_of(pattern, buf)) {
        strcpy(dir, buf);
        slash = strrchr(dir, '\\');
        if (slash) {
            *slash = '\0';
            if (exists(dir)) {
                return real_FindFirstFileA(buf, fd);
            }
        }
    }
    return real_FindFirstFileA(pattern, fd);
}

static BOOL WINAPI h_DeleteFileA(LPCSTR name)
{
    char buf[MAX_PATH];
    return real_DeleteFileA(for_read(name, buf));
}

static DWORD WINAPI h_GetFileAttributesA(LPCSTR name)
{
    char buf[MAX_PATH];
    return real_GetFileAttributesA(for_read(name, buf));
}

static BOOL WINAPI h_SetFileAttributesA(LPCSTR name, DWORD attr)
{
    char buf[MAX_PATH];
    return real_SetFileAttributesA(for_read(name, buf), attr);
}

/* ---------------------------------------------------------------- migration */

static int g_copied;

/* copies src\<pattern> (files) into dst, recursing into subfolders when recurse is set; never overwrites */
static void copy_tree(const char *src, const char *dst, const char *pattern, int recurse)
{
    char find[MAX_PATH * 2], from[MAX_PATH * 2], to[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(find, sizeof find, "%s\\%s", src, recurse ? "*" : pattern);
    h = real_FindFirstFileA(find, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) {
            continue;
        }
        snprintf(from, sizeof from, "%s\\%s", src, fd.cFileName);
        snprintf(to, sizeof to, "%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recurse) {
                copy_tree(from, to, pattern, recurse);
            }
        } else if (!recurse || PathMatchSpecA(fd.cFileName, pattern)) {
            make_dirs(to);
            if (CopyFileA(from, to, TRUE)) {
                g_copied++;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void migrate(void)
{
    char src[MAX_PATH * 2], dst[MAX_PATH * 2];
    snprintf(src, sizeof src, "%sdata\\DataMin\\Planes", g_game);
    snprintf(dst, sizeof dst, "%sPlanes", g_dir);
    CreateDirectoryA(dst, NULL);
    copy_tree(src, dst, "*.pln", 0);
    snprintf(src, sizeof src, "%sdata\\DataMin\\Ghosts", g_game);
    snprintf(dst, sizeof dst, "%sGhosts", g_dir);
    copy_tree(src, dst, "*.ghs", 1);
    snprintf(src, sizeof src, "%sdata\\DataMin\\Bovine", g_game);
    snprintf(dst, sizeof dst, "%sBovine", g_dir);
    copy_tree(src, dst, "hiscore_?.dat", 1);
    pc_log("saves: first start, %d files copied over from the install", g_copied);
}

/* ---------------------------------------------------------------- install */

#define HOOK(dll, fn) (*(void **)&real_##fn = pc_hook_import(dll, #fn, (void *)h_##fn))

void pc_saves_install(void)
{
    char ovr[MAX_PATH], *slash;
    int fresh;

    GetModuleFileNameA(NULL, g_game, MAX_PATH);
    slash = strrchr(g_game, '\\');
    if (slash) {
        slash[1] = '\0';
    }
    if (pc_ini_str("Saves", "Folder", ovr, sizeof ovr)) {
        GetFullPathNameA(ovr, MAX_PATH, g_dir, NULL);
    } else if (!saved_games_folder(g_dir)) {
        pc_log("saves: no Saved Games folder; using the install folder");
        lstrcpynA(g_dir, g_game, MAX_PATH);
        return;
    }
    add_slash(g_dir);
    fresh = GetFileAttributesA(g_dir) == INVALID_FILE_ATTRIBUTES;
    make_dirs(g_dir);
    pc_log("saves: folder %s", g_dir);

    if (!pc_ini_int("Saves", "Redirect", 1)) {
        return;
    }
    real_GetFileAttributesA = GetFileAttributesA;
    real_FindFirstFileA = FindFirstFileA;
    g_redirect = 1;
    if (fresh) {
        migrate();
    }
    HOOK("KERNEL32.dll", CreateFileA);
    HOOK("KERNEL32.dll", _lopen);
    HOOK("KERNEL32.dll", FindFirstFileA);
    HOOK("KERNEL32.dll", DeleteFileA);
    HOOK("KERNEL32.dll", GetFileAttributesA);
    HOOK("KERNEL32.dll", SetFileAttributesA);
}
