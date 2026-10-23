/* Resolution and colour depth in the Options menu (docs/graphics_options_research.md).
 *
 * The video mode comes from the -s setup dialog (iStartRes = index into its 16-bit mode list), copied by
 * GAME_ReadConfig 0x412610 into game+0x158/0x15c/0x160/0x164 (width, height, bpp, "stretched x 2" shift).
 * FE_InitGraphics and RACE_SetupGraphics call GP_SetMode with those fields every time a scene starts, and
 * GP_SetMode rebuilds the display (buffers, D3D device, textures, pictures) when they changed. So a new resolution
 * picked in the menu is written there and takes effect at the next scene (race start, or back to the menus).
 * It is kept in Settings.ini [Display] (iStartRes too, for the setup dialog).
 *
 * Colour depth: everything on the hardware path follows the display format; only two things keep the game at
 * 16 bits: game+0x160 (always 16) and the display mode enumeration (the graphport's own list, 40 entries max,
 * which ZoomFix0.dll filters to 16-bit modes by replacing the callback at 0x4a39a6). Applied at the next start.
 * The software renderer is 16-bit only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ddraw.h>
#include "pcpatch.h"

#define ENUM_CB_IMM      0x4a39a6u   /* mov ebx, <display mode enumeration callback> in GP_InitDDraw */
#define ENUM_CB          0x4a00f0u   /* original callback (stdcall desc, graphport) */
#define GAME_PTR         0x523184u

#define G_WIDTH   0x158
#define G_HEIGHT  0x15c
#define G_BPP     0x160              /* 0 = windowed */
#define G_SHIFT   0x164
#define G_GP      0x168
#define G_HW      0x280              /* 1 = hardware renderer */
#define GP_NMODES 0xc4a4
#define GP_MODES  0xc4ac             /* {w, h, bpp, valid} x 40 */

#define SETUP_DEVICES 0x523880u      /* -s dialog device table, 0x335c bytes per device */
#define SETUP_DEVICE  0x558f60u      /* chosen device */
#define GAME_KEY  "Software\\Inner Workings\\Plane Crazy\\1.0"

static int g_bpp_want;               /* 16 or 32: the depth enumerated and used */
static int g_bpp_saved;              /* the depth the menu shows (next start) */
static int g_hw, g_full;
static int g_cur_w, g_cur_h;         /* the resolution the menu shows */
static int g_id_nop, g_id_up, g_id_dn, g_id_16, g_id_32, g_id_full, g_id_hw;
static char g_settings[MAX_PATH];

static char *game(void) { return *(char **)PC_ADDR(GAME_PTR); }

static int setting(const char *key)
{
    return (int)GetPrivateProfileIntA("Display", key, 0, g_settings);
}

static void save_setting(const char *key, int value)
{
    char v[16];
    snprintf(v, sizeof v, "%d", value);
    WritePrivateProfileStringA("Display", key, v, g_settings);
}

/* index of w x h (not stretched) in the setup dialog's mode list, or -1 */
static int setup_mode_index(int w, int h)
{
    char *dev = (char *)PC_ADDR(SETUP_DEVICES) + *(int *)PC_ADDR(SETUP_DEVICE) * 0x335c;
    int i, n = *(int *)(dev + 0x3354);
    for (i = 0; i < n && i < 0x100; i++) {
        int *m = (int *)(dev + 0x2354 + i * 16);
        if (m[0] == w && m[1] == h && m[3] == 0) {
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- startup */

static HRESULT WINAPI enum_mode(DDSURFACEDESC *d, void *gp)
{
    if ((int)d->ddpfPixelFormat.dwRGBBitCount != g_bpp_want || d->dwWidth < 640 || d->dwHeight < 480) {
        return DDENUMRET_OK;
    }
    return ((HRESULT (WINAPI *)(DDSURFACEDESC *, void *))PC_ADDR(ENUM_CB))(d, gp);
}

static void after_config(char *g)
{
    int w, h, bpp;
    g_hw = *(int *)(g + G_HW) != 0;
    g_full = *(int *)(g + G_BPP) != 0;
    w = setting("Width");
    h = setting("Height");
    bpp = setting("BitDepth");
    g_bpp_saved = bpp == 32 ? 32 : 16;
    g_bpp_want = g_full ? *(int *)(g + G_BPP) : 16;
    if (g_full && w > 0 && h > 0) {
        if (setup_mode_index(w, h) >= 0) {
            *(int *)(g + G_WIDTH) = w;
            *(int *)(g + G_HEIGHT) = h;
            *(int *)(g + G_SHIFT) = 0;
        } else {
            pc_log("display: %dx%d is not an available mode, keeping the setup's", w, h);
        }
    }
    if (g_full && g_hw && bpp == 32) {
        *(int *)(g + G_BPP) = g_bpp_want = 32;
    }
    if (g_full) {
        uint32_t cb;
        memcpy(&cb, PC_ADDR(ENUM_CB_IMM), 4);
        if (pc_expect(ENUM_CB_IMM - 1, "\xbb", 1)) {
            uint32_t mine = (uint32_t)(uintptr_t)enum_mode;
            pc_write(ENUM_CB_IMM, &mine, 4);   /* also replaces ZoomFix0's 16-bit filter */
            pc_log("display: mode list filter %d bit (was callback %08x)", g_bpp_want, cb);
        }
    }
    g_cur_w = *(int *)(g + G_WIDTH) << *(int *)(g + G_SHIFT);
    g_cur_h = *(int *)(g + G_HEIGHT) << *(int *)(g + G_SHIFT);
    pc_log("display: %dx%dx%d%s", g_cur_w, g_cur_h, *(int *)(g + G_BPP), g_hw ? "" : " (software renderer)");
}

/* ---------------------------------------------------------------- menu */

static int mode_cmp(const void *a, const void *b)
{
    const int *x = a, *y = b;
    return x[0] * x[1] != y[0] * y[1] ? x[0] * x[1] - y[0] * y[1] : x[0] - y[0];
}

/* the graphport's usable modes at the current depth, ascending, as {w, h} pairs; returns the count */
static int list_modes(int out[][2], int max)
{
    char *g = game(), *gp = g ? *(char **)(g + G_GP) : NULL;
    int i, j, n = 0, count;
    if (!gp) {
        return 0;
    }
    count = *(int *)(gp + GP_NMODES);
    for (i = 0; i < count && i < 40; i++) {
        int *m = (int *)(gp + GP_MODES + i * 16);
        if (!m[3] || m[2] != *(int *)(g + G_BPP) || m[0] < 640) {
            continue;
        }
        for (j = 0; j < n && (out[j][0] != m[0] || out[j][1] != m[1]); j++) {
        }
        if (j == n && n < max) {
            out[n][0] = m[0];
            out[n][1] = m[1];
            n++;
        }
    }
    qsort(out, n, sizeof out[0], mode_cmp);
    return n;
}

static void show_resolution(void)
{
    char *list = pc_menu_list("pcres");
    if (list) {
        snprintf(list + 0x30, 16, "%dx%d", g_cur_w, g_cur_h);
        *(int *)(list + 0x24) = 0;
    }
    pc_menu_redraw();
}

static void step_resolution(int dir)
{
    int modes[40][2], n = list_modes(modes, 40), i, idx;
    char *g = game();
    if (!n || !g) {
        show_resolution();
        return;
    }
    for (idx = 0; idx < n && (modes[idx][0] != g_cur_w || modes[idx][1] != g_cur_h); idx++) {
    }
    if (idx == n) {   /* current mode not in the list: start from the nearest smaller one */
        for (idx = 0, i = 0; i < n; i++) {
            if (modes[i][0] * modes[i][1] <= g_cur_w * g_cur_h) {
                idx = i;
            }
        }
    } else {
        idx = (idx + dir + n) % n;
    }
    g_cur_w = modes[idx][0];
    g_cur_h = modes[idx][1];
    *(int *)(g + G_WIDTH) = g_cur_w;    /* GP_SetMode picks it up at the next scene start */
    *(int *)(g + G_HEIGHT) = g_cur_h;
    *(int *)(g + G_SHIFT) = 0;
    save_setting("Width", g_cur_w);
    save_setting("Height", g_cur_h);
    i = setup_mode_index(g_cur_w, g_cur_h);
    if (i >= 0) {   /* keep the setup dialog in step (Settings.ini is the game's registry, registry.c) */
        char v[24];
        snprintf(v, sizeof v, "dword:%08x", i);
        WritePrivateProfileStringA(GAME_KEY, "iStartRes", v, g_settings);
    }
    pc_log("display: resolution %dx%d (from the next race or menu screen)", g_cur_w, g_cur_h);
    show_resolution();
}

static int on_action(void *fe, int id)
{
    (void)fe;
    if (id < 0) {
        return 0;
    }
    if (id == g_id_up || id == g_id_dn) {
        step_resolution(id == g_id_up ? 1 : -1);
        return 1;
    }
    if (id == g_id_nop) {
        return 1;
    }
    if (id == g_id_16 || id == g_id_32) {
        g_bpp_saved = id == g_id_32 ? 32 : 16;
        save_setting("BitDepth", g_bpp_saved);
        pc_log("display: %d bit colour from the next start", g_bpp_saved);
        return 1;
    }
    return 0;
}

static int on_condition(int id, int *result)
{
    if (id >= 0 && id == g_id_full) {
        *result = g_full;
        return 1;
    }
    if (id >= 0 && id == g_id_hw) {
        *result = g_full && g_hw;
        return 1;
    }
    return 0;
}

static void on_loaded(void)
{
    char *list;
    g_id_nop = pc_menu_id("pcresnop");
    g_id_up = pc_menu_id("pcresup");
    g_id_dn = pc_menu_id("pcresdn");
    g_id_16 = pc_menu_id("pcbpp16");
    g_id_32 = pc_menu_id("pcbpp32");
    g_id_full = pc_menu_id("pcisfull");
    g_id_hw = pc_menu_id("pcishw");
    show_resolution();
    list = pc_menu_list("pcbpp");
    if (list) {
        *(int *)(list + 0x24) = g_bpp_saved == 32;
    }
}

void pc_display_install(void)
{
    if (!pc_ini_int("Display", "MenuOptions", 1)) {
        return;
    }
    snprintf(g_settings, sizeof g_settings, "%sSettings.ini", pc_saves_dir());
    pc_on_game_config(after_config);
    pc_menu_add_defs("DEFOPTACTION \"pcresnop\"\nDEFOPTACTION \"pcresup\"\nDEFOPTACTION \"pcresdn\"\n"
                     "DEFOPTACTION \"pcbpp16\"\nDEFOPTACTION \"pcbpp32\"\n"
                     "DEFCONDVISACTION \"pcisfull\"\nDEFCONDVISACTION \"pcishw\"\n");
    pc_menu_insert_before("ITEM \"Define Controls\"",
                          "ITEM \"Resolution\" OPTION \"pcres\" CONDITIONALVIS \"pcisfull\"\n"
                          "ITEM \"Colour Depth\" OPTION \"pcbpp\" CONDITIONALVIS \"pcishw\"\n");
    pc_menu_append("OPTIONLIST \"pcres\"\nWRAP\n"
                   "VALUE \"640x480\" OPTACTION \"pcresnop\"\n"   /* label = the chosen mode, set at run time */
                   "VALUE \">\" OPTACTION \"pcresup\"\n"
                   "VALUE \"<\" OPTACTION \"pcresdn\"\n\n"
                   "OPTIONLIST \"pcbpp\"\n"
                   "VALUE \"16 bit\" OPTACTION \"pcbpp16\"\n"
                   "VALUE \"32 bit\" OPTACTION \"pcbpp32\"\n\n");
    pc_menu_on_action(on_action);
    pc_menu_on_condition(on_condition);
    pc_menu_on_loaded(on_loaded);
    pc_log("display: resolution and colour depth in the Options menu");
}
