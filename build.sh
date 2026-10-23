#!/bin/sh
# Builds build/dinput.dll (pcrazy.exe patch, dinput.dll proxy) with MSYS2 mingw32 gcc.
# PC_VERSION=v1.2.3 sets the version the patch logs (default "dev").
set -e
cd "$(dirname "$0")"
export PATH="/c/msys64/mingw32/bin:$PATH"
mkdir -p build
CFLAGS="-m32 -O2 -Wall -Wextra -shared -static-libgcc -s"
CORE="src/pcpatch.c src/iat.c src/sound.c src/saves.c src/registry.c src/trace.c src/gamepad.c src/widescreen.c src/players.c src/menu.c src/display.c src/net.c src/framerate.c"
# SDL2 (gamepads), linked statically: the patch stays a single DLL
SDL_PREFIX=$(sdl2-config --prefix)
CFLAGS="$CFLAGS -I$SDL_PREFIX/include/SDL2"
SDL_LIBS="$SDL_PREFIX/lib/libSDL2.a -lm -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8"
gcc $CFLAGS "-DPC_VERSION=\"${PC_VERSION:-dev}\"" -o build/dinput.dll \
    src/dinput_proxy.c $CORE src/dinput.def -Wl,--enable-stdcall-fixup -Wl,--kill-at -lshlwapi $SDL_LIBS
objdump -p build/dinput.dll | grep -A3 "Ordinal/Name Pointer" | sed -n 3p
