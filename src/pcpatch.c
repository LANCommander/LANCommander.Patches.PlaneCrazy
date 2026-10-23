/* Core of the patch: verifies the exe, reads the settings, installs the patch modules.
 * Settings and log go to pcpatch.ini / pcpatch.log next to the DLL. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pcpatch.h"

#define PC_TIMESTAMP 0x36a5f213u   /* pcrazy.exe, Plane Crazy Championship Edition (ZOOM Platform 1.4) */
#define PC_ENTRY     0xfd140u

static char g_ini[MAX_PATH];
static char g_log[MAX_PATH];

void pc_log(const char *fmt, ...)
{
    va_list ap;
    FILE *f = fopen(g_log, "a");
    if (!f) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

int pc_write(uint32_t addr, const void *bytes, size_t len)
{
    DWORD old;
    if (!VirtualProtect(PC_ADDR(addr), len, PAGE_EXECUTE_READWRITE, &old)) {
        pc_log("VirtualProtect %08x failed (%lu)", addr, GetLastError());
        return 0;
    }
    memcpy(PC_ADDR(addr), bytes, len);
    VirtualProtect(PC_ADDR(addr), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), PC_ADDR(addr), len);
    return 1;
}

int pc_expect(uint32_t addr, const void *bytes, size_t len)
{
    return memcmp(PC_ADDR(addr), bytes, len) == 0;
}

int pc_patch(uint32_t addr, const void *expect, const void *bytes, size_t len)
{
    if (!pc_expect(addr, expect, len)) {
        pc_log("patch %08x: original bytes differ, skipped", addr);
        return 0;
    }
    return pc_write(addr, bytes, len);
}

int pc_patch32(uint32_t addr, uint32_t expect, uint32_t value)
{
    return pc_patch(addr, &expect, &value, 4);
}

static int rel32(uint32_t from, uint8_t op, const void *to)
{
    uint8_t b[5];
    int32_t rel = (int32_t)((uintptr_t)to - (from + 5));
    b[0] = op;
    memcpy(b + 1, &rel, 4);
    return pc_write(from, b, 5);
}

int pc_jmp(uint32_t from, const void *to) { return rel32(from, 0xe9, to); }
int pc_call(uint32_t from, const void *to) { return rel32(from, 0xe8, to); }

/* detour: copies the first len bytes at addr (must be whole, position-independent instructions, matching
   expect) into a trampoline that jumps back, and jumps addr to hook. Returns the trampoline (call it to run the
   original), or NULL. */
void *pc_hook(uint32_t addr, const void *expect, size_t len, void *hook)
{
    uint8_t *t;
    int32_t rel;
    if (!pc_expect(addr, expect, len)) {
        pc_log("hook %08x: original bytes differ, skipped", addr);
        return NULL;
    }
    t = VirtualAlloc(NULL, len + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) {
        return NULL;
    }
    memcpy(t, expect, len);
    t[len] = 0xe9;
    rel = (int32_t)(addr + len - ((uintptr_t)t + len + 5));
    memcpy(t + len + 1, &rel, 4);
    pc_jmp(addr, hook);
    return t;
}

int pc_ini_int(const char *section, const char *key, int def)
{
    return (int)GetPrivateProfileIntA(section, key, def, g_ini);
}

int pc_ini_str(const char *section, const char *key, char *out, int size)
{
    out[0] = '\0';
    GetPrivateProfileStringA(section, key, "", out, size, g_ini);
    return out[0] != '\0';
}

static int exe_is_supported(void)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)((const char *)dos + dos->e_lfanew);
    return (uintptr_t)dos == 0x400000 && nt->FileHeader.TimeDateStamp == PC_TIMESTAMP
        && nt->OptionalHeader.AddressOfEntryPoint == PC_ENTRY;
}

int pc_patch_start(void)
{
    if (!exe_is_supported()) {
        pc_log("pcpatch: not the supported pcrazy.exe; no patches applied");
        return 0;
    }
    if (!pc_ini_int("General", "Enabled", 1)) {
        pc_log("pcpatch: disabled in the settings");
        return 0;
    }
    {
        char name[48];
        snprintf(name, sizeof name, "Local\\pcpatch_%lu", GetCurrentProcessId());
        if (CreateMutexA(NULL, FALSE, name) && GetLastError() == ERROR_ALREADY_EXISTS) {
            pc_log("pcpatch: another copy of the patch is already loaded; this one does nothing");
            return 0;
        }
    }
    pc_log("pcpatch " PC_VERSION ": pcrazy.exe recognised, installing");
    return 1;
}

/* GAME_ReadConfig 0x412610 (fastcall game, called at 0x412e54 in GAME_Init): reads the settings into the game
   object before anything uses them; modules adjust them afterwards */
#define READ_CONFIG_CALL 0x412e54u
#define READ_CONFIG      0x412610u
static pc_config_fn g_config[4];
static int g_nconfig;

void pc_on_game_config(pc_config_fn fn)
{
    if (g_nconfig < 4) {
        g_config[g_nconfig++] = fn;
    }
}

static void __attribute__((fastcall)) read_config(char *game)
{
    int i;
    ((void (__attribute__((fastcall)) *)(char *))PC_ADDR(READ_CONFIG))(game);
    for (i = 0; i < g_nconfig; i++) {
        g_config[i](game);
    }
}

static void install_config_hook(void)
{
    static const uint8_t call_cfg[5] = { 0xe8, 0xb7, 0xf7, 0xff, 0xff };
    if (!g_nconfig) {
        return;
    }
    if (pc_expect(READ_CONFIG_CALL, call_cfg, 5)) {
        pc_call(READ_CONFIG_CALL, (void *)read_config);
    } else {
        pc_log("pcpatch: GAME_ReadConfig call differs, settings hooks off");
    }
}

void pc_install(void)
{
    pc_sound_install();
    pc_saves_install();
    pc_registry_install();
    pc_widescreen_install();
    pc_players_install();
    pc_gamepad_install();
    pc_display_install();
    pc_net_install();
    pc_framerate_install();
    pc_menu_install();     /* after the modules that add to the menus */
    install_config_hook();
    pc_trace_install();   /* last: it logs the calls as the game makes them */
}

void pc_set_files(const char *ini, const char *log)
{
    lstrcpynA(g_ini, ini, MAX_PATH);
    lstrcpynA(g_log, log, MAX_PATH);
}
