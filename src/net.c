/* Network games over TCP/IP only, with an in-game list of open games (docs/network_lobby_research.md).
 *
 * The original: "pcrazy.exe -m" (sticky setting bWantNetwork) asks for a protocol in a dialog at startup
 * (NET_StartupInit 0x414be0, then an IP address dialog for TCP/IP), and "Network Game" in the main menu
 * joins the one network-wide "Plane Crazy" DirectPlay session (or creates it), freezing the menus for up to
 * 10 s in synchronous EnumSessions calls; choosing a course then joins or creates that course's session
 * ("Border Dash"...), again with synchronous enumerations.
 *
 * Here:
 * - Network play is always available ([Network] Enable sets game+0x228 = bWantNetwork after the settings are
 *   read), and the protocol dialog is replaced by NET_pt_InitTCP 0x47d8e0(pt, [Network] Host): TCP/IP, LAN
 *   broadcast unless a host is given. That only initialises the two DirectPlay objects (pt[0] sessions,
 *   pt[1] enumeration); no network traffic.
 * - "Network Game" opens a new "Network Games" menu that lists the lobby sessions found by asynchronous
 *   enumeration on pt[0] (polled from the frontend loop while the menu is shown, never blocking). Join opens
 *   the session, Host creates one named [Network] GameName (default "<pilot>'s game"); then the game's own
 *   "nnet" action runs with NET_pt_ConnectLobby (call at 0x42bf22) replaced by just creating the player in that
 *   session, and the course menu ("lobby") follows as before. Enumeration happens only on that screen.
 * - Lobby sessions carry a tag (dwUser3 = hash of the name, dwUser4 = 'PCLB'); course sessions created by the
 *   members get the same tag, so several games can share a network. The course menu's async enumeration
 *   callback (0x4805d0, pushed at 0x47f749) is wrapped to skip sessions of other games (it also stopped at
 *   the first unknown session name).
 * - NET_pt_Connect (call at 0x47f302) is replaced: a course session already seen by pt[1]'s asynchronous
 *   enumeration is joined on pt[1] (which then becomes the session object), otherwise one is created on
 *   pt[0]. No synchronous enumeration, no 5 s duplicate checks. */
#include <stdio.h>
#include <string.h>
#include <dplay.h>
#include "pcpatch.h"

#define WANT_NET_JE     0x414dafu    /* je: skip the network init unless the registry says bWantNetwork */
#define DIALOG_CALL     0x414ddeu    /* call [DialogBoxParamA] (connection type dialog) in NET_StartupInit */
#define PT_INIT_TCP     0x47d8e0u    /* NET_pt_InitTCP(pt, char *host), stdcall */
#define LOBBY_CALL      0x42bf22u    /* call NET_pt_ConnectLobby(pt, name) in MENU_NetConnectLobby */
#define PT_CONNECT_LOBBY 0x47df40u
#define CONNECT_CALL    0x47f302u    /* call NET_pt_Connect(pt, course) in NET_pt_JoinSession */
#define COURSE_CB_PUSH  0x47f749u    /* push 0x4805d0 (course table callback) in NET_pt_CheckSession */
#define COURSE_CB       0x4805d0u
#define COURSE_CB_CALLS 0x5598c8u    /* its call counter (reset by NET_pt_CheckSession) */
#define PT_PTR          0x5220dcu    /* the pt struct */
#define NET_FAILED      0x5220e4u    /* 1 = network init failed (network menus hidden) */
#define PT_GLOBAL       0x559760u    /* global copy of the pt struct (0xb4 bytes) */
#define LOBBY_RESULT    0x559750u
#define CONNECT_RESULT  0x55981cu
#define CONNECT_COURSE  0x559898u
#define AM_HOST         0x559810u
#define CONNECTED       0x559811u
#define HOST_TABLE      0x559820u    /* 7 x {current, max, user1, user2} by course slot */
#define MAX_PLAYERS     0x51bba0u    /* players per session (players.c), lobby = x6 */
#define APP_GUID        0x5118c8u
#define G_WANT_NET      0x228
#define MENU_DO_ACTION  0x42a84fu    /* MENU_DoAction (thiscall fe, id) */

#define PT_DP0   0x00
#define PT_DP1   0x04
#define PT_DESC  0x08
#define PT_NAME  0x58                /* DPNAME */
#define PT_ID    0x68
#define PT_HOST  0xb0
#define PT_CONN  0xb1
#define PT_DIRECT 0xb2

#define RES_HOST 0x1d
#define RES_JOIN 0x1e
#define RES_FAIL 0x21

#define LOBBY_MARK 0x424c4350u        /* dwUser4 'PCLB': a lobby made by this patch */
#define SLOTS 8

typedef struct {
    GUID guid;
    char name[64];
    DWORD cur, max, tag;
} game_t;

static const struct { int id; const char *name; int slot; } COURSES[] = {
    { 6000, "Border Dash", 1 }, { 6001, "Docklands Dive", 3 }, { 6002, "Monument Rush", 0 },
    { 6003, "Volcano Rapids", 4 }, { 6004, "Sin City Run", 2 }, { 6005, "Circuit Chase", 5 },
    { 6006, "Championship", 6 },
};

static char g_host[128];
static char g_game_name[48];
static int g_browser = 1;
static DWORD g_tag;                  /* tag of the lobby we are in (0 = an original "Plane Crazy" lobby) */
static int g_pending;                /* lobby opened by the browser: RES_HOST / RES_JOIN, for net_connect_lobby */
static char g_session_name[64];      /* name of the session opened (pt+8 points here) */

static game_t g_games[SLOTS], g_found[SLOTS];
static int g_ngames, g_nfound;
static int g_browsing;               /* enumeration running (the Network Games menu is shown) */
static DWORD g_last_poll;
static int g_menu = -1;              /* index of the Network Games menu */
static int g_id_host, g_id_join[SLOTS], g_id_slot[SLOTS], g_id_nop, g_id_nnet;

static char *pt(void) { return *(char **)PC_ADDR(PT_PTR); }
static IDirectPlay4A *dp(int i) { return *(IDirectPlay4A **)(pt() + (i ? PT_DP1 : PT_DP0)); }

static DWORD name_tag(const char *s)  /* FNV-1a, never 0 */
{
    DWORD h = 2166136261u;
    for (; *s; s++) {
        h = (h ^ (unsigned char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s)) * 16777619u;
    }
    return h ? h : 1;
}

static const char *course_name(int id)
{
    int i;
    for (i = 0; i < (int)(sizeof COURSES / sizeof COURSES[0]); i++) {
        if (COURSES[i].id == id) {
            return COURSES[i].name;
        }
    }
    return NULL;
}

static int course_index(const char *name)
{
    int i;
    for (i = 0; name && i < (int)(sizeof COURSES / sizeof COURSES[0]); i++) {
        if (!strcmp(COURSES[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static void init_desc(DPSESSIONDESC2 *d)
{
    memset(d, 0, sizeof *d);
    d->dwSize = sizeof *d;
    memcpy(&d->guidApplication, PC_ADDR(APP_GUID), sizeof(GUID));
}

/* ---------------------------------------------------------------- startup: TCP/IP, no dialog */

static INT_PTR WINAPI startup_dialog(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM param)
{
    int r;
    (void)inst; (void)tmpl; (void)parent; (void)proc; (void)param;
    r = ((int (WINAPI *)(void *, const char *))PC_ADDR(PT_INIT_TCP))(pt(), g_host);
    if (r == RES_FAIL) {
        *(int *)PC_ADDR(NET_FAILED) = 1;
        pc_log("net: TCP/IP initialisation failed, no network games");
    } else {
        pc_log("net: TCP/IP%s%s", g_host[0] ? ", host " : " (LAN broadcast)", g_host);
    }
    return 0;
}

static void after_config(char *game)
{
    *(int *)(game + G_WANT_NET) = 1;   /* "-m": network menus on, startup initialises DirectPlay */
}

/* ---------------------------------------------------------------- lobby browser */

static BOOL WINAPI browse_cb(LPCDPSESSIONDESC2 d, LPDWORD timeout, DWORD flags, LPVOID ctx)
{
    game_t *g;
    (void)timeout; (void)ctx;
    if (flags & DPESC_TIMEDOUT) {
        return FALSE;
    }
    if (!d->lpszSessionNameA || course_index(d->lpszSessionNameA) >= 0 || !strcmp(d->lpszSessionNameA, "Lobby")) {
        return TRUE;   /* course sessions are listed in the course menu once in a game */
    }
    if (d->dwUser4 != LOBBY_MARK && strcmp(d->lpszSessionNameA, "Plane Crazy")) {
        return TRUE;
    }
    if (g_nfound < SLOTS) {
        g = &g_found[g_nfound++];
        g->guid = d->guidInstance;
        lstrcpynA(g->name, d->lpszSessionNameA, sizeof g->name);
        g->cur = d->dwCurrentPlayers;
        g->max = d->dwMaxPlayers;
        g->tag = d->dwUser4 == LOBBY_MARK ? d->dwUser3 : 0;
    }
    return TRUE;
}

static char *menu_base(void)
{
    return g_menu < 0 ? NULL : pc_menu_item("netgames", 0) - 0x64;
}

static void show_games(void)
{
    char *m = menu_base(), *item;
    int i, cursor;
    if (!m) {
        return;
    }
    item = pc_menu_item("netgames", 2);   /* status line */
    if (item) {
        snprintf(item + 8, 36, g_ngames ? "%d game%s found" : "Searching...", g_ngames, g_ngames == 1 ? "" : "s");
    }
    for (i = 0; i < SLOTS; i++) {
        item = pc_menu_item("netgames", 3 + i);
        if (!item) {
            break;
        }
        if (i < g_ngames) {
            snprintf(item + 8, 36, "%.26s  %lu/%lu", g_games[i].name, g_games[i].cur, g_games[i].max);
        }
        *(int *)(item + 4) = i < g_ngames;
    }
    cursor = *(int *)(m + 0x38);
    if (cursor >= 3 && cursor < 3 + SLOTS && cursor - 3 >= g_ngames) {
        *(int *)(m + 0x38) = 0;   /* the highlighted game went away: back to "Host New Game" */
    }
    pc_menu_redraw();
}

static void poll_games(void)
{
    DPSESSIONDESC2 d;
    HRESULT hr;
    int i, changed;
    init_desc(&d);
    g_nfound = 0;
    hr = IDirectPlayX_EnumSessions(dp(0), &d, 0, browse_cb, NULL, DPENUMSESSIONS_AVAILABLE | DPENUMSESSIONS_ASYNC);
    if (hr == E_FAIL && !g_browsing) {
        IDirectPlayX_Close(dp(0));     /* still in a session from before: leave it and retry */
        hr = IDirectPlayX_EnumSessions(dp(0), &d, 0, browse_cb, NULL, DPENUMSESSIONS_AVAILABLE | DPENUMSESSIONS_ASYNC);
    }
    if (FAILED(hr) && hr != DPERR_CONNECTING) {
        pc_log("net: EnumSessions failed (%08lx)", hr);
    }
    changed = g_nfound != g_ngames;
    for (i = 0; !changed && i < g_nfound; i++) {
        changed = memcmp(&g_found[i], &g_games[i], sizeof g_found[i]) != 0;
    }
    if (changed || !g_browsing) {
        memcpy(g_games, g_found, sizeof g_games);
        g_ngames = g_nfound;
        show_games();
    }
    g_browsing = 1;
}

static void stop_browsing(void)
{
    DPSESSIONDESC2 d;
    if (g_browsing) {
        init_desc(&d);
        IDirectPlayX_EnumSessions(dp(0), &d, 0, browse_cb, NULL, DPENUMSESSIONS_STOPASYNC);
        g_browsing = 0;
    }
}

static void on_frame(void *fe)
{
    (void)fe;
    if (g_menu < 0 || !pt()) {
        return;
    }
    if (pc_menu_current() != g_menu) {
        stop_browsing();
        return;
    }
    if (!g_browsing || GetTickCount() - g_last_poll >= 500) {
        g_last_poll = GetTickCount();
        poll_games();
    }
}

/* opens (JOIN) a listed game or creates (CREATE) ours on pt[0], then runs the game's own network start */
static void enter_lobby(void *fe, int join)
{
    DPSESSIONDESC2 *d = (DPSESSIONDESC2 *)(pt() + PT_DESC);
    game_t g;
    HRESULT hr;
    if (join >= 0) {
        g = g_games[join];
    }
    /* no STOPASYNC first: it drops the sessions found, and JOIN then fails with DPERR_NOSESSIONS */
    init_desc(d);
    d->dwFlags = DPSESSION_MIGRATEHOST | DPSESSION_KEEPALIVE;
    d->dwMaxPlayers = *(int *)PC_ADDR(MAX_PLAYERS) * 6;
    if (join >= 0) {
        d->guidInstance = g.guid;
        lstrcpynA(g_session_name, g.name, sizeof g_session_name);
        d->lpszSessionNameA = g_session_name;
        hr = IDirectPlayX_Open(dp(0), d, DPOPEN_JOIN);
        g_tag = g.tag;
    } else {
        lstrcpynA(g_session_name, g_game_name, sizeof g_session_name);
        if (!g_session_name[0]) {   /* "<pilot>'s game" */
            const char *pilot = *(char **)((char *)fe + 0x7584) + 0x20;
            snprintf(g_session_name, sizeof g_session_name, "%.20s's game", pilot[0] ? pilot : "Plane Crazy");
        }
        g_tag = name_tag(g_session_name);
        d->lpszSessionNameA = g_session_name;
        d->dwUser3 = g_tag;
        d->dwUser4 = LOBBY_MARK;
        hr = IDirectPlayX_Open(dp(0), d, DPOPEN_CREATE);
    }
    if (FAILED(hr)) {
        pc_log("net: could not %s \"%s\" (%08lx)", join >= 0 ? "join" : "create", g_session_name, hr);
        g_last_poll = 0;
        return;   /* stays on the list; the poll restarts the enumeration */
    }
    g_browsing = 0;   /* opening a session ends the enumeration */
    pc_log("net: %s \"%s\"", join >= 0 ? "joined" : "hosting", g_session_name);
    g_pending = join >= 0 ? RES_JOIN : RES_HOST;
    pc_menu_goto("lobby");                 /* where LINK "lobby" went from the original "Network Game" item, */
    ((int (__attribute__((thiscall)) *)(void *, int))PC_ADDR(MENU_DO_ACTION))(fe, g_id_nnet);   /* then "nnet" */
}

static int on_action(void *fe, int id)
{
    int i;
    if (id < 0) {
        return 0;
    }
    if (id == g_id_nop) {
        return 1;
    }
    if (id == g_id_host) {
        enter_lobby(fe, -1);
        return 1;
    }
    for (i = 0; i < SLOTS; i++) {
        if (id == g_id_join[i]) {
            if (i < g_ngames) {
                enter_lobby(fe, i);
            }
            return 1;
        }
    }
    return 0;
}

static int on_condition(int id, int *result)
{
    int i;
    for (i = 0; id >= 0 && i < SLOTS; i++) {
        if (id == g_id_slot[i]) {
            *result = i < g_ngames;
            return 1;
        }
    }
    return 0;
}

static void on_loaded(void)
{
    char name[16];
    int i;
    g_menu = pc_menu_index("netgames");
    if (g_menu >= 0) {
        *(int *)pc_menu_item("netgames", 2) = 3;   /* status line: shown, not selectable (like INFO lines) */
    }
    g_id_host = pc_menu_id("pchost");
    g_id_nop = pc_menu_id("pcnetnop");
    g_id_nnet = pc_menu_id("nnet");
    for (i = 0; i < SLOTS; i++) {
        snprintf(name, sizeof name, "pcjoin%d", i + 1);
        g_id_join[i] = pc_menu_id(name);
        snprintf(name, sizeof name, "pcslot%d", i + 1);
        g_id_slot[i] = pc_menu_id(name);
    }
}

/* ---------------------------------------------------------------- lobby / course session connects */

/* replaces NET_pt_ConnectLobby: the browser already opened the session on pt[0]; create our player in it */
static int WINAPI net_connect_lobby(char *p, char *pilot)
{
    DPNAME *n = (DPNAME *)(p + PT_NAME);
    int r = g_pending;
    if (!r) {
        return ((int (WINAPI *)(char *, char *))PC_ADDR(PT_CONNECT_LOBBY))(p, pilot);
    }
    g_pending = 0;
    memset(n, 0, sizeof *n);
    n->dwSize = sizeof *n;
    n->lpszShortNameA = pilot;
    if (FAILED(IDirectPlayX_CreatePlayer(dp(0), (DPID *)(p + PT_ID), n, NULL, NULL, 0, 0))) {
        pc_log("net: CreatePlayer failed");
        IDirectPlayX_Close(dp(0));
        return RES_FAIL;
    }
    p[PT_CONN] = 1;
    p[PT_HOST] = r == RES_HOST;
    *(int *)PC_ADDR(LOBBY_RESULT) = r;
    return r;
}

typedef struct {
    const char *name;
    DPSESSIONDESC2 desc;
    int found;
} course_find_t;

static BOOL WINAPI course_find_cb(LPCDPSESSIONDESC2 d, LPDWORD timeout, DWORD flags, LPVOID ctx)
{
    course_find_t *f = ctx;
    (void)timeout;
    if (flags & DPESC_TIMEDOUT) {
        return FALSE;
    }
    if (d->lpszSessionNameA && !strcmp(d->lpszSessionNameA, f->name) && d->dwUser3 == g_tag
        && d->dwCurrentPlayers < d->dwMaxPlayers) {
        f->desc = *d;
        f->found = 1;
        return FALSE;
    }
    return TRUE;
}

/* replaces NET_pt_Connect (stdcall pt, course id): join the course session of our game or create it */
static int WINAPI net_pt_connect(char *p, int course)
{
    DPSESSIONDESC2 *d = (DPSESSIONDESC2 *)(p + PT_DESC), q;
    course_find_t f;
    int r = 0, ci;
    const char *name = course_name(course);
    *(int *)PC_ADDR(CONNECT_RESULT) = 0;
    *(int *)PC_ADDR(CONNECT_COURSE) = course;
    if (!name) {
        return RES_FAIL;
    }
    memset(&f, 0, sizeof f);
    f.name = name;
    init_desc(&q);
    IDirectPlayX_EnumSessions(dp(1), &q, 0, course_find_cb, &f, DPENUMSESSIONS_AVAILABLE | DPENUMSESSIONS_ASYNC);
    if (f.found) {   /* pt[1] has seen it (the course menu enumerates on it every frame): join there */
        init_desc(d);
        d->dwFlags = DPSESSION_MIGRATEHOST | DPSESSION_KEEPALIVE;
        d->guidInstance = f.desc.guidInstance;
        d->dwMaxPlayers = f.desc.dwMaxPlayers;
        d->lpszSessionNameA = (char *)name;
        d->dwUser3 = g_tag;
        if (SUCCEEDED(IDirectPlayX_Open(dp(1), d, DPOPEN_JOIN))) {
            void *t = *(void **)(p + PT_DP0);       /* the joined object becomes the session object */
            *(void **)(p + PT_DP0) = *(void **)(p + PT_DP1);
            *(void **)(p + PT_DP1) = t;
            r = RES_JOIN;
            ci = course_index(name);
            if (ci >= 0) {   /* MENU_StartNetRace waits (5 s, frozen) until the course table shows the session */
                DWORD *slot = (DWORD *)PC_ADDR(HOST_TABLE) + COURSES[ci].slot * 4;
                slot[0] = f.desc.dwCurrentPlayers;
                slot[1] = f.desc.dwMaxPlayers;
                slot[2] = f.desc.dwUser1;
                slot[3] = f.desc.dwUser2;
            }
            pc_log("net: joined course session \"%s\"", name);
        } else {
            pc_log("net: could not join course session \"%s\", creating one", name);
        }
    }
    if (!r) {
        init_desc(d);
        d->dwFlags = DPSESSION_MIGRATEHOST | DPSESSION_KEEPALIVE;
        d->dwMaxPlayers = *(int *)PC_ADDR(MAX_PLAYERS);
        d->lpszSessionNameA = (char *)name;
        d->dwUser3 = g_tag;
        if (FAILED(IDirectPlayX_Open(dp(0), d, DPOPEN_CREATE))) {
            pc_log("net: could not create course session \"%s\"", name);
            *(int *)PC_ADDR(CONNECT_RESULT) = RES_FAIL;
            return RES_FAIL;
        }
        r = RES_HOST;
        pc_log("net: created course session \"%s\"", name);
    }
    if (FAILED(IDirectPlayX_CreatePlayer(dp(0), (DPID *)(p + PT_ID), (DPNAME *)(p + PT_NAME), NULL, NULL, 0, 0))) {
        pc_log("net: CreatePlayer in the course session failed");
        *(int *)PC_ADDR(CONNECT_RESULT) = RES_FAIL;
        return RES_FAIL;
    }
    p[PT_CONN] = *(char *)PC_ADDR(CONNECTED);
    p[PT_HOST] = *(char *)PC_ADDR(AM_HOST);
    memcpy(PC_ADDR(PT_GLOBAL), p, 0xb4);
    *(int *)PC_ADDR(CONNECT_RESULT) = r;
    return r;
}

/* the course menu's enumeration callback, minus the sessions of other games */
static BOOL WINAPI course_table_cb(LPCDPSESSIONDESC2 d, LPDWORD timeout, DWORD flags, LPVOID ctx)
{
    if (!(flags & DPESC_TIMEDOUT) && (course_index(d->lpszSessionNameA) < 0 || d->dwUser3 != g_tag)) {
        if (course_index(d->lpszSessionNameA) < 0) {   /* it counts its calls: 2 at the end = only the lobby */
            (*(int *)PC_ADDR(COURSE_CB_CALLS))++;       /* seen, no course sessions: clear the table */
        }
        return TRUE;
    }
    return ((BOOL (WINAPI *)(LPCDPSESSIONDESC2, LPDWORD, DWORD, LPVOID))PC_ADDR(COURSE_CB))(d, timeout, flags, ctx);
}

/* ---------------------------------------------------------------- install */

static const char MENU_DEFS[] =
    "DEFACTION \"pchost\"\nDEFACTION \"pcnetnop\"\n"
    "DEFACTION \"pcjoin1\"\nDEFACTION \"pcjoin2\"\nDEFACTION \"pcjoin3\"\nDEFACTION \"pcjoin4\"\n"
    "DEFACTION \"pcjoin5\"\nDEFACTION \"pcjoin6\"\nDEFACTION \"pcjoin7\"\nDEFACTION \"pcjoin8\"\n"
    "DEFCONDVISACTION \"pcslot1\"\nDEFCONDVISACTION \"pcslot2\"\nDEFCONDVISACTION \"pcslot3\"\n"
    "DEFCONDVISACTION \"pcslot4\"\nDEFCONDVISACTION \"pcslot5\"\nDEFCONDVISACTION \"pcslot6\"\n"
    "DEFCONDVISACTION \"pcslot7\"\nDEFCONDVISACTION \"pcslot8\"\n";

static const char MENU_BODY[] =
    "MENU \"netgames\" TITLE \"Network Games\"\n"
    "XPOS 0\nYPOS 0\nZPOS 100\nANGA 20\nANGB 20\nANGC 0\n"
    "ITEM \"Host New Game\" ACTION \"pchost\" MINWIDTH 30\n"    /* 0 */
    "ITEM \"\"\n"                                              /* 1 */
    "ITEM \"Searching...\" ACTION \"pcnetnop\"\n"              /* 2: status line (label set at run time) */
    "ITEM \"-\" ACTION \"pcjoin1\" CONDITIONALVIS \"pcslot1\"\n"  /* 3..10: games found */
    "ITEM \"-\" ACTION \"pcjoin2\" CONDITIONALVIS \"pcslot2\"\n"
    "ITEM \"-\" ACTION \"pcjoin3\" CONDITIONALVIS \"pcslot3\"\n"
    "ITEM \"-\" ACTION \"pcjoin4\" CONDITIONALVIS \"pcslot4\"\n"
    "ITEM \"-\" ACTION \"pcjoin5\" CONDITIONALVIS \"pcslot5\"\n"
    "ITEM \"-\" ACTION \"pcjoin6\" CONDITIONALVIS \"pcslot6\"\n"
    "ITEM \"-\" ACTION \"pcjoin7\" CONDITIONALVIS \"pcslot7\"\n"
    "ITEM \"-\" ACTION \"pcjoin8\" CONDITIONALVIS \"pcslot8\"\n"
    "ITEM \"\"\n"
    "ITEM \"Done\" ACTION \"back\" MINWIDTH 30\n\n";

void pc_net_install(void)
{
    static const uint8_t call_dialog[6] = { 0xff, 0x15, 0x00, 0x02, 0x51, 0x00 };
    static const uint8_t call_lobby[5] = { 0xe8, 0x19, 0x20, 0x05, 0x00 };
    static const uint8_t call_connect[5] = { 0xe8, 0x89, 0x00, 0x00, 0x00 };
    static const uint8_t push_cb[5] = { 0x68, 0xd0, 0x05, 0x48, 0x00 };
    if (!pc_ini_int("Network", "Enable", 1)) {
        return;
    }
    pc_ini_str("Network", "Host", g_host, sizeof g_host);
    pc_ini_str("Network", "GameName", g_game_name, sizeof g_game_name);
    g_browser = pc_ini_int("Network", "Browser", 1);
    if (!pc_expect(DIALOG_CALL, call_dialog, 6) || !pc_expect(LOBBY_CALL, call_lobby, 5)
        || !pc_expect(CONNECT_CALL, call_connect, 5) || !pc_expect(COURSE_CB_PUSH, push_cb, 5)) {
        pc_log("net: code differs, network changes off");
        return;
    }
    /* NET_StartupInit reads bWantNetwork from the registry again: skip that test (game+0x228 is tested next) */
    if (!pc_patch(WANT_NET_JE, "\x0f\x84\x2d\x01\x00\x00", "\x90\x90\x90\x90\x90\x90", 6)) {
        return;
    }
    pc_on_game_config(after_config);
    pc_call(DIALOG_CALL, (void *)startup_dialog);
    pc_write(DIALOG_CALL + 5, "\x90", 1);
    pc_call(CONNECT_CALL, (void *)net_pt_connect);
    if (g_browser) {
        uint32_t cb = (uint32_t)(uintptr_t)course_table_cb;
        pc_write(COURSE_CB_PUSH + 1, &cb, 4);
        pc_call(LOBBY_CALL, (void *)net_connect_lobby);
        pc_menu_add_defs(MENU_DEFS);
        pc_menu_replace("ITEM \"Network Game\" LINK \"lobby\"",
                        "ITEM \"Network Game\" LINK \"netgames\" CONDITIONALVIS \"isnet\"\n");
        pc_menu_append(MENU_BODY);
        pc_menu_on_action(on_action);
        pc_menu_on_condition(on_condition);
        pc_menu_on_frame(on_frame);
        pc_menu_on_loaded(on_loaded);
    }
    pc_log("net: TCP/IP only%s", g_browser ? ", game browser in the menus" : "");
}
