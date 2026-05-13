#include "menu.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

// Internal one-time patch ignore this shit
static void PatchLogo()
{
    static bool patched = false;
    if (patched)
        return;

    patched = true;

    uintptr_t base = (uintptr_t)GetModuleHandle(NULL);

    // offset from your IDA: 412AE268 - 412A0000
    uintptr_t addr = base + 0x2AE268;

    DWORD oldProtect;
    if (VirtualProtect((void*)addr, 16, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        unsigned char patch[] =
        {
            // mov dword ptr [esi+20h], "test"
            0xC7, 0x46, 0x20, 0x74, 0x65, 0x73, 0x74,

            // mov byte ptr [esi+24h], 0
            0xC6, 0x46, 0x24, 0x00,

            // nop remaining
            0x90, 0x90, 0x90, 0x90
        };

        std::memcpy((void*)addr, patch, sizeof(patch));

        VirtualProtect((void*)addr, 16, oldProtect, &oldProtect);
    }
}

// Your existing function (we hook into execution flow here)
Menu* Menu::sub_412CE650()
{
    // Apply patch once when menu initializes
    PatchLogo();

    return this;
}