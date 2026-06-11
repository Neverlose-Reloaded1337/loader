#pragma once

#include <string>

std::wstring GetCsgoInstallPath();
std::wstring GetCsgoExePath(const std::wstring& installPath);
std::wstring GetCsgoLibrariesPath(const std::wstring& installPath);
std::wstring GetCsgoScriptsPath(const std::wstring& installPath);
std::wstring GetCsgoConfigsPath(const std::wstring& installPath);
bool EnsureNlCloudFolder(const std::wstring& installPath);
