/* dinput.dll proxy: pcrazy.exe imports DINPUT.dll!DirectInputCreateA, so Windows loads this
 * file from the game folder first. The real DLL is loaded from the system directory and the
 * call forwarded; DllMain applies the patches before pcrazy.exe's entry point runs. */
#include <windows.h>
#include <string.h>
#include "pcpatch.h"

typedef HRESULT (WINAPI *DirectInputCreateA_fn)(HINSTANCE, DWORD, void **, void *);
static DirectInputCreateA_fn real_DirectInputCreateA;

static FARPROC real_proc(const char *name)
{
    static HMODULE real;
    if (!real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);   /* SysWOW64 for a 32-bit process */
        lstrcpynA(path + n, "\\dinput.dll", MAX_PATH - n);
        real = LoadLibraryA(path);
        if (!real) {
            pc_log("cannot load %s (%lu)", path, GetLastError());
            return NULL;
        }
    }
    return GetProcAddress(real, name);
}

__declspec(dllexport) HRESULT WINAPI DirectInputCreateA(HINSTANCE inst, DWORD ver, void **out, void *outer)
{
    if (!real_DirectInputCreateA) {
        real_DirectInputCreateA = (DirectInputCreateA_fn)(void (*)(void))real_proc("DirectInputCreateA");
        if (!real_DirectInputCreateA) {
            return E_FAIL;
        }
    }
    {
        HRESULT hr = real_DirectInputCreateA(inst, ver, out, outer);
        if (hr == 0 && out) {
            *out = pc_gamepad_wrap(*out);
        }
        return hr;
    }
}

/* pcpatch.ini / pcpatch.log next to this DLL */
static void patch_init(HINSTANCE dll)
{
    char ini[MAX_PATH], log[MAX_PATH + 16], *slash;
    GetModuleFileNameA(dll, ini, MAX_PATH);
    slash = strrchr(ini, '\\');
    if (slash) {
        slash[1] = '\0';
    }
    strcpy(log, ini);
    strcat(ini, "pcpatch.ini");
    strcat(log, "pcpatch.log");
    DeleteFileA(log);
    pc_set_files(ini, log);

    if (pc_patch_start()) {
        pc_install();
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        patch_init(inst);
    }
    return TRUE;
}
