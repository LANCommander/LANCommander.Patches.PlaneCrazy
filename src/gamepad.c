/* Gamepads through SDL2, with rumble (docs/gamepad.md, research in docs/gamepad_research.md).
 *
 * The game uses one DirectInput 5 joystick: IN_SetupJoystick 0x43f0f0 enumerates DIDEVTYPE_JOYSTICK devices and
 * takes the first, reads a DIJOYSTATE (c_dfDIJoystick) every frame (IN_PollJoystick 0x43f420: X = roll, Y = pitch,
 * buttons numbered from 1), and drives force feedback effects on it when the device reports DIDC_FORCEFEEDBACK
 * (spring centring, collisions, cannon, hits, buffeting).
 *
 * The IDirectInputA the game creates (dinput_proxy.c) is wrapped: its joystick enumeration offers an SDL gamepad
 * first, as a fake IDirectInputDevice2A, and hides the DirectInput devices SDL already handles. Real joysticks
 * and wheels come first instead when no SDL gamepad is present but they are (they keep working as before).
 * The fake device follows the first SDL gamepad present (hotplug, at any time), reports 2 axes and 12 buttons
 * ("gamepad mode": throttle on buttons, gamepad.map):
 *   X/Y = left stick (D-pad = full deflection); buttons 1 A, 2 B, 3 X, 4 Y, 5 LB, 6 RB, 7 RT, 8 LT, 9 Start,
 *   10 Back, 11 LS, 12 RS. The game's defaults (options iOptionJoy*) then give: A fire powerup / menu select,
 *   B cannon / menu back, X air brakes, RT accelerate, LT brake, Start quit race (gamepad.map; during a race it is also sent as
 *   Escape, which opens the in-flight menu); Y cockpit view
 *   (the shipped gamepad.map has none: set after IN_LoadJoystickMap if still unbound).
 * Effects become SDL_GameControllerRumble: constant/ramp forces drive the low-frequency motor, periodic ones the
 * high-frequency motor; conditions (spring, inertia, damper, friction) are ignored. The in-game force feedback
 * strength option (effect/device gain) applies, and [Gamepad] Rumble scales everything.
 *
 * SDL runs on the game thread: initialised in the game's EnumDevices call, pumped from the device's Poll. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#define DIRECTINPUT_VERSION 0x0500
#include <dinput.h>
#include <stdio.h>
#include <string.h>
#include "pcpatch.h"

#define PAD_BUTTONS 12
#define LOAD_MAP_CALL 0x43e3bau          /* call IN_LoadJoystickMap (thiscall, ecx = input object) */
#define LOAD_MAP      0x43e3d0u
#define BIND_COCKPIT  0x49c              /* input object: COCKPIT_VIEW button (-1 = unbound) */
#define RACE_TICK_CALL 0x45d416u         /* call RACE_Tick 0x458400 (fastcall, ecx = race), once per race frame */
#define RACE_TICK     0x458400u

/* {a3c2b1d0-5043-4b5a-9e1f-504350414430}: "PCPAD0" */
static const GUID PAD_GUID = { 0xa3c2b1d0, 0x5043, 0x4b5a, { 0x9e, 0x1f, 0x50, 0x43, 0x50, 0x41, 0x44, 0x30 } };
static const GUID PAD_PRODUCT = { 0xa3c2b1d1, 0x5043, 0x4b5a, { 0x9e, 0x1f, 0x50, 0x43, 0x50, 0x41, 0x44, 0x30 } };
static const GUID IID_DIDevice2A = { 0x5944e682, 0xc92e, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0 } };
static const GUID IID_DIDeviceA = { 0x5944e680, 0xc92e, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0 } };
static const GUID IID_DIEffect = { 0xe7e1f7c0, 0x88d2, 0x11d0, { 0x9a, 0xd0, 0x00, 0xa0, 0xc9, 0xa0, 0x6e, 0x35 } };
static const GUID IID_Unknown = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

static const SDL_GameControllerButton BUTTON_MAP[PAD_BUTTONS] = {
    SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_BUTTON_INVALID,   /* 7 RT, 8 LT: triggers */
    SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
};

static int g_deadzone = 15;              /* percent; the game asks for 15 (DIPROP_DEADZONE 1500) */
static int g_rumble = 100;               /* percent */
static int g_invert_y;
static int g_cockpit_button = 4;
static int g_start_menu = 1;             /* Start in a race = Escape (in-flight menu) */
static DWORD g_race_tick;                /* GetTickCount of the last race frame */
static int g_start_was_down;
static int g_sdl;                      /* 1 = up, -1 = failed */
static SDL_GameController *g_gc;
static SDL_JoystickID g_gc_id = -1;

/* ---------------------------------------------------------------- SDL */

static int g_vpad = -1;

static void pick_pad(void)
{
    int i, n = SDL_NumJoysticks();
    for (i = 0; i < n && !g_gc; i++) {
        if (g_vpad >= 0 && i != g_vpad) {
            continue;   /* the test pad, when attached, is the one used */
        }
        if (SDL_IsGameController(i)) {
            g_gc = SDL_GameControllerOpen(i);
            if (g_gc) {
                g_gc_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_gc));
                pc_log("gamepad: using \"%s\"", SDL_GameControllerName(g_gc));
            }
        }
    }
}

/* ---- test aid: env PC_VPAD=1 attaches a virtual SDL gamepad, driven by <exe dir>\padcmd.txt
   ("a <SDL axis> <value>" / "b <SDL button> <0|1>" lines, consumed on the next poll); its rumble is logged */
static SDL_Joystick *g_vpad_js;

static int SDLCALL vpad_rumble(void *user, Uint16 low, Uint16 high)
{
    (void)user;
    pc_log("vpad: rumble low %u high %u", low, high);
    return 0;
}

static void vpad_attach(void)
{
    SDL_VirtualJoystickDesc d;
    char v[8];
    if (!GetEnvironmentVariableA("PC_VPAD", v, sizeof v)) {
        return;
    }
    memset(&d, 0, sizeof d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.name = "Virtual test pad";
    d.Rumble = vpad_rumble;
    g_vpad = SDL_JoystickAttachVirtualEx(&d);
    g_vpad_js = g_vpad >= 0 ? SDL_JoystickOpen(g_vpad) : NULL;
    pc_log("vpad: attached (%d)", g_vpad);
}

static void vpad_commands(void)
{
    char path[MAX_PATH], line[64], *slash;
    FILE *f;
    if (!g_vpad_js) {
        return;
    }
    GetModuleFileNameA(NULL, path, MAX_PATH);
    slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "padcmd.txt");
    f = fopen(path, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof line, f)) {
        int i, val;
        char c;
        if (sscanf(line, " %c %d %d", &c, &i, &val) == 3) {
            if (c == 'a') {
                SDL_JoystickSetVirtualAxis(g_vpad_js, i, (Sint16)val);
            } else if (c == 'b') {
                SDL_JoystickSetVirtualButton(g_vpad_js, i, (Uint8)val);
            }
        }
    }
    fclose(f);
    DeleteFileA(path);
}

static void sdl_init(void)
{
    if (g_sdl) {
        return;
    }
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0) {
        pc_log("gamepad: SDL_Init failed: %s", SDL_GetError());
        g_sdl = -1;
        return;
    }
    g_sdl = 1;
    vpad_attach();
    SDL_PumpEvents();
    pick_pad();
}

static void sdl_pump(void)
{
    SDL_Event ev;
    if (g_sdl != 1) {
        return;
    }
    vpad_commands();
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_CONTROLLERDEVICEREMOVED && g_gc && ev.cdevice.which == g_gc_id) {
            pc_log("gamepad: unplugged");
            SDL_GameControllerClose(g_gc);
            g_gc = NULL;
            g_gc_id = -1;
        }
    }
    if (!g_gc) {
        pick_pad();     /* also picks up a pad plugged in later */
    }
}

/* SDL gamepads by USB vendor/product, as DirectInput puts them in guidProduct.Data1 (MAKELONG(vid, pid)) */
static int sdl_handles(const GUID *product)
{
    int i, n = g_sdl == 1 ? SDL_NumJoysticks() : 0;
    for (i = 0; i < n; i++) {
        if (SDL_IsGameController(i)) {
            DWORD id = (DWORD)SDL_JoystickGetDeviceVendor(i) | ((DWORD)SDL_JoystickGetDeviceProduct(i) << 16);
            if (id && id == product->Data1) {
                return 1;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- effects -> rumble */

enum { FX_NONE, FX_LOW, FX_HIGH };

typedef struct effect {
    IDirectInputEffectVtbl *vtbl;
    LONG refs;
    struct effect *next;
    int kind;
    double magnitude;          /* 0..1 */
    double gain;               /* 0..1 */
    DWORD duration;            /* us, INFINITE */
    int active;
    DWORD until;               /* GetTickCount end; 0 = no end */
} effect_t;

static effect_t *g_effects;
static double g_device_gain = 1.0;
static DWORD g_rumble_sent;
static Uint16 g_low_sent, g_high_sent;

static int effect_kind(const GUID *g)
{
    if (g->Data2 != 0x8e33 || (g->Data1 & ~0xfu) != 0x13541c20) {
        return FX_NONE;
    }
    switch (g->Data1 & 0xf) {
    case 0: case 1: return FX_LOW;                  /* ConstantForce, RampForce */
    case 2: case 3: case 4: case 5: case 6: return FX_HIGH;   /* Square, Sine, Triangle, SawtoothUp/Down */
    default: return FX_NONE;                        /* Spring, Damper, Inertia, Friction, CustomForce */
    }
}

static void effect_params(effect_t *e, const DIEFFECT *p, DWORD flags)
{
    if (flags & DIEP_DURATION) {
        e->duration = p->dwDuration;
    }
    if (flags & DIEP_GAIN) {
        e->gain = p->dwGain / 10000.0;
    }
    if ((flags & DIEP_TYPESPECIFICPARAMS) && p->lpvTypeSpecificParams) {
        double m = 0;
        if (e->kind == FX_LOW && p->cbTypeSpecificParams >= sizeof(DIRAMPFORCE)) {
            const DIRAMPFORCE *r = p->lpvTypeSpecificParams;
            m = labs(r->lStart) > labs(r->lEnd) ? labs(r->lStart) : labs(r->lEnd);
        } else if (e->kind == FX_LOW && p->cbTypeSpecificParams >= sizeof(DICONSTANTFORCE)) {
            m = labs(((const DICONSTANTFORCE *)p->lpvTypeSpecificParams)->lMagnitude);
        } else if (e->kind == FX_HIGH && p->cbTypeSpecificParams >= sizeof(DIPERIODIC)) {
            m = ((const DIPERIODIC *)p->lpvTypeSpecificParams)->dwMagnitude;
        }
        e->magnitude = m > 10000 ? 1.0 : m / 10000.0;
    }
}

static void rumble_update(void)
{
    double low = 0, high = 0, scale = g_device_gain * g_rumble / 100.0;
    DWORD now = GetTickCount();
    effect_t *e;
    Uint16 l, h;
    for (e = g_effects; e; e = e->next) {
        if (e->active && e->until && (LONG)(now - e->until) >= 0) {
            e->active = 0;
        }
        if (e->active) {
            double v = e->magnitude * e->gain * scale;
            if (e->kind == FX_LOW) {
                low += v;
            } else if (e->kind == FX_HIGH) {
                high += v;
            }
        }
    }
    l = (Uint16)(low >= 1 ? 0xffff : low * 0xffff);
    h = (Uint16)(high >= 1 ? 0xffff : high * 0xffff);
    if (!g_gc || (l == g_low_sent && h == g_high_sent && now - g_rumble_sent < 500)) {
        return;
    }
    SDL_GameControllerRumble(g_gc, l, h, l || h ? 1000 : 0);
    g_low_sent = l;
    g_high_sent = h;
    g_rumble_sent = now;
}

static HRESULT WINAPI fx_QueryInterface(IDirectInputEffect *self, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_DIEffect) || IsEqualGUID(iid, &IID_Unknown)) {
        *out = self;
        ((effect_t *)self)->refs++;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI fx_AddRef(IDirectInputEffect *self) { return ++((effect_t *)self)->refs; }

static ULONG WINAPI fx_Release(IDirectInputEffect *self)
{
    effect_t *e = (effect_t *)self, **p;
    if (--e->refs) {
        return e->refs;
    }
    for (p = &g_effects; *p; p = &(*p)->next) {
        if (*p == e) {
            *p = e->next;
            break;
        }
    }
    free(e);
    return 0;
}

static HRESULT WINAPI fx_Initialize(IDirectInputEffect *self, HINSTANCE i, DWORD v, REFGUID g)
{
    (void)self; (void)i; (void)v; (void)g;
    return DI_OK;
}

static HRESULT WINAPI fx_GetEffectGuid(IDirectInputEffect *self, LPGUID g)
{
    (void)self;
    memset(g, 0, sizeof *g);
    return DI_OK;
}

static HRESULT WINAPI fx_GetParameters(IDirectInputEffect *self, LPDIEFFECT p, DWORD flags)
{
    effect_t *e = (effect_t *)self;
    if (flags & DIEP_DURATION) {
        p->dwDuration = e->duration;
    }
    if (flags & DIEP_GAIN) {
        p->dwGain = (DWORD)(e->gain * 10000);
    }
    return DI_OK;
}

static HRESULT WINAPI fx_SetParameters(IDirectInputEffect *self, LPCDIEFFECT p, DWORD flags)
{
    effect_t *e = (effect_t *)self;
    if (p) {
        effect_params(e, p, flags);
    }
    if (flags & DIEP_START) {
        e->active = 1;
        e->until = e->duration == INFINITE ? 0 : GetTickCount() + e->duration / 1000 + 1;
    }
    return DI_OK;
}

static HRESULT WINAPI fx_Start(IDirectInputEffect *self, DWORD iterations, DWORD flags)
{
    effect_t *e = (effect_t *)self;
    (void)flags;
    e->active = 1;
    e->until = e->duration == INFINITE || iterations == INFINITE ? 0
        : GetTickCount() + (DWORD)((double)e->duration / 1000 * (iterations ? iterations : 1)) + 1;
    return DI_OK;
}

static HRESULT WINAPI fx_Stop(IDirectInputEffect *self)
{
    ((effect_t *)self)->active = 0;
    return DI_OK;
}

static HRESULT WINAPI fx_GetEffectStatus(IDirectInputEffect *self, LPDWORD status)
{
    *status = ((effect_t *)self)->active ? DIEGES_PLAYING : 0;
    return DI_OK;
}

static HRESULT WINAPI fx_Download(IDirectInputEffect *self) { (void)self; return DI_OK; }
static HRESULT WINAPI fx_Unload(IDirectInputEffect *self) { (void)self; return DI_OK; }
static HRESULT WINAPI fx_Escape(IDirectInputEffect *self, LPDIEFFESCAPE e) { (void)self; (void)e; return DIERR_UNSUPPORTED; }

static IDirectInputEffectVtbl g_fx_vtbl = {
    fx_QueryInterface, fx_AddRef, fx_Release, fx_Initialize, fx_GetEffectGuid, fx_GetParameters,
    fx_SetParameters, fx_Start, fx_Stop, fx_GetEffectStatus, fx_Download, fx_Unload, fx_Escape,
};

/* ---------------------------------------------------------------- fake device */

typedef struct {
    IDirectInputDevice2AVtbl *vtbl;
    LONG refs;
    LONG range_min[3], range_max[3];     /* X, Y, Z (DIPROP_RANGE by offset) */
    DWORD deadzone[3];                   /* 0..10000 */
} pad_dev_t;

static pad_dev_t g_dev;

static HRESULT WINAPI dev_QueryInterface(IDirectInputDevice2A *self, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_DIDevice2A) || IsEqualGUID(iid, &IID_DIDeviceA) || IsEqualGUID(iid, &IID_Unknown)) {
        *out = self;
        ((pad_dev_t *)self)->refs++;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dev_AddRef(IDirectInputDevice2A *self) { return ++((pad_dev_t *)self)->refs; }
static ULONG WINAPI dev_Release(IDirectInputDevice2A *self) { return --((pad_dev_t *)self)->refs; }

static HRESULT WINAPI dev_GetCapabilities(IDirectInputDevice2A *self, LPDIDEVCAPS caps)
{
    DWORD size = caps->dwSize;
    (void)self;
    memset(caps, 0, size);
    caps->dwSize = size;
    caps->dwFlags = DIDC_ATTACHED | DIDC_FORCEFEEDBACK;
    caps->dwDevType = DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8);
    caps->dwAxes = 2;
    caps->dwButtons = PAD_BUTTONS;
    caps->dwPOVs = 0;
    return DI_OK;
}

static HRESULT WINAPI dev_EnumObjects(IDirectInputDevice2A *self, LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags)
{
    (void)self; (void)cb; (void)ref; (void)flags;
    return DI_OK;
}

static int prop_axis(LPCDIPROPHEADER h)
{
    if (h->dwHow == DIPH_BYOFFSET && h->dwObj <= 8 && h->dwObj % 4 == 0) {
        return (int)h->dwObj / 4;
    }
    return h->dwHow == DIPH_DEVICE ? -1 : -2;
}

static HRESULT WINAPI dev_GetProperty(IDirectInputDevice2A *self, REFGUID prop, LPDIPROPHEADER h)
{
    pad_dev_t *d = (pad_dev_t *)self;
    int a = prop_axis(h);
    if ((uintptr_t)prop == (uintptr_t)DIPROP_RANGE && a >= 0) {
        ((LPDIPROPRANGE)h)->lMin = d->range_min[a];
        ((LPDIPROPRANGE)h)->lMax = d->range_max[a];
        return DI_OK;
    }
    if ((uintptr_t)prop == (uintptr_t)DIPROP_DEADZONE && a >= 0) {
        ((LPDIPROPDWORD)h)->dwData = d->deadzone[a];
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI dev_SetProperty(IDirectInputDevice2A *self, REFGUID prop, LPCDIPROPHEADER h)
{
    pad_dev_t *d = (pad_dev_t *)self;
    int a = prop_axis(h), i;
    for (i = 0; i < 3; i++) {
        if (a != -1 && a != i) {
            continue;
        }
        if ((uintptr_t)prop == (uintptr_t)DIPROP_RANGE) {
            d->range_min[i] = ((LPCDIPROPRANGE)h)->lMin;
            d->range_max[i] = ((LPCDIPROPRANGE)h)->lMax;
        } else if ((uintptr_t)prop == (uintptr_t)DIPROP_DEADZONE) {
            d->deadzone[i] = ((LPCDIPROPDWORD)h)->dwData;
        }
    }
    if ((uintptr_t)prop == (uintptr_t)DIPROP_FFGAIN) {
        g_device_gain = ((LPCDIPROPDWORD)h)->dwData / 10000.0;
    }
    return DI_OK;
}

static HRESULT WINAPI dev_Acquire(IDirectInputDevice2A *self) { (void)self; return DI_OK; }
static HRESULT WINAPI dev_Unacquire(IDirectInputDevice2A *self) { (void)self; return DI_OK; }

/* stick value -32768..32767 -> -1..1 with a dead zone (pct of full scale), rescaled so it starts at 0 */
static double stick(Sint16 raw, DWORD dz)
{
    double v = raw / 32767.0, z = dz / 10000.0;
    double m = v < 0 ? -v : v;
    if (m > 1) {
        m = 1;
    }
    if (m <= z) {
        return 0;
    }
    m = (m - z) / (1 - z);
    return v < 0 ? -m : m;
}

static LONG to_range(pad_dev_t *d, int axis, double v)   /* v -1..1 */
{
    double lo = d->range_min[axis], hi = d->range_max[axis];
    return (LONG)(lo + (v + 1) / 2 * (hi - lo) + (v >= 0 ? 0.5 : -0.5));
}

/* Start during a race opens (and closes) the in-flight menu, which the game only opens on Escape (RACE_Tick
   0x458585 / 0x465860 test the last key, [input+0x3fc] == VK_ESCAPE): send the game window an Escape press. */
static BOOL CALLBACK find_game_window(HWND w, LPARAM out)
{
    if (IsWindowVisible(w)) {
        *(HWND *)out = w;
        return FALSE;
    }
    return TRUE;
}

static void start_button(int down)
{
    static int sent;
    HWND w = NULL;
    if (down == g_start_was_down) {
        return;
    }
    g_start_was_down = down;
    if (down && GetTickCount() - g_race_tick > 250) {
        return;   /* not in a race: Start keeps its QUIT_RACE binding only */
    }
    if (!down && !sent) {
        return;
    }
    EnumThreadWindows(GetCurrentThreadId(), find_game_window, (LPARAM)&w);
    if (w) {
        PostMessageA(w, down ? WM_KEYDOWN : WM_KEYUP, VK_ESCAPE, down ? 0x00010001 : 0xc0010001);
    }
    sent = down;
}

typedef void (__attribute__((fastcall)) *race_tick_fn)(void *race);

static void __attribute__((fastcall)) race_tick(void *race)
{
    g_race_tick = GetTickCount();
    ((race_tick_fn)PC_ADDR(RACE_TICK))(race);
}

static HRESULT WINAPI dev_GetDeviceState(IDirectInputDevice2A *self, DWORD size, LPVOID out)
{
    pad_dev_t *d = (pad_dev_t *)self;
    DIJOYSTATE st;
    double x = 0, y = 0;
    int i;
    memset(&st, 0, sizeof st);
    if (g_gc) {
        DWORD dzx = d->deadzone[0] > (DWORD)g_deadzone * 100 ? d->deadzone[0] : (DWORD)g_deadzone * 100;
        DWORD dzy = d->deadzone[1] > (DWORD)g_deadzone * 100 ? d->deadzone[1] : (DWORD)g_deadzone * 100;
        x = stick(SDL_GameControllerGetAxis(g_gc, SDL_CONTROLLER_AXIS_LEFTX), dzx);
        y = stick(SDL_GameControllerGetAxis(g_gc, SDL_CONTROLLER_AXIS_LEFTY), dzy);
        if (SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) x = -1;
        if (SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) x = 1;
        if (SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_DPAD_UP)) y = -1;
        if (SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) y = 1;
        if (g_invert_y) {
            y = -y;
        }
        for (i = 0; i < PAD_BUTTONS; i++) {
            int down;
            if (i == 6) {
                down = SDL_GameControllerGetAxis(g_gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000;
            } else if (i == 7) {
                down = SDL_GameControllerGetAxis(g_gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000;
            } else {
                down = SDL_GameControllerGetButton(g_gc, BUTTON_MAP[i]);
            }
            st.rgbButtons[i] = down ? 0x80 : 0;
        }
        if (g_start_menu) {
            start_button(SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_START));
        }
    }
    st.lX = to_range(d, 0, x);
    st.lY = to_range(d, 1, y);
    st.lZ = to_range(d, 2, 0);
    st.rgdwPOV[0] = st.rgdwPOV[1] = st.rgdwPOV[2] = st.rgdwPOV[3] = (DWORD)-1;
    memcpy(out, &st, size < sizeof st ? size : sizeof st);
    return DI_OK;
}

static HRESULT WINAPI dev_GetDeviceData(IDirectInputDevice2A *self, DWORD cb, LPDIDEVICEOBJECTDATA data, LPDWORD n, DWORD flags)
{
    (void)self; (void)cb; (void)data; (void)flags;
    *n = 0;
    return DI_OK;
}

static HRESULT WINAPI dev_SetDataFormat(IDirectInputDevice2A *self, LPCDIDATAFORMAT f) { (void)self; (void)f; return DI_OK; }
static HRESULT WINAPI dev_SetEventNotification(IDirectInputDevice2A *self, HANDLE h) { (void)self; (void)h; return DI_OK; }
static HRESULT WINAPI dev_SetCooperativeLevel(IDirectInputDevice2A *self, HWND w, DWORD f) { (void)self; (void)w; (void)f; return DI_OK; }

static HRESULT WINAPI dev_GetObjectInfo(IDirectInputDevice2A *self, LPDIDEVICEOBJECTINSTANCEA o, DWORD obj, DWORD how)
{
    (void)self; (void)o; (void)obj; (void)how;
    return DIERR_OBJECTNOTFOUND;
}

static void fill_instance(LPDIDEVICEINSTANCEA di)
{
    DWORD size = di->dwSize;
    memset(di, 0, size);
    di->dwSize = size;
    di->guidInstance = PAD_GUID;
    di->guidProduct = PAD_PRODUCT;
    di->dwDevType = DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8);
    snprintf(di->tszInstanceName, sizeof di->tszInstanceName, "%s", g_gc ? SDL_GameControllerName(g_gc) : "Gamepad");
    snprintf(di->tszProductName, sizeof di->tszProductName, "SDL gamepad");
}

static HRESULT WINAPI dev_GetDeviceInfo(IDirectInputDevice2A *self, LPDIDEVICEINSTANCEA di)
{
    (void)self;
    fill_instance(di);
    return DI_OK;
}

static HRESULT WINAPI dev_RunControlPanel(IDirectInputDevice2A *self, HWND w, DWORD f) { (void)self; (void)w; (void)f; return DI_OK; }
static HRESULT WINAPI dev_Initialize(IDirectInputDevice2A *self, HINSTANCE i, DWORD v, REFGUID g) { (void)self; (void)i; (void)v; (void)g; return DI_OK; }

static HRESULT WINAPI dev_CreateEffect(IDirectInputDevice2A *self, REFGUID guid, LPCDIEFFECT p, LPDIRECTINPUTEFFECT *out, LPUNKNOWN outer)
{
    effect_t *e = calloc(1, sizeof *e);
    (void)self; (void)outer;
    if (!e) {
        return DIERR_OUTOFMEMORY;
    }
    e->vtbl = &g_fx_vtbl;
    e->refs = 1;
    e->kind = effect_kind(guid);
    e->gain = 1.0;
    e->duration = INFINITE;
    if (p) {
        effect_params(e, p, DIEP_DURATION | DIEP_GAIN | DIEP_TYPESPECIFICPARAMS);
    }
    e->next = g_effects;
    g_effects = e;
    *out = (LPDIRECTINPUTEFFECT)e;
    return DI_OK;
}

static HRESULT WINAPI dev_EnumEffects(IDirectInputDevice2A *self, LPDIENUMEFFECTSCALLBACKA cb, LPVOID ref, DWORD type)
{
    (void)self; (void)cb; (void)ref; (void)type;
    return DI_OK;
}

static HRESULT WINAPI dev_GetEffectInfo(IDirectInputDevice2A *self, LPDIEFFECTINFOA info, REFGUID g)
{
    (void)self; (void)info; (void)g;
    return DIERR_DEVICENOTREG;
}

static HRESULT WINAPI dev_GetForceFeedbackState(IDirectInputDevice2A *self, LPDWORD st)
{
    (void)self;
    *st = DIGFFS_POWERON;
    return DI_OK;
}

static HRESULT WINAPI dev_SendForceFeedbackCommand(IDirectInputDevice2A *self, DWORD cmd)
{
    effect_t *e;
    (void)self;
    if (cmd & (DISFFC_RESET | DISFFC_STOPALL)) {
        for (e = g_effects; e; e = e->next) {
            e->active = 0;
        }
        rumble_update();
    }
    return DI_OK;
}

static HRESULT WINAPI dev_EnumCreatedEffectObjects(IDirectInputDevice2A *self, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref, DWORD f)
{
    (void)self; (void)cb; (void)ref; (void)f;
    return DI_OK;
}

static HRESULT WINAPI dev_Escape(IDirectInputDevice2A *self, LPDIEFFESCAPE e) { (void)self; (void)e; return DIERR_UNSUPPORTED; }

static HRESULT WINAPI dev_Poll(IDirectInputDevice2A *self)
{
    (void)self;
    sdl_pump();
    rumble_update();
    return DI_OK;
}

static HRESULT WINAPI dev_SendDeviceData(IDirectInputDevice2A *self, DWORD cb, LPCDIDEVICEOBJECTDATA d, LPDWORD n, DWORD f)
{
    (void)self; (void)cb; (void)d; (void)n; (void)f;
    return DI_OK;
}

static IDirectInputDevice2AVtbl g_dev_vtbl = {
    dev_QueryInterface, dev_AddRef, dev_Release, dev_GetCapabilities, dev_EnumObjects, dev_GetProperty,
    dev_SetProperty, dev_Acquire, dev_Unacquire, dev_GetDeviceState, dev_GetDeviceData, dev_SetDataFormat,
    dev_SetEventNotification, dev_SetCooperativeLevel, dev_GetObjectInfo, dev_GetDeviceInfo, dev_RunControlPanel,
    dev_Initialize, dev_CreateEffect, dev_EnumEffects, dev_GetEffectInfo, dev_GetForceFeedbackState,
    dev_SendForceFeedbackCommand, dev_EnumCreatedEffectObjects, dev_Escape, dev_Poll, dev_SendDeviceData,
};

/* ---------------------------------------------------------------- IDirectInputA wrapper */

typedef struct {
    IDirectInputAVtbl *vtbl;
    IDirectInputA *real;
    LONG refs;
} di_wrap_t;

#define REAL(self) (((di_wrap_t *)(self))->real)

static HRESULT WINAPI di_QueryInterface(IDirectInputA *self, REFIID iid, void **out)
{
    return REAL(self)->lpVtbl->QueryInterface(REAL(self), iid, out);
}

static ULONG WINAPI di_AddRef(IDirectInputA *self) { return ++((di_wrap_t *)self)->refs; }

static ULONG WINAPI di_Release(IDirectInputA *self)
{
    di_wrap_t *w = (di_wrap_t *)self;
    if (--w->refs) {
        return w->refs;
    }
    w->real->lpVtbl->Release(w->real);
    free(w);
    return 0;
}

static HRESULT WINAPI di_CreateDevice(IDirectInputA *self, REFGUID guid, LPDIRECTINPUTDEVICEA *out, LPUNKNOWN outer)
{
    if (IsEqualGUID(guid, &PAD_GUID)) {
        g_dev.vtbl = &g_dev_vtbl;
        g_dev.refs++;
        *out = (LPDIRECTINPUTDEVICEA)&g_dev;
        return DI_OK;
    }
    return REAL(self)->lpVtbl->CreateDevice(REAL(self), guid, out, outer);
}

typedef struct {
    LPDIENUMDEVICESCALLBACKA cb;
    LPVOID ref;
    int stopped;
    int others;          /* counting pass: real joysticks SDL doesn't handle */
    int counting;
} enum_ctx_t;

static BOOL CALLBACK enum_real(LPCDIDEVICEINSTANCEA di, LPVOID ref)
{
    enum_ctx_t *c = ref;
    if (sdl_handles(&di->guidProduct)) {
        return DIENUM_CONTINUE;
    }
    if (c->counting) {
        c->others++;
        return DIENUM_CONTINUE;
    }
    if (c->stopped || c->cb(di, c->ref) == DIENUM_STOP) {
        c->stopped = 1;
        return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

static HRESULT WINAPI di_EnumDevices(IDirectInputA *self, DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags)
{
    enum_ctx_t c = { cb, ref, 0, 0, 1 };
    DIDEVICEINSTANCEA pad;
    HRESULT hr;
    if (type != DIDEVTYPE_JOYSTICK) {
        return REAL(self)->lpVtbl->EnumDevices(REAL(self), type, cb, ref, flags);
    }
    sdl_init();
    REAL(self)->lpVtbl->EnumDevices(REAL(self), type, enum_real, &c, flags);
    c.counting = 0;
    pad.dwSize = sizeof pad;
    fill_instance(&pad);
    /* the game takes the first joystick: the SDL pad, unless only real (non-SDL) joysticks are attached */
    if (g_gc || !c.others) {
        pc_log("gamepad: offering the SDL gamepad%s", g_gc ? "" : " (none plugged in yet)");
        if (cb(&pad, ref) == DIENUM_STOP) {
            return DI_OK;
        }
    }
    hr = REAL(self)->lpVtbl->EnumDevices(REAL(self), type, enum_real, &c, flags);
    if (!c.stopped && !g_gc && c.others) {
        cb(&pad, ref);
    }
    return hr;
}

static HRESULT WINAPI di_GetDeviceStatus(IDirectInputA *self, REFGUID guid)
{
    if (IsEqualGUID(guid, &PAD_GUID)) {
        return DI_OK;
    }
    return REAL(self)->lpVtbl->GetDeviceStatus(REAL(self), guid);
}

static HRESULT WINAPI di_RunControlPanel(IDirectInputA *self, HWND w, DWORD f)
{
    return REAL(self)->lpVtbl->RunControlPanel(REAL(self), w, f);
}

static HRESULT WINAPI di_Initialize(IDirectInputA *self, HINSTANCE i, DWORD v)
{
    return REAL(self)->lpVtbl->Initialize(REAL(self), i, v);
}

static IDirectInputAVtbl g_di_vtbl = {
    di_QueryInterface, di_AddRef, di_Release, di_CreateDevice, di_EnumDevices, di_GetDeviceStatus,
    di_RunControlPanel, di_Initialize,
};

static int g_on;

void *pc_gamepad_wrap(void *direct_input)
{
    di_wrap_t *w;
    if (!g_on || !direct_input) {
        return direct_input;
    }
    w = calloc(1, sizeof *w);
    if (!w) {
        return direct_input;
    }
    w->vtbl = &g_di_vtbl;
    w->real = direct_input;
    w->refs = 1;
    return w;
}

/* ---------------------------------------------------------------- cockpit view binding */

typedef void (__attribute__((thiscall)) *load_map_fn)(void *in);

static void __attribute__((fastcall)) load_map(void *in)
{
    int *bind = (int *)((char *)in + BIND_COCKPIT);
    ((load_map_fn)PC_ADDR(LOAD_MAP))(in);
    if (*bind == -1 && g_cockpit_button > 0) {
        *bind = g_cockpit_button;
        pc_log("gamepad: cockpit view on button %d", g_cockpit_button);
    }
}

void pc_gamepad_install(void)
{
    static const uint8_t call_map[5] = { 0xe8, 0x11, 0x00, 0x00, 0x00 };
    if (!pc_ini_int("Gamepad", "UseSDL", 1)) {
        return;
    }
    g_deadzone = pc_ini_int("Gamepad", "Deadzone", g_deadzone);
    g_rumble = pc_ini_int("Gamepad", "Rumble", g_rumble);
    g_invert_y = pc_ini_int("Gamepad", "InvertY", 0);
    g_cockpit_button = pc_ini_int("Gamepad", "CockpitViewButton", g_cockpit_button);
    g_start_menu = pc_ini_int("Gamepad", "StartOpensMenu", 1);
    g_dev.range_min[0] = g_dev.range_min[1] = g_dev.range_min[2] = 0;
    g_dev.range_max[0] = g_dev.range_max[1] = g_dev.range_max[2] = 0xffff;
    if (pc_expect(LOAD_MAP_CALL, call_map, 5)) {
        pc_call(LOAD_MAP_CALL, (void *)load_map);
    }
    if (g_start_menu) {
        static const uint8_t call_tick[5] = { 0xe8, 0xe5, 0xaf, 0xff, 0xff };
        if (pc_expect(RACE_TICK_CALL, call_tick, 5)) {
            pc_call(RACE_TICK_CALL, (void *)race_tick);
        } else {
            g_start_menu = 0;
        }
    }
    g_on = 1;
    pc_log("gamepad: SDL gamepads on (dead zone %d%%, rumble %d%%)", g_deadzone, g_rumble);
}
