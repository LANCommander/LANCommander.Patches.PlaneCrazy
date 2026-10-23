/* Frame rate limit, vertical sync and high refresh rates (docs/framerate_research.md).
 *
 * The race simulates in fixed 40 ms steps and interpolates planes and camera when drawing, and the frontend
 * scales everything by its frame time, so the game runs at any frame rate. It has no frame cap of its own: in
 * fullscreen the Flip (DDFLIP_WAIT) holds it to the refresh rate, which is whatever Windows picks for the mode,
 * because GP_DDSetDisplayMode 0x4a2324 asks IDirectDraw2::SetDisplayMode for refresh rate 0 (usually 60 Hz).
 *  - [Framerate] RefreshRate: the SetDisplayMode method is replaced (IDirectDraw2 vtable, found through the
 *    DirectDrawCreate import) to ask for the display's highest rate at the mode size, or a chosen one.
 *  - "Frame Rate Limit" (Options): waits before GP_PumpMessages in the frontend loop (call 0x4299a5) and the race
 *    loop (call 0x45d41e), right after each frame is shown.
 *  - "Vertical Sync" (Options): the Flip flags at 0x4b34c6 (mov ecx, DDFLIP_WAIT [| DDFLIP_NOVSYNC]).
 * Two things the game did once per frame are rate limited, as they assumed 25-60 fps: the force feedback spin
 * effect (IN_FF_UpdateSpin, one envelope step per call, call 0x457fe8) and the host's lobby countdown broadcast
 * (NET_pt_SetSessionTime = IDirectPlay4::SetSessionDesc, call 0x42a6a3). */
#include <stdio.h>
#include <string.h>
#include <ddraw.h>
#include "pcpatch.h"

#define FE_PUMP_CALL    0x4299a5u    /* FE_RunLoop: call GP_PumpMessages (thiscall gp) */
#define RACE_PUMP_CALL  0x45d41eu    /* RACE_Run: same */
#define GP_PUMP         0x4afc70u
#define FLIP_FLAGS      0x4b34c7u    /* imm32 of mov ecx, DDFLIP_WAIT */
#define FF_SPIN_CALL    0x457fe8u    /* RACE_RacingTick: call IN_FF_UpdateSpin (stdcall int now) */
#define FF_SPIN         0x4279b0u
#define SESSION_TIME_CALL 0x42a6a3u  /* MENU_NetFrame: call NET_pt_SetSessionTime (stdcall int sec) */
#define SESSION_TIME    0x47f780u
#define SIM_STEP        40           /* ms, race+0x34 */

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

static const int g_limits[] = { 0, 30, 60, 75, 90, 100, 120, 144, 165, 200, 240, 360 };
#define NLIMITS (int)(sizeof g_limits / sizeof g_limits[0])

static int g_max_fps;                /* 0 = no limit */
static int g_vsync = 1;
static int g_refresh;                /* [Framerate] RefreshRate: 0 = highest, -1 = Windows' choice */
static int g_id_nop, g_id_up, g_id_dn, g_id_von, g_id_voff;
static char g_settings[MAX_PATH];

static LARGE_INTEGER g_freq;
static LONGLONG g_next;
static HANDLE g_timer;

/* ---------------------------------------------------------------- frame limiter */

static LONGLONG now_qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void wait_until(LONGLONG t)
{
    LONGLONG left;
    while ((left = t - now_qpc()) > 0) {
        LONGLONG us = left * 1000000 / g_freq.QuadPart;
        if (us > 1500) {   /* sleep to ~1 ms before the target, spin the rest */
            if (g_timer) {
                LARGE_INTEGER due;
                due.QuadPart = -(us - 1000) * 10;
                SetWaitableTimer(g_timer, &due, 0, NULL, NULL, FALSE);
                WaitForSingleObject(g_timer, INFINITE);
            } else {
                Sleep((DWORD)(us - 1000) / 1000);
            }
        } else if (us > 200) {
            Sleep(0);
        }
    }
}

static void limit_frame(void)
{
    LONGLONG period, now;
    if (g_max_fps <= 0) {
        g_next = 0;
        return;
    }
    period = g_freq.QuadPart / g_max_fps;
    now = now_qpc();
    if (!g_next || now - g_next > period * 4 || g_next - now > period * 4) {
        g_next = now;   /* first frame, a long stall (loading) or a new limit: start over */
        return;
    }
    g_next += period;
    if (g_next > now) {
        wait_until(g_next);
    } else if (now - g_next > period) {
        g_next = now;   /* running behind: don't race to catch up */
    }
}

static int g_fps_log;                /* env PC_FPSLOG=1: log the frame rate every 2 s */

static void log_fps(void)
{
    static LONGLONG start;
    static int frames;
    LONGLONG now = now_qpc();
    frames++;
    if (!start || now - start > g_freq.QuadPart * 4) {
        start = now;
        frames = 0;
    } else if (now - start >= g_freq.QuadPart * 2) {
        pc_log("framerate: %.1f fps", frames * (double)g_freq.QuadPart / (now - start));
        start = now;
        frames = 0;
    }
}

static int __attribute__((fastcall)) pump_messages(void *gp, void *unused)
{
    (void)unused;
    limit_frame();
    if (g_fps_log) {
        log_fps();
    }
    return ((int (__attribute__((thiscall)) *)(void *))PC_ADDR(GP_PUMP))(gp);
}

static void set_vsync(int on)
{
    uint32_t flags = on ? DDFLIP_WAIT : DDFLIP_WAIT | DDFLIP_NOVSYNC;
    if (pc_expect(FLIP_FLAGS - 1, "\xb9", 1)) {
        pc_write(FLIP_FLAGS, &flags, 4);
    }
}

/* ---------------------------------------------------------------- refresh rate */

typedef HRESULT (WINAPI *set_display_mode_fn)(void *dd, DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags);
typedef HRESULT (WINAPI *dd_create_fn)(GUID *guid, IDirectDraw **dd, IUnknown *outer);
static set_display_mode_fn g_set_display_mode;
static dd_create_fn g_dd_create;

static DWORD pick_refresh(DWORD w, DWORD h)
{
    DEVMODEA dm;
    DWORD i, best = 0;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    for (i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++) {
        DWORD f = dm.dmDisplayFrequency;
        if (dm.dmPelsWidth != w || dm.dmPelsHeight != h || f <= 1) {
            continue;
        }
        if (g_refresh > 0 && f > (DWORD)g_refresh) {
            continue;   /* a chosen rate: that one, or the nearest below it */
        }
        if (f > best) {
            best = f;
        }
    }
    return best;
}

static HRESULT WINAPI set_display_mode(void *dd, DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags)
{
    HRESULT hr;
    if (refresh == 0 && g_refresh >= 0) {
        DWORD want = pick_refresh(w, h);
        if (want) {
            hr = g_set_display_mode(dd, w, h, bpp, want, flags);
            pc_log("framerate: display mode %lux%lux%lu at %lu Hz: %s (%08lx)", w, h, bpp, want,
                   SUCCEEDED(hr) ? "ok" : "failed, using the default rate", (unsigned long)hr);
            if (SUCCEEDED(hr)) {
                return hr;
            }
        }
    }
    return g_set_display_mode(dd, w, h, bpp, refresh, flags);
}

/* the game sets its display modes through IDirectDraw2 (GP_DDSetDisplayMode): replace SetDisplayMode in that
   interface's vtable, which every IDirectDraw2 object shares */
static HRESULT WINAPI dd_create(GUID *guid, IDirectDraw **dd, IUnknown *outer)
{
    HRESULT hr = g_dd_create(guid, dd, outer);
    IDirectDraw2 *dd2;
    if (FAILED(hr) || !dd || !*dd || g_set_display_mode) {
        return hr;
    }
    if (SUCCEEDED(IDirectDraw_QueryInterface(*dd, &IID_IDirectDraw2, (void **)&dd2))) {
        void **slot = (void **)&dd2->lpVtbl->SetDisplayMode;
        DWORD old;
        if (VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) {
            g_set_display_mode = (set_display_mode_fn)*slot;
            *slot = (void *)set_display_mode;
            VirtualProtect(slot, sizeof *slot, old, &old);
            pc_log("framerate: refresh rate %s", g_refresh > 0 ? "chosen" : "highest available");
        }
        IDirectDraw2_Release(dd2);
    }
    return hr;
}

/* ---------------------------------------------------------------- once-per-frame game code */

static int g_ff_last = -1000;

static char __attribute__((stdcall)) ff_update_spin(int now)
{
    if (now - g_ff_last < SIM_STEP && now >= g_ff_last) {
        return 1;   /* the spin envelope advances one step per call: keep it at the 25 Hz of the race */
    }
    g_ff_last = now;
    return ((char (__attribute__((stdcall)) *)(int))PC_ADDR(FF_SPIN))(now);
}

static int g_session_sec = -1;
static DWORD g_session_sent;

static int __attribute__((stdcall)) set_session_time(int sec)
{
    DWORD t = GetTickCount();
    if (sec == g_session_sec && t - g_session_sent < 1000) {
        return 1;   /* unchanged: don't broadcast the session description again every frame */
    }
    g_session_sec = sec;
    g_session_sent = t;
    return ((int (__attribute__((stdcall)) *)(int))PC_ADDR(SESSION_TIME))(sec);
}

/* ---------------------------------------------------------------- menu */

static void save_setting(const char *key, int value)
{
    char v[16];
    snprintf(v, sizeof v, "%d", value);
    WritePrivateProfileStringA("Display", key, v, g_settings);
}

static void show_limit(void)
{
    char *list = pc_menu_list("pcfps");
    if (list) {
        if (g_max_fps > 0) {
            snprintf(list + 0x30, 16, "%d fps", g_max_fps);
        } else {
            strcpy(list + 0x30, "Off");
        }
        *(int *)(list + 0x24) = 0;
    }
    pc_menu_redraw();
}

static void step_limit(int dir)
{
    int i;
    for (i = 0; i < NLIMITS && g_limits[i] != g_max_fps; i++) {
    }
    if (i == NLIMITS) {   /* a value from the settings that isn't in the list: start from the nearest below */
        for (i = NLIMITS - 1; i > 0 && g_limits[i] > g_max_fps; i--) {
        }
        if (dir < 0) {
            dir = 0;
        }
    }
    g_max_fps = g_limits[(i + dir + NLIMITS) % NLIMITS];
    g_next = 0;
    save_setting("MaxFPS", g_max_fps);
    pc_log("framerate: limit %d", g_max_fps);
    show_limit();
}

static int on_action(void *fe, int id)
{
    (void)fe;
    if (id < 0) {
        return 0;
    }
    if (id == g_id_up || id == g_id_dn) {
        step_limit(id == g_id_up ? 1 : -1);
        return 1;
    }
    if (id == g_id_nop) {
        return 1;
    }
    if (id == g_id_von || id == g_id_voff) {
        g_vsync = id == g_id_von;
        set_vsync(g_vsync);
        save_setting("VSync", g_vsync);
        pc_log("framerate: vsync %s", g_vsync ? "on" : "off");
        return 1;
    }
    return 0;
}

static void on_loaded(void)
{
    char *list;
    g_id_nop = pc_menu_id("pcfpsnop");
    g_id_up = pc_menu_id("pcfpsup");
    g_id_dn = pc_menu_id("pcfpsdn");
    g_id_von = pc_menu_id("pcvsyncon");
    g_id_voff = pc_menu_id("pcvsyncoff");
    show_limit();
    list = pc_menu_list("pcvsync");
    if (list) {
        *(int *)(list + 0x24) = !g_vsync;
    }
}

/* the menu system has room for 364 action names and the patch's menus use most of them: the limit is a 3-value
   list whose first label is rewritten (like Resolution in display.c) */
static void add_menu(void)
{
    pc_menu_add_defs("DEFOPTACTION \"pcfpsnop\"\nDEFOPTACTION \"pcfpsup\"\nDEFOPTACTION \"pcfpsdn\"\n"
                     "DEFOPTACTION \"pcvsyncon\"\nDEFOPTACTION \"pcvsyncoff\"\n");
    pc_menu_insert_before("ITEM \"Define Controls\"",
                          "ITEM \"Frame Rate Limit\" OPTION \"pcfps\"\n"
                          "ITEM \"Vertical Sync\" OPTION \"pcvsync\"\n");
    pc_menu_append("OPTIONLIST \"pcfps\"\nWRAP\n"
                   "VALUE \"Off\" OPTACTION \"pcfpsnop\"\n"   /* label = the limit, set at run time */
                   "VALUE \">\" OPTACTION \"pcfpsup\"\n"
                   "VALUE \"<\" OPTACTION \"pcfpsdn\"\n\n"
                   "OPTIONLIST \"pcvsync\"\n"
                   "VALUE \"On\" OPTACTION \"pcvsyncon\"\n"
                   "VALUE \"Off\" OPTACTION \"pcvsyncoff\"\n\n");
    pc_menu_on_action(on_action);
    pc_menu_on_loaded(on_loaded);
}

/* ---------------------------------------------------------------- install */

void pc_framerate_install(void)
{
    static const uint8_t fe_pump[5] = { 0xe8, 0xc6, 0x62, 0x08, 0x00 };
    static const uint8_t race_pump[5] = { 0xe8, 0x4d, 0x28, 0x05, 0x00 };
    static const uint8_t ff_call[5] = { 0xe8, 0xc3, 0xf9, 0xfc, 0xff };
    static const uint8_t session_call[5] = { 0xe8, 0xd8, 0x50, 0x05, 0x00 };
    if (!pc_ini_int("Framerate", "Enable", 1)) {
        return;
    }
    timeBeginPeriod(1);   /* 1 ms timeGetTime, the game's only clock (DDrawCompat happens to do this too) */
    QueryPerformanceFrequency(&g_freq);
    g_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_timer) {
        g_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    snprintf(g_settings, sizeof g_settings, "%sSettings.ini", pc_saves_dir());
    g_max_fps = (int)GetPrivateProfileIntA("Display", "MaxFPS", pc_ini_int("Framerate", "MaxFPS", 0), g_settings);
    g_vsync = (int)GetPrivateProfileIntA("Display", "VSync", pc_ini_int("Framerate", "VSync", 1), g_settings) != 0;
    g_refresh = pc_ini_int("Framerate", "RefreshRate", 0);
    g_fps_log = GetEnvironmentVariableA("PC_FPSLOG", NULL, 0) > 0;

    if (pc_expect(FE_PUMP_CALL, fe_pump, 5) && pc_expect(RACE_PUMP_CALL, race_pump, 5)) {
        pc_call(FE_PUMP_CALL, (void *)pump_messages);
        pc_call(RACE_PUMP_CALL, (void *)pump_messages);
    } else {
        pc_log("framerate: GP_PumpMessages calls differ, no frame rate limit");
    }
    if (!g_vsync) {
        set_vsync(0);
    }
    if (pc_expect(FF_SPIN_CALL, ff_call, 5)) {
        pc_call(FF_SPIN_CALL, (void *)ff_update_spin);
    }
    if (pc_expect(SESSION_TIME_CALL, session_call, 5)) {
        pc_call(SESSION_TIME_CALL, (void *)set_session_time);
    }
    if (g_refresh >= 0) {
        g_dd_create = (dd_create_fn)pc_hook_import("DDRAW.dll", "DirectDrawCreate", (void *)dd_create);
        if (!g_dd_create) {
            pc_log("framerate: DirectDrawCreate import not found, default refresh rate");
        }
    }
    if (pc_ini_int("Framerate", "MenuOptions", 1)) {
        add_menu();
    }
    pc_log("framerate: limit %d fps, vsync %s, refresh rate %s", g_max_fps, g_vsync ? "on" : "off",
           g_refresh < 0 ? "Windows default" : g_refresh ? "chosen" : "highest");
}
