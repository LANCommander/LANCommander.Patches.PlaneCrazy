/* Sound device list overflow (the startup crash on PCs with many audio endpoints).
 *
 * SND_EnumDevicesCallback 0x46ef70 (the DirectSoundEnumerateA callback, passed at 0x46f3e8) appends every
 * device to the static table g_SndDevices 0x556e58: 16 entries of 0x210 bytes (description[0x100],
 * module[0x100], GUID), with the count at 0x558f58 right behind it. There is no bounds check: the 17th
 * device overwrites the count with its description, the next one strncpy()s to a wild address and the
 * game crashes (0xc0000005 at 0x4fb359, inside strncpy). Windows lists the "Primary Sound Driver" plus
 * every active playback endpoint, so 16 endpoints are enough (monitors with HDMI/DP audio, virtual
 * devices, headsets...).
 *
 * Fix: the enumeration goes through a wrapper that stops it once the table is full. The primary driver
 * is listed first, so it is always kept. */
#include <string.h>
#include "pcpatch.h"

#define SND_ENUM_PUSH   0x46f3e8u   /* push SND_EnumDevicesCallback */
#define SND_ENUM_CB     0x46ef70u
#define SND_DEVICES     0x556e58u
#define SND_COUNT       0x558f58u
#define SND_MAX         16

typedef BOOL (CALLBACK *enum_cb)(GUID *, const char *, const char *, void *);

static int g_skipped;

static BOOL CALLBACK enum_devices(GUID *guid, const char *desc, const char *module, void *ctx)
{
    if (*(volatile int *)PC_ADDR(SND_COUNT) >= SND_MAX) {
        if (!g_skipped++) {
            pc_log("sound: device list full (%d), skipping \"%s\" and later devices", SND_MAX, desc ? desc : "");
        }
        return FALSE;
    }
    return ((enum_cb)PC_ADDR(SND_ENUM_CB))(guid, desc, module, ctx);
}

void pc_sound_install(void)
{
    static const uint8_t push_cb[5] = { 0x68, 0x70, 0xef, 0x46, 0x00 };
    uint8_t push_ours[5] = { 0x68 };
    uint32_t to = (uint32_t)(uintptr_t)enum_devices;

    if (!pc_ini_int("Sound", "LimitDevices", 1)) {
        return;
    }
    memcpy(push_ours + 1, &to, 4);
    if (pc_patch(SND_ENUM_PUSH, push_cb, push_ours, 5)) {
        pc_log("sound: device list limited to %d entries", SND_MAX);
    }
}
