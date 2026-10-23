/* More network players: 8 -> [Players] MaxPlayers (up to 16; docs/network_players_research.md).
 *
 * The netcode keeps players in maps keyed by DPID / object id and sends a player id as a dword or the high byte
 * of an object id, and the race side already has room for 16 planes (grid, results, championship tables; the race
 * clamps ids above 16 at 0x467378 / 0x45f6a3). What holds a game at 8 is a set of immediates: the DirectPlay
 * session size, the id checks that drop players above 8, the object id -> owner clamp and the "Player0..7"
 * material array. Every machine in a game needs the same MaxPlayers (there is no version handshake).
 * Not changed: the plane number markers and progress-bar markers stay on ids 1..8 (ids are printed as icon
 * glyphs there, and 9+ would print control characters). */
#include "pcpatch.h"

typedef struct {
    uint32_t addr;      /* immediate's address */
    uint8_t size;       /* 1 = imm8, 4 = dword */
    uint32_t orig;
    int add;            /* new value = N + add (N = max players) */
    int at_least;       /* ... but not below this */
} site_t;

static const site_t SITES[] = {
    { 0x47f44b, 4, 8, 0, 0 },     /* NET_pt_Connect: dwMaxPlayers of the per-course (race) session, create */
    { 0x47f60f, 4, 8, 0, 0 },     /*   ... join */
    { 0x51bba0, 4, 8, 0, 0 },     /* dwMaxPlayers of the "Plane Crazy" / "Lobby" sessions (.data) */
    { 0x479298, 1, 8, 0, 0 },     /* NET_PumpAndPrunePlayers: removes players with id > 8 */
    { 0x47b03f, 1, 8, 0, 0 },     /* NET_SendEvent: my id > 8 -> 0 */
    { 0x479761, 1, 9, 0, 0 },     /* id counter cap ("fa" join mode) */
    { 0x47778e, 1, 9, 1, 0 },     /* NET_ObjIdToOwnerPlayer: owner clamp (cmp id, 9 / jl) */
    { 0x477792, 4, 8, 0, 0 },     /*   ... clamped value */
    { 0x410531, 4, 8, 0, 8 },     /* game ctor: CPU/player material counts ("Player0..7" materials, one per remote plane) */
    { 0x4661c3, 4, 9, 0, 9 },     /* HUD_ListPlayersById: pre-race "ready" list shows ids 1..9 */
    { 0x47ff2d, 1, 8, 0, 0 },     /* pt lobby id counter */
    { 0x47ecd6, 1, 8, 0, 0 },     /* pt own-id validation */
    { 0x480354, 1, 9, 1, 0 },     /* pt received-id validation */
};

int pc_max_players = 8;

void pc_players_install(void)
{
    int n = pc_ini_int("Players", "MaxPlayers", 16), ok = 0;
    unsigned i;
    if (n < 8) {
        n = 8;
    }
    if (n > 16) {
        n = 16;
    }
    if (n == 8) {
        return;
    }
    for (i = 0; i < sizeof SITES / sizeof SITES[0]; i++) {
        const site_t *s = &SITES[i];
        uint32_t v = (uint32_t)(n + s->add);
        if ((int)v < s->at_least) {
            v = (uint32_t)s->at_least;
        }
        ok += pc_patch(s->addr, &s->orig, &v, s->size);
    }
    pc_max_players = n;
    pc_log("players: up to %d players per network game (%d/%u sites)", n, ok, (unsigned)(sizeof SITES / sizeof SITES[0]));
}
