/* Import address table hooks (pcrazy.exe, or a DLL it loaded). */
#include <string.h>
#include "pcpatch.h"

void *pc_hook_import(const char *dll, const char *name, void *replacement)
{
    return pc_hook_import_in(GetModuleHandleA(NULL), dll, name, replacement);
}

void *pc_hook_import_in(HMODULE module, const char *dll, const char *name, void *replacement)
{
    char *base = (char *)module;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);

    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA32 *names, *slots;
        if (lstrcmpiA(base + imp->Name, dll) != 0) {
            continue;
        }
        slots = (IMAGE_THUNK_DATA32 *)(base + imp->FirstThunk);
        if (!imp->OriginalFirstThunk) {
            /* no name table (the bound IAT is all that's left): match the resolved address */
            HMODULE mod = GetModuleHandleA(dll);
            uintptr_t target = mod ? (uintptr_t)GetProcAddress(mod, name) : 0;
            for (; target && slots->u1.Function; slots++) {
                if (slots->u1.Function == target) {
                    pc_write((uint32_t)(uintptr_t)&slots->u1.Function, &replacement, 4);
                    return (void *)target;
                }
            }
            continue;
        }
        names = (IMAGE_THUNK_DATA32 *)(base + imp->OriginalFirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            void *old;
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) {
                /* by ordinal (WSOCK32): match the address the name resolves to */
                HMODULE mod = GetModuleHandleA(dll);
                if (!mod || slots->u1.Function != (uintptr_t)GetProcAddress(mod, name)) {
                    continue;
                }
            } else if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, name) != 0) {
                continue;
            }
            old = (void *)(uintptr_t)slots->u1.Function;
            pc_write((uint32_t)(uintptr_t)&slots->u1.Function, &replacement, 4);
            return old;
        }
    }
    pc_log("iat: %s!%s not imported", dll, name);
    return NULL;
}
