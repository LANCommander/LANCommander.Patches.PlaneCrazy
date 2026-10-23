/* No registry: the game's settings are kept in Settings.ini in the saves folder (docs/registry.md).
 *
 * pcrazy.exe keeps all of its settings (video mode, sound device, options, key and joystick bindings, last plane)
 * in HKLM\Software\Inner Workings\Plane Crazy\1.0, through one small wrapper (REG_OpenKey 0x4685e0,
 * REG_ReadInt 0x468720, REG_ReadString 0x4686a0, REG_WriteInt/String 0x468870), and reads RegisteredOwner from
 * HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion for the default pilot name. Without admin rights Windows
 * virtualises the HKLM writes to HKCU\Software\Classes\VirtualStore.
 *
 * The IAT entries RegCreateKeyExA / RegQueryValueExA / RegSetValueExA (its only registry imports) are replaced:
 * a key becomes an ini section named after the subkey, values are stored as dword:<hex> or sz:<text>.
 * RegisteredOwner is answered with [Player] Name, or the Windows user name.
 *
 * On the first start (no Settings.ini) the values of an existing install are copied over once from the
 * VirtualStore or HKLM key ([Registry] Import=0 skips that). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcpatch.h"

#define GAME_KEY  "Software\\Inner Workings\\Plane Crazy\\1.0"
#define OWNER_KEY "SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
#define MAX_KEYS  8
#define FAKE_KEY  0x7e570000u          /* fake HKEY values: FAKE_KEY + index */

static char g_file[MAX_PATH];
static char g_keys[MAX_KEYS][128];    /* open keys: the subkey names (= ini sections) */
static int g_nkeys;
static CRITICAL_SECTION g_cs;

static int key_index(HKEY key)
{
    uint32_t v = (uint32_t)(uintptr_t)key - FAKE_KEY;
    return v < (uint32_t)g_nkeys ? (int)v : -1;
}

static LONG WINAPI r_RegCreateKeyExA(HKEY root, LPCSTR sub, DWORD reserved, LPSTR cls, DWORD opt, REGSAM sam,
                                     void *sa, PHKEY out, LPDWORD disp)
{
    int i;
    (void)root; (void)reserved; (void)cls; (void)opt; (void)sam; (void)sa;
    if (!sub || !out) {
        return ERROR_INVALID_PARAMETER;
    }
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_nkeys && lstrcmpiA(g_keys[i], sub); i++) {
    }
    if (i == g_nkeys) {
        if (g_nkeys == MAX_KEYS) {
            LeaveCriticalSection(&g_cs);
            return ERROR_OUTOFMEMORY;
        }
        lstrcpynA(g_keys[g_nkeys++], sub, sizeof g_keys[0]);
    }
    LeaveCriticalSection(&g_cs);
    *out = (HKEY)(uintptr_t)(FAKE_KEY + i);
    if (disp) {
        *disp = REG_OPENED_EXISTING_KEY;
    }
    return ERROR_SUCCESS;
}

/* RegisteredOwner: [Player] Name, else the Windows user name */
static int owner_name(char *out, int size)
{
    DWORD n = size;
    if (pc_ini_str("Player", "Name", out, size)) {
        return 1;
    }
    return GetUserNameA(out, &n) && out[0];
}

static LONG WINAPI r_RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data,
                                      LPDWORD size)
{
    char text[1024];
    BYTE value[1024];
    DWORD len, t;
    int i = key_index(key);
    (void)reserved;
    if (i < 0 || !name) {
        return ERROR_INVALID_HANDLE;
    }
    if (!lstrcmpiA(g_keys[i], OWNER_KEY)) {
        if (lstrcmpiA(name, "RegisteredOwner") || !owner_name((char *)value, sizeof value)) {
            return ERROR_FILE_NOT_FOUND;
        }
        t = REG_SZ;
        len = (DWORD)strlen((char *)value) + 1;
    } else {
        GetPrivateProfileStringA(g_keys[i], name, "", text, sizeof text, g_file);
        if (!strncmp(text, "dword:", 6)) {
            DWORD v = strtoul(text + 6, NULL, 16);
            t = REG_DWORD;
            len = 4;
            memcpy(value, &v, 4);
        } else if (!strncmp(text, "sz:", 3)) {
            t = REG_SZ;
            len = (DWORD)strlen(text + 3) + 1;
            memcpy(value, text + 3, len);
        } else {
            return ERROR_FILE_NOT_FOUND;
        }
    }
    if (type) {
        *type = t;
    }
    if (data) {
        if (!size || *size < len) {
            if (size) {
                *size = len;
            }
            return ERROR_MORE_DATA;
        }
        memcpy(data, value, len);
    }
    if (size) {
        *size = len;
    }
    return ERROR_SUCCESS;
}

static void store(const char *section, const char *name, DWORD type, const BYTE *data, DWORD size)
{
    char text[1024];
    if (type == REG_DWORD && size >= 4) {
        DWORD v;
        memcpy(&v, data, 4);
        snprintf(text, sizeof text, "dword:%08lx", v);
    } else if (type == REG_SZ || type == REG_EXPAND_SZ) {
        int n = size > sizeof text - 4 ? (int)sizeof text - 4 : (int)size;
        memcpy(text, "sz:", 3);
        memcpy(text + 3, data, n);
        text[3 + n] = '\0';
    } else {
        pc_log("registry: %s: type %lu not supported, not stored", name, type);
        return;
    }
    WritePrivateProfileStringA(section, name, text, g_file);
}

static LONG WINAPI r_RegSetValueExA(HKEY key, LPCSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD size)
{
    int i = key_index(key);
    (void)reserved;
    if (i < 0 || !name) {
        return ERROR_INVALID_HANDLE;
    }
    if (lstrcmpiA(g_keys[i], OWNER_KEY)) {
        store(g_keys[i], name, type, data, size);
    }
    return ERROR_SUCCESS;
}

/* first start: copy the values an existing install left in the registry */
static void import_registry(void)
{
    static const char *const sources[] = {
        "Software\\Classes\\VirtualStore\\MACHINE\\SOFTWARE\\WOW6432Node\\Inner Workings\\Plane Crazy\\1.0",
        "Software\\Classes\\VirtualStore\\MACHINE\\SOFTWARE\\Inner Workings\\Plane Crazy\\1.0",
        GAME_KEY,
    };
    static const HKEY roots[] = { HKEY_CURRENT_USER, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    unsigned s;
    for (s = 0; s < sizeof sources / sizeof sources[0]; s++) {
        HKEY key;
        DWORD i, n = 0;
        if (RegOpenKeyExA(roots[s], sources[s], 0, KEY_READ, &key) != ERROR_SUCCESS) {
            continue;
        }
        for (i = 0;; i++) {
            char name[256];
            BYTE data[1024];
            DWORD nlen = sizeof name, dlen = sizeof data, type;
            LONG rc = RegEnumValueA(key, i, name, &nlen, NULL, &type, data, &dlen);
            if (rc == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (rc == ERROR_SUCCESS) {
                store(GAME_KEY, name, type, data, dlen);
                n++;
            }
        }
        RegCloseKey(key);
        if (n) {
            pc_log("registry: %lu settings imported from %s\\%s", n, s < 2 ? "HKCU" : "HKLM", sources[s]);
            return;
        }
    }
}

void pc_registry_install(void)
{
    const char *dir = pc_saves_dir();
    if (!pc_ini_int("Registry", "UseSettingsFile", 1) || !dir[0]) {
        return;
    }
    InitializeCriticalSection(&g_cs);
    snprintf(g_file, sizeof g_file, "%sSettings.ini", dir);
    if (GetFileAttributesA(g_file) == INVALID_FILE_ATTRIBUTES) {
        if (pc_ini_int("Registry", "Import", 1)) {
            import_registry();
        }
        if (GetFileAttributesA(g_file) == INVALID_FILE_ATTRIBUTES) {
            /* the game's defaults apply; the file marks the import as done */
            WritePrivateProfileStringA(GAME_KEY, NULL, NULL, g_file);
            WritePrivateProfileStringA("Plane Crazy", "Note", "settings of pcrazy.exe, formerly in the registry", g_file);
        }
    }
    pc_hook_import("ADVAPI32.dll", "RegCreateKeyExA", (void *)r_RegCreateKeyExA);
    pc_hook_import("ADVAPI32.dll", "RegQueryValueExA", (void *)r_RegQueryValueExA);
    pc_hook_import("ADVAPI32.dll", "RegSetValueExA", (void *)r_RegSetValueExA);
    pc_log("registry: settings in %s", g_file);
}
