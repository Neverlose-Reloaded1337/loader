#include <Windows.h>
#include <TlHelp32.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace
{
    constexpr const char* kDllName = "neverlose.dll";
    constexpr const char* kWindowClass = "Valve001";
    constexpr DWORD kProcessAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;

    LPVOID g_nt_open_file = GetProcAddress(LoadLibraryW(L"ntdll"), "NtOpenFile");

    void print_banner()
    {
        SetConsoleTitleA("neverpastelite");
        std::puts("fixed by bob and spiny <3");
        std::puts("            best loader                 ");
        std::puts(">.<");
        std::puts("Tip: if it fails right after token entry, just launch it again.");
        std::puts("");
    }

    void print_status(const char* label, const char* message)
    {
        std::printf("%s %s\n", label, message);
    }


    LPVOID GetModBase(DWORD pid, const wchar_t* name)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) return nullptr;

        MODULEENTRY32W me = { sizeof(me) };
        LPVOID base = nullptr;
        for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
        {
            if (!_wcsicmp(me.szModule, name))
            {
                base = me.modBaseAddr;
                break;
            }
        }
        CloseHandle(snap);
        return base;
    }

    void RestoreNtOpenFile(HANDLE hProcess)
        {
            HMODULE hNtdll = GetModuleHandleW(L"ntdll");
            LPVOID pLocal = GetProcAddress(hNtdll, "NtOpenFile");
            if (!pLocal) return;

            auto GetModBase = [](DWORD pid, const wchar_t* name) -> LPVOID {
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
                if (snap == INVALID_HANDLE_VALUE) return nullptr;
                MODULEENTRY32W me = { sizeof(me) };
                LPVOID base = nullptr;
                for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
                {
                    if (!_wcsicmp(me.szModule, name))
                    {
                        base = me.modBaseAddr;
                        break;
                    }
                }
                CloseHandle(snap);
                return base;
            };

            DWORD pid = GetProcessId(hProcess);
            LPVOID pRemote = GetModBase(pid, L"ntdll.dll");
            if (!pRemote) return;

            LPVOID target = (LPVOID)((uintptr_t)pRemote + ((uintptr_t)pLocal - (uintptr_t)hNtdll));

            char orig[5] = { 0 };

            wchar_t path[MAX_PATH];
            GetSystemDirectoryW(path, MAX_PATH);
            wcscat_s(path, L"\\ntdll.dll");

            HMODULE hFresh = LoadLibraryExW(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
            if (hFresh)
            {
                LPVOID pFn = GetProcAddress(hFresh, "NtOpenFile");
                if (pFn) memcpy(orig, pFn, 5);
                FreeLibrary(hFresh);
            }

            if (!*(DWORD*)orig)
                return;

            DWORD oldProt;
            if (VirtualProtectEx(hProcess, target, 5, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                WriteProcessMemory(hProcess, target, orig, 5, nullptr);
                VirtualProtectEx(hProcess, target, 5, oldProt, &oldProt);
            }
        }


    HWND wait_for_game_window(DWORD& process_id)
    {
        print_status("[*]", "Waiting for CS:GO...");

        HWND window = nullptr;
        while (!window)
        {
            window = FindWindowA(kWindowClass, nullptr);
            if (!window)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            GetWindowThreadProcessId(window, &process_id);
        }

        return window;
    }
}

int main()
{
    print_banner();

    char full_dll_path[MAX_PATH]{};
    DWORD process_id = 0;

    wait_for_game_window(process_id);
    std::printf("[+] Found CS:GO (PID: %lu)\n", process_id);

    if (!GetFullPathNameA(kDllName, MAX_PATH, full_dll_path, nullptr))
    {
        print_status("[-]", "Failed to resolve DLL path.");
        return 1;
    }

    std::printf("[+] DLL path: %s\n", full_dll_path);

    HANDLE process = OpenProcess(kProcessAccess, FALSE, process_id);
    if (!process || process == INVALID_HANDLE_VALUE)
    {
        print_status("[-]", "Failed to open process. Run as administrator.");
        return 1;
    }

    RestoreNtOpenFile(process);

    const SIZE_T path_length = std::strlen(full_dll_path) + 1;
    LPVOID remote_path = VirtualAllocEx(process, nullptr, path_length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remote_path)
    {
        print_status("[-]", "VirtualAllocEx failed.");
        CloseHandle(process);
        return 1;
    }

    std::printf("[+] Allocated remote memory at 0x%p\n", remote_path);

    if (!WriteProcessMemory(process, remote_path, full_dll_path, path_length, nullptr))
    {
        print_status("[-]", "WriteProcessMemory failed.");
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    FARPROC load_library = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    if (!load_library)
    {
        print_status("[-]", "Failed to locate LoadLibraryA.");
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    std::printf("[+] LoadLibraryA at 0x%p\n", reinterpret_cast<void*>(load_library));

    HANDLE remote_thread = CreateRemoteThread(
        process,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(load_library),
        remote_path,
        0,
        nullptr
    );

    if (!remote_thread || remote_thread == INVALID_HANDLE_VALUE)
    {
        print_status("[-]", "CreateRemoteThread failed.");
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    print_status("[*]", "Waiting for remote thread...");
    WaitForSingleObject(remote_thread, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeThread(remote_thread, &exit_code);
    std::printf("[+] LoadLibrary returned 0x%lX\n", exit_code);

    if (exit_code == 0)
        print_status("[-]", "DLL failed to load. Check the path and architecture.");
    else
        print_status("[+]", "DLL injected successfully.");

    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    CloseHandle(remote_thread);
    CloseHandle(process);

    std::puts("");
    print_status("[*]", "Closing in 2 seconds...");
    Sleep(2000);
    return exit_code == 0 ? 1 : 0;
}
