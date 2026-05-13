#include <windows.h>

#include "neverlose.h"
#include "internal_fixes.h"
#include "token.h"

// main logic thread you dont understand the rage, i have felt from this cheat.
NTSTATUS NTAPI MainThread(LPVOID lpThreadParameter)
{
    ensure_auth_token_loaded();

    // map manually
    g_neverlose.map((HMODULE)lpThreadParameter);

    // wait for serverbrowser.dll
    while (!GetModuleHandleW(L"serverbrowser.dll"))
        Sleep(100);

    // setup sequence
    g_neverlose.fix_dump();
    g_neverlose.set_veh();
    g_neverlose.setup_hooks();
    g_neverlose.spoof();

    // wait 10 seconds before running entry
    Sleep(10000);

    // run main entry
    g_neverlose.entry();

    return STATUS_SUCCESS;
}

// dll entry point
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinstDLL);

        HANDLE hThread = NULL;

        NTSTATUS status = NtCreateThreadEx(
            &hThread,
            THREAD_ALL_ACCESS,
            NULL,
            NtCurrentProcess(),
            MainThread,
            hinstDLL,
            THREAD_CREATE_FLAGS_NONE,
            0, 0, 0,
            NULL
        );

        if (NT_SUCCESS(status) && hThread)
        {
            NtClose(hThread);
        }
        else
        {
            return FALSE;
        }
    }

    return TRUE;
}
