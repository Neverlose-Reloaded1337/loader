#include "game_paths.h"

#include <Windows.h>

namespace
{
    constexpr const wchar_t* kCsgoRegistryPath = L"SOFTWARE\\WOW6432Node\\Valve\\cs2";
    constexpr const wchar_t* kCsgoRegistryValue = L"installpath";
    constexpr const wchar_t* kCsgoExeName = L"csgo.exe";
    constexpr const wchar_t* kNlCloudFolderName = L"nl_cloud";
    constexpr const wchar_t* kScriptsFolderName = L"scripts";
    constexpr const wchar_t* kLibrariesFolderName = L"libraries";
}

std::wstring GetCsgoInstallPath()
{
    HKEY hKey;
    wchar_t steamPath[MAX_PATH]{};
    DWORD pathSize = sizeof(steamPath);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,   
        L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
        0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return {};

    const LSTATUS status = RegQueryValueExW(hKey, L"InstallPath", nullptr, nullptr,
        reinterpret_cast<LPBYTE>(steamPath), &pathSize);
    RegCloseKey(hKey);

    if (status != ERROR_SUCCESS || steamPath[0] == L'\0')
        return {};

    return std::wstring(steamPath) +
        L"\\steamapps\\common\\Counter-Strike Global Offensive";
}

std::wstring GetCsgoExePath(const std::wstring& installPath)
{
    if (installPath.empty())
        return {};

    return installPath + L"\\" + kCsgoExeName;
}

std::wstring GetCsgoLibrariesPath(const std::wstring& installPath)
{
    if (installPath.empty())
        return {};

    return installPath + L"\\" + kNlCloudFolderName + L"\\" + kScriptsFolderName + L"\\" + kLibrariesFolderName;
}

std::wstring GetCsgoScriptsPath(const std::wstring& installPath)
{
    if (installPath.empty())
        return {};

    return installPath + L"\\" + kNlCloudFolderName + L"\\" + kScriptsFolderName;
}

std::wstring GetCsgoConfigsPath(const std::wstring& installPath)
{
    if (installPath.empty())
        return {};

    constexpr const wchar_t* kConfigsFolderName = L"configs";
    return installPath + L"\\" + kNlCloudFolderName + L"\\" + kConfigsFolderName;
}

bool EnsureNlCloudFolder(const std::wstring& installPath)
{
    if (installPath.empty())
        return false;

    const std::wstring folderPath = installPath + L"\\" + kNlCloudFolderName;
    const DWORD attributes = GetFileAttributesW(folderPath.c_str());

    if (attributes != INVALID_FILE_ATTRIBUTES)
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    return CreateDirectoryW(folderPath.c_str(), nullptr) != FALSE ||
        GetLastError() == ERROR_ALREADY_EXISTS;
}
