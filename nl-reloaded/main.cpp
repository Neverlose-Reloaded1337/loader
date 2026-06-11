#include <Windows.h>
#include <TlHelp32.h>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>
#include <string>

#include "game_paths.h"
#include "library_sync.h"
#include "menu.h"
#include "dll_extractor.h"
#include "resource.h"

namespace
{
    constexpr const char* kDllName = "neverlose.dll";
    constexpr const char* kWindowClass = "Valve001";
    constexpr DWORD kProcessAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;

    LPVOID g_nt_open_file = GetProcAddress(LoadLibraryW(L"ntdll"), "NtOpenFile");

    void log_status(MenuLogCallback log, const char* label, const char* message)
    {
        char buffer[512]{};
        std::snprintf(buffer, sizeof(buffer), "%s %s", label, message);
        log(buffer);
    }

    void log_format(MenuLogCallback log, const char* format, ...)
    {
        char buffer[512]{};
        va_list args;
        va_start(args, format);
        std::vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        log(buffer);
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

    bool LaunchCsgo(const std::wstring& installPath)
    {
        const std::wstring exePath = GetCsgoExePath(installPath);
        if (exePath.empty())
            return false;

        std::wstring cmdLine = L"\"" + exePath + L"\" -insecure";

        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};

        if (!CreateProcessW(
            exePath.c_str(),
            cmdLine.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            installPath.c_str(),
            &si,
            &pi)) {
            return false;
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    HWND wait_for_game_window(DWORD& process_id, MenuLogCallback log)
    {
        log_status(log, "[*]", "Waiting for CS:GO...");

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

    void PatchUsername(const std::wstring& installPath, MenuLogCallback log)
    {
        const std::wstring jsonPath = installPath + L"\\nl_cloud\\global_data.json";

        HANDLE hFile = CreateFileW(jsonPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            log_status(log, "[-]", "global_data.json not found, skipping username patch.");
            return;
        }

        DWORD size = GetFileSize(hFile, nullptr);
        if (size == INVALID_FILE_SIZE || size == 0)
        {
            CloseHandle(hFile);
            return;
        }

        std::string content(size, '\0');
        DWORD read = 0;
        ReadFile(hFile, content.data(), size, &read, nullptr);
        CloseHandle(hFile);

        const std::string key = "\"username\":";
        size_t pos = content.find(key);
        if (pos == std::string::npos)
        {
            log_status(log, "[-]", "username key not found in global_data.json.");
            return;
        }

        size_t q1 = content.find('"', pos + key.size());
        if (q1 == std::string::npos) return;
        size_t q2 = content.find('"', q1 + 1);
        if (q2 == std::string::npos) return;

        content.replace(q1 + 1, q2 - q1 - 1, "Neverlose - Reloaded");

        HANDLE hWrite = CreateFileW(jsonPath.c_str(), GENERIC_WRITE, 0,
            nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hWrite == INVALID_HANDLE_VALUE)
        {
            log_status(log, "[-]", "Failed to open global_data.json for writing.");
            return;
        }

        DWORD written = 0;
        WriteFile(hWrite, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
        CloseHandle(hWrite);

        log_status(log, "[+]", "Username patched to 'Neverlose - Reloaded'.");
    }

    int RunLoader(MenuLogCallback log)
    {
        log_status(log, "[*]", "Neverlose Reloaded");
        log_status(log, "[*]", "Tip: if it fails right after token entry, launch it again.");

        DWORD process_id = 0;
        const std::wstring csgo_install_path = GetCsgoInstallPath();

        if (!EnsureNlCloudFolder(csgo_install_path))
        {
            log_status(log, "[-]", "Failed to find or create nl_cloud next to csgo.exe.");
            return 1;
        }

        log_status(log, "[+]", "nl_cloud folder is ready.");

        PatchUsername(csgo_install_path, log);

        log_status(log, "[*]", "Checking script libraries...");
        if (!SyncNeverloseLibraries(csgo_install_path, log))
            return 1;
        HWND existing_window = FindWindowA(kWindowClass, nullptr);

        if (!existing_window)
        {
            log_status(log, "[*]", "CS:GO is not running.");

            if (!LaunchCsgo(csgo_install_path))
            {
                log_status(log, "[-]", "Failed to launch CS:GO.");
                return 1;
            }

            log_status(log, "[*]", "Waiting for CS:GO to initialize...");
        }
        //fff
        wait_for_game_window(process_id, log);
        log_format(log, "[+] Found CS:GO (PID: %lu)", process_id);

        log_status(log, "[*]", "Extracting embedded DLL...");
        std::string full_dll_path = ExtractEmbeddedDll(IDR_DLL_NEVERLOSE);
        if (full_dll_path.empty())
        {
            log_status(log, "[-]", "Failed to extract embedded DLL from resources.");
            return 1;
        }

        log_format(log, "[+] DLL extracted to: %s", full_dll_path.c_str());

        HANDLE process = OpenProcess(kProcessAccess, FALSE, process_id);
        if (!process || process == INVALID_HANDLE_VALUE)
        {
            log_status(log, "[-]", "Failed to open process. Run as administrator.");
            return 1;
        }

        RestoreNtOpenFile(process);

        const SIZE_T path_length = full_dll_path.length() + 1;
        LPVOID remote_path = VirtualAllocEx(process, nullptr, path_length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!remote_path)
        {
            log_status(log, "[-]", "VirtualAllocEx failed.");
            CloseHandle(process);
            return 1;
        }

        log_format(log, "[+] Allocated remote memory at 0x%p", remote_path);

        if (!WriteProcessMemory(process, remote_path, full_dll_path.c_str(), path_length, nullptr))
        {
            log_status(log, "[-]", "WriteProcessMemory failed.");
            VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
            CloseHandle(process);
            return 1;
        }

        FARPROC load_library = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
        if (!load_library)
        {
            log_status(log, "[-]", "Failed to locate LoadLibraryA.");
            VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
            CloseHandle(process);
            return 1;
        }

        log_format(log, "[+] LoadLibraryA at 0x%p", reinterpret_cast<void*>(load_library));

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
            log_status(log, "[-]", "CreateRemoteThread failed.");
            VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
            CloseHandle(process);
            return 1;
        }

        log_status(log, "[*]", "Waiting for remote thread...");
        WaitForSingleObject(remote_thread, INFINITE);

        DWORD exit_code = 0;
        GetExitCodeThread(remote_thread, &exit_code);
        log_format(log, "[+] LoadLibrary returned 0x%lX", exit_code);

        if (exit_code == 0)
            log_status(log, "[-]", "DLL failed to load. Check the path and architecture.");
        else
            log_status(log, "[+]", "DLL injected successfully.");

        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(remote_thread);
        CloseHandle(process);

        // Удаляем временный файл DLL
        DeleteFileA(full_dll_path.c_str());

        return exit_code == 0 ? 1 : 0;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    return RunMenu(instance, RunLoader);
}