#pragma once

#include <string>

using LibrarySyncLogCallback = void (*)(const char* message);

bool SyncNeverloseLibraries(const std::wstring& installPath, LibrarySyncLogCallback log);