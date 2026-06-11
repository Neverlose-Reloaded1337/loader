#include "library_sync.h"

#include <Windows.h>
#include <winhttp.h>

#include <cstdio>
#include <string>
#include <vector>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace
{
    constexpr const wchar_t* kGithubTreeUrl =
        L"https://api.github.com/repos/Neverlose-Reloaded1337/Neverlose-Reloaded/git/trees/main?recursive=1";
    constexpr const wchar_t* kRawBaseUrl =
        L"https://raw.githubusercontent.com/Neverlose-Reloaded1337/Neverlose-Reloaded/main/";
    constexpr const wchar_t* kLocalLibraryDir = L"nl_cloud\\scripts\\libraries";
    constexpr const char* kLibraryPrefix = "libraries/";
    constexpr const char* kExcludedPrefix = "libraries/open_source/";
    constexpr const wchar_t* kAvatarUrl =
        L"https://raw.githubusercontent.com/Neverlose-Reloaded1337/Neverlose-Reloaded/main/avatar.png";
    constexpr const wchar_t* kAvatarLocalPath = L"nl_cloud\\avatar.png";

    void Log(LibrarySyncLogCallback log, const char* message)
    {
        if (log)
            log(message);
    }

    void LogFormat(LibrarySyncLogCallback log, const char* format, const char* value)
    {
        if (!log)
            return;

        char buffer[512]{};
        std::snprintf(buffer, sizeof(buffer), format, value);
        log(buffer);
    }

    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
            return {};

        const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
        if (size <= 0)
            return {};

        std::wstring wide(size - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), size);
        return wide;
    }

    std::wstring UrlEncodePath(const std::string& path)
    {
        std::wstring encoded;
        constexpr char hex[] = "0123456789ABCDEF";

        for (const unsigned char ch : path)
        {
            if ((ch >= 'A' && ch <= 'Z') ||
                (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/')
            {
                encoded.push_back(static_cast<wchar_t>(ch));
            }
            else
            {
                encoded.push_back(L'%');
                encoded.push_back(static_cast<wchar_t>(hex[(ch >> 4) & 0xF]));
                encoded.push_back(static_cast<wchar_t>(hex[ch & 0xF]));
            }
        }

        return encoded;
    }

    bool CreateDirectoryRecursive(const std::wstring& path)
    {
        if (path.empty())
            return false;

        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
            return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
        {
            const std::wstring parent = path.substr(0, slash);
            if (!parent.empty() && !CreateDirectoryRecursive(parent))
                return false;
        }

        return CreateDirectoryW(path.c_str(), nullptr) != FALSE ||
            GetLastError() == ERROR_ALREADY_EXISTS;
    }

    std::wstring ParentDirectory(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return {};

        return path.substr(0, slash);
    }

    bool IsSafeRelativePath(const std::string& path)
    {
        return !path.empty() &&
            path.find("..") == std::string::npos &&
            path.find(':') == std::string::npos &&
            path.front() != '/' &&
            path.front() != '\\';
    }

    bool HttpGet(const std::wstring& url, std::vector<unsigned char>& data)
    {
        URL_COMPONENTS parts{};
        wchar_t host[256]{};
        wchar_t path[2048]{};

        parts.dwStructSize = sizeof(parts);
        parts.lpszHostName = host;
        parts.dwHostNameLength = _countof(host);
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = _countof(path);
        parts.dwSchemeLength = static_cast<DWORD>(-1);

        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
            return false;

        HINTERNET session = WinHttpOpen(
            L"nl-reloaded/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );
        if (!session)
            return false;

        HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
        if (!connect)
        {
            WinHttpCloseHandle(session);
            return false;
        }

        std::wstring pathAndQuery(path);
        if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0)
            pathAndQuery.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

        const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(
            connect,
            L"GET",
            pathAndQuery.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags
        );
        if (!request)
        {
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }

        const wchar_t headers[] = L"Accept: application/vnd.github+json\r\n";
        bool ok = WinHttpSendRequest(
            request,
            headers,
            static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0
        ) != FALSE && WinHttpReceiveResponse(request, nullptr) != FALSE;

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (ok)
        {
            ok = WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status,
                &statusSize,
                WINHTTP_NO_HEADER_INDEX
            ) != FALSE && status >= 200 && status < 300;
        }

        while (ok)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available))
            {
                ok = false;
                break;
            }

            if (available == 0)
                break;

            const size_t oldSize = data.size();
            data.resize(oldSize + available);

            DWORD read = 0;
            if (!WinHttpReadData(request, data.data() + oldSize, available, &read))
            {
                ok = false;
                break;
            }

            data.resize(oldSize + read);
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return ok;
    }

    std::string JsonStringValue(const std::string& object, const char* key)
    {
        const std::string needle = std::string("\"") + key + "\"";
        size_t pos = object.find(needle);
        if (pos == std::string::npos)
            return {};

        pos = object.find(':', pos + needle.size());
        if (pos == std::string::npos)
            return {};

        pos = object.find('"', pos + 1);
        if (pos == std::string::npos)
            return {};

        std::string value;
        bool escaped = false;
        for (++pos; pos < object.size(); ++pos)
        {
            const char ch = object[pos];
            if (escaped)
            {
                value.push_back(ch);
                escaped = false;
                continue;
            }

            if (ch == '\\')
            {
                escaped = true;
                continue;
            }

            if (ch == '"')
                break;

            value.push_back(ch);
        }

        return value;
    }

    std::vector<std::string> ParseLibraryPaths(const std::string& json)
    {
        std::vector<std::string> paths;
        size_t pos = 0;

        while ((pos = json.find('{', pos)) != std::string::npos)
        {
            const size_t end = json.find('}', pos + 1);
            if (end == std::string::npos)
                break;

            const std::string object = json.substr(pos, end - pos + 1);
            pos = end + 1;

            if (JsonStringValue(object, "type") != "blob")
                continue;

            const std::string path = JsonStringValue(object, "path");

            // Только libraries/, но не libraries/open_source/
            if (path.rfind(kLibraryPrefix, 0) != 0)
                continue;
            if (path.rfind(kExcludedPrefix, 0) == 0)
                continue;

            const std::string relative = path.substr(std::char_traits<char>::length(kLibraryPrefix));
            if (IsSafeRelativePath(relative))
                paths.push_back(relative);
        }

        return paths;
    }

    // Скачивает avatar.png и сохраняет в nl_cloud при каждом запуске
    void DownloadAvatar(const std::wstring& installPath, LibrarySyncLogCallback log)
    {
        const std::wstring destPath = installPath + L"\\" + kAvatarLocalPath;

        std::vector<unsigned char> data;
        if (!HttpGet(kAvatarUrl, data))
        {
            Log(log, "[-] Failed to download avatar.png.");
            return;
        }

        HANDLE file = CreateFileW(
            destPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (file == INVALID_HANDLE_VALUE)
        {
            Log(log, "[-] Failed to open avatar.png for writing.");
            return;
        }

        DWORD written = 0;
        const BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
        CloseHandle(file);

        if (!ok || written != data.size())
            Log(log, "[-] Failed to save avatar.png.");
        else
            Log(log, "[+] avatar.png updated.");
    }

    // Фоновая загрузка — вызывается в отдельном потоке
    void BackgroundSync(std::wstring installPath, LibrarySyncLogCallback log)
    {
        // Скачиваем/обновляем avatar.png при каждом запуске
        DownloadAvatar(installPath, log);

        const std::wstring targetDir = installPath + L"\\" + kLocalLibraryDir;
        if (!CreateDirectoryRecursive(targetDir))
        {
            Log(log, "[-] [bg] Failed to create libraries folder.");
            return;
        }

        std::vector<unsigned char> treeData;
        if (!HttpGet(kGithubTreeUrl, treeData))
        {
            Log(log, "[-] [bg] Failed to fetch libraries list.");
            return;
        }

        const std::string treeJson(treeData.begin(), treeData.end());
        const std::vector<std::string> libraries = ParseLibraryPaths(treeJson);
        if (libraries.empty())
        {
            Log(log, "[-] [bg] Libraries list is empty.");
            return;
        }

        int downloaded = 0;
        for (const std::string& relative : libraries)
        {
            const std::wstring localPath = targetDir + L"\\" + Utf8ToWide(relative);
            if (GetFileAttributesW(localPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                continue;

            const std::wstring url = std::wstring(kRawBaseUrl) + L"libraries/" + UrlEncodePath(relative);

            std::vector<unsigned char> fileData;
            if (!HttpGet(url, fileData))
            {
                LogFormat(log, "[-] [bg] Failed to download %s", relative.c_str());
                continue; // не прерываем — продолжаем остальные
            }

            if (!CreateDirectoryRecursive(ParentDirectory(localPath)))
            {
                LogFormat(log, "[-] [bg] Failed to create folder for %s", relative.c_str());
                continue;
            }

            HANDLE file = CreateFileW(
                localPath.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (file == INVALID_HANDLE_VALUE)
            {
                LogFormat(log, "[-] [bg] Failed to open file for %s", relative.c_str());
                continue;
            }

            DWORD written = 0;
            const BOOL writeOk = WriteFile(
                file,
                fileData.data(),
                static_cast<DWORD>(fileData.size()),
                &written,
                nullptr
            );
            CloseHandle(file);

            if (!writeOk || written != fileData.size())
            {
                LogFormat(log, "[-] [bg] Failed to save %s", relative.c_str());
                continue;
            }

            ++downloaded;
            LogFormat(log, "[+] [bg] Downloaded %s", relative.c_str());
        }

        char buffer[128]{};
        std::snprintf(buffer, sizeof(buffer), "[+] [bg] Libraries sync done. Downloaded: %d", downloaded);
        Log(log, buffer);
    }
}

bool SyncNeverloseLibraries(const std::wstring& installPath, LibrarySyncLogCallback log)
{
    if (installPath.empty())
    {
        Log(log, "[-] CS:GO install path is empty.");
        return false;
    }

    Log(log, "[*] Syncing script libraries...");

    // Запускаем в потоке чтобы не блокировать UI-рендер,
    // но join-им — инжект не начнётся пока синк не завершится.
    std::thread worker([installPath, log]()
        {
            BackgroundSync(installPath, log);
        });

    worker.join();

    return true;
}