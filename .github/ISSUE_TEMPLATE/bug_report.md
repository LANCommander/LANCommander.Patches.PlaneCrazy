---
name: Bug report
about: Something in the game doesn't work right with the patch
title: "[Bug]"
labels: ''
assignees: ''

---

**Describe the bug**
A clear and concise description of what the bug is.

**To Reproduce**
Steps to reproduce the behavior:
1. Start the game, go to '...'
2. Pick '....'
3. See error

**Expected behavior**
A clear and concise description of what you expected to happen.

**Without the patch**
Does it also happen with `dinput.dll` removed, or with the matching `pcpatch.ini` key set to `0`? [e.g. yes / no / not tried]

**Screenshots**
If applicable, add screenshots to help explain your problem.

**Logs**
Attach `pcpatch.log` from the game folder (it is rewritten at every start, so copy it right after the problem).
For crashes, also add the Windows Event Viewer entry (Windows Logs → Application, "Application Error" for `pcrazy.exe`).

**Versions:**
 - Patch version: [first line of `pcpatch.log`, e.g. `pcpatch v1.0.0`]
 - Game: [e.g. ZOOM Platform Championship Edition]
 - Windows: [e.g. Windows 11 24H2]
 - GPU: [e.g. NVIDIA RTX 4070]

**Additional Info**
- Resolution / colour depth / refresh rate: [e.g. 2560x1440, 32-bit, 144 Hz]
- Controller: [e.g. Xbox Series pad, DualSense, wheel, none]
- Network game: [e.g. no / LAN / internet via ZeroTier; number of players]
- Changed `pcpatch.ini` keys: [e.g. none, `MaxPlayers=8`]

**Additional context**
Add any other context about the problem here.
