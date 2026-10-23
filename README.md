# Plane Crazy Comprehensive Modern Patch

This project is a comprehensive set of patches for the game Plane Crazy (Inner Workings, 1998) that aims to modernize
the game. The patches are applied on startup without making modifications to the original executable. The following
fixes/improvements are included:

- **Starts on modern PCs.** The game crashed at startup when Windows reported 16 or more audio playback devices and has been fixed.
- **Widescreen.** Full support for widescreen resolutions have been added.
- **Graphics options in the game.** Resolution and Color Depth settings have been added to the Options menu.
- **High Refresh Rate.** Frame Rate Limit and Vertical Sync are now available in the Options menu.
- **Network discovery in-game.** Network multiplayer game session discovery has been added to the in-game menus and no longer requires direct IP connection.
- **More players.** Network games now support up to 16 players.
- **Gamepads.** Support for modern controllers (XInput, PlayStation, Switch) with hotplug, analog steering and trigger throttle has been added by utilizing [SDL 2](https://libsdl.org).
- **Rumble.** The game's force feedback effects are played as controller rumble.
- **Relocated Saves.** Custom planes, ghosts and high scores are kept in `%USERPROFILE%\Saved Games\Inner Workings\Plane Crazy` instead of the install folder. Existing ones are copied over on first launch.
- **No registry.** The game's settings (`HKLM\Software\Inner Workings\Plane Crazy\1.0`) are stored in `Settings.ini` in the same folder as the saves and no longer require registry access.
- **Upscaled fonts.** The bitmap fonts used in game have been regenerated at a higher resolution for crisp text.

This build only supports the ZOOM Platform *Championship Edition* build of `pcrazy.exe` (PE timestamp `0x36a5f213`) and works alongside ZOOM's own fixes (`AllFixes.ini`: skip intro, cockpit camera, invert Y, no throttle axis).

## Install

Download `pcpatch-<version>.zip` from the
[releases](https://github.com/LANCommander/LANCommander.Patches.PlaneCrazy/releases) and extract it into the folder
that holds `pcrazy.exe` (it contains `dinput.dll`, `pcpatch.ini`, and this readme and the license). Delete
`dinput.dll` to uninstall. The patch logs to `pcpatch.log` next to the exe.

Multiplayer needs the Windows DirectPlay feature (*Turn Windows features on or off → Legacy Components →
DirectPlay*; Windows offers to install it the first time) and the firewall to let `pcrazy.exe` and `dplaysvr.exe`
through (UDP/TCP 47624, 2300-2400).

## Gamepad controls

| Pad | Race | Menus |
|---|---|---|
| Left stick / D-pad | steer (roll / pitch) | move |
| RT / LT | accelerate / brake | |
| A | fire powerup | select |
| B | fire cannon | back |
| X | air brakes | |
| Y | cockpit / chase view | |
| Start | in-flight menu (continue / quit / restart) | |

Rumble follows the game's own force feedback option (Options menu); `[Gamepad] Rumble` scales it.

## Settings

Every option is on by default and described in [`dist/pcpatch.ini`](dist/pcpatch.ini). Set a key to `0` to turn
that fix off (`MaxPlayers=8` gives the original limit).

| section | key | what it does |
|---|---|---|
| `[Sound]` | `LimitDevices` | Stops the sound device list at 16 entries (the startup crash fix) |
| `[Widescreen]` | `HorPlusFov` | Keeps the 4:3 vertical view on wide screens and widens the horizontal one |
| | `Center2D` | HUD, text and full-screen pictures at 4:3 in the middle of the screen instead of stretched |
| `[Display]` | `MenuOptions` | Resolution / Colour Depth in the Options menu (kept in Settings.ini `[Display]`) |
| `[Framerate]` | `MenuOptions` | Frame Rate Limit / Vertical Sync in the Options menu (kept in Settings.ini `[Display]`) |
| | `MaxFPS`, `VSync` | Defaults for those two items (`MaxFPS=0` = no limit: the display's refresh rate with vsync on) |
| | `RefreshRate` | Fullscreen refresh rate in Hz: `0` = the highest available (default), `-1` = leave it to Windows (the original) |
| `[Network]` | `Enable` | TCP/IP without `-m` or the protocol dialog |
| | `Host` | Look for games on this computer only (IP address or name) instead of the whole LAN |
| | `GameName` | Name of the games you host (empty = "&lt;pilot&gt;'s game") |
| | `Browser` | The "Network Game" list of games (`0` = the original single network-wide lobby) |
| `[Players]` | `MaxPlayers` | Players per network game, 8..16 (default 16). Everyone needs the same value |
| `[Gamepad]` | `UseSDL` | Gamepads through SDL; a wheel or joystick still works when no gamepad is attached |
| | `Deadzone` | Left stick dead zone in percent (default 15) |
| | `Rumble` | Rumble strength in percent (`0` = off) |
| | `InvertY` | Inverts the stick's up/down |
| | `CockpitViewButton` | Button for the cockpit/chase view toggle (default 4 = Y) |
| | `StartOpensMenu` | Start opens/closes the in-flight menu during a race |
| `[Saves]` | `Redirect` | Keep saves in the Saved Games folder (default on; `0` = the original place) |
| | `Folder` | Use this folder instead (empty = `Saved Games\Inner Workings\Plane Crazy`) |
| `[Registry]` | `UseSettingsFile` | Settings in `Settings.ini` instead of the registry |
| | `Import` | Copy the settings an earlier install left in the registry on the first start |
| `[Player]` | `Name` | Default pilot name (the game used the Windows registered owner; empty = the Windows user name) |

## Building

Requires MSYS2 with the 32-bit MinGW toolchain and SDL 2 (`C:\msys64\mingw32`,
`pacman -S mingw-w64-i686-gcc mingw-w64-i686-SDL2`). SDL is linked statically, so the patch stays a single DLL
(SDL is under the [zlib license](https://github.com/libsdl-org/SDL/blob/SDL2/LICENSE.txt)).

```
git clone https://github.com/LANCommander/LANCommander.Patches.PlaneCrazy.git
cd LANCommander.Patches.PlaneCrazy
sh build.sh
```

Output: `build\dinput.dll`, the patch (a `DirectInputCreateA` proxy that forwards to the system DLL).

`PC_VERSION=v1.2.3 sh build.sh` sets the version the patch writes to its log (`dev` otherwise).

GitHub Actions (`.github/workflows/build.yml`) builds every push and pull request. Pushing a semver tag
(`v1.2.3`, or `v1.2.3-rc.1` for a pre-release) builds that version and creates a draft release with the package
attached; publish it from the Releases page.

## Repository layout

| path | contents |
|---|---|
| `src/` | The patch: `pcpatch.c` (exe check, settings, guarded memory writes), `sound.c`, `widescreen.c`, `display.c`, `framerate.c`, `net.c`, `players.c`, `gamepad.c`, `menu.c` (Options menu items), `saves.c`, `registry.c`, `trace.c` (debug logging), `iat.c` (import table hooks), and the loader `dinput_proxy.c` |
| `dist/` | `pcpatch.ini`, shipped with the patch |
| `docs/` | Research notes on the game's internals for each feature |
| `tools/` | Development helpers (debugger, memory peek, UI driver, font export, network test peer) |

The game's bitmap fonts can be exported to PNG with `python -I tools/extract_fonts.py Game/data build/fonts`
(`docs/fonts.md`).

## License

[MIT](LICENSE). Plane Crazy itself is not included; you need your own copy of the game.
