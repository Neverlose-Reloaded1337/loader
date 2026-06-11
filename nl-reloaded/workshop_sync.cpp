#include "workshop_sync.h"

#include <Windows.h>
#include <winhttp.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace
{
    constexpr const wchar_t* kGithubTreeUrl =
        L"https://api.github.com/repos/Neverlose-Reloaded1337/Neverlose-Reloaded/git/trees/main?recursive=1";
    constexpr const wchar_t* kRawBaseUrl =
        L"https://raw.githubusercontent.com/Neverlose-Reloaded1337/Neverlose-Reloaded/main/";

    void Log(WorkshopLogCallback log, const char* message)
    {
        if (log) log(message);
    }

    void LogFormat(WorkshopLogCallback log, const char* fmt, const char* val)
    {
        if (!log) return;
        char buf[512]{};
        std::snprintf(buf, sizeof(buf), fmt, val);
        log(buf);
    }

    std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (n <= 0) return {};
        std::wstring w(n - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    }

    std::wstring UrlEncodePath(const std::string& path)
    {
        std::wstring encoded;
        constexpr char hex[] = "0123456789ABCDEF";
        for (const unsigned char ch : path)
        {
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
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

    bool IsSafeRelativePath(const std::string& p)
    {
        return !p.empty() &&
               p.find("..") == std::string::npos &&
               p.find(':')  == std::string::npos &&
               p.front() != '/' &&
               p.front() != '\\';
    }

    bool CreateDirectoryRecursive(const std::wstring& path)
    {
        if (path.empty()) return false;
        const DWORD attr = GetFileAttributesW(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES)
            return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

        const size_t sl = path.find_last_of(L"\\/");
        if (sl != std::wstring::npos)
        {
            const std::wstring parent = path.substr(0, sl);
            if (!parent.empty() && !CreateDirectoryRecursive(parent))
                return false;
        }
        return CreateDirectoryW(path.c_str(), nullptr) != FALSE ||
               GetLastError() == ERROR_ALREADY_EXISTS;
    }

    std::wstring ParentDir(const std::wstring& path)
    {
        const size_t sl = path.find_last_of(L"\\/");
        return sl == std::wstring::npos ? std::wstring{} : path.substr(0, sl);
    }

    // Strips "#number" suffix before the file extension.
    // "gazolina [source]_7018075#9.lua" -> "gazolina [source]_7018075.lua"
    std::string NormalizeName(const std::string& name)
    {
        const size_t dot = name.rfind('.');
        const size_t hash = name.rfind('#');

        if (hash == std::string::npos || hash == 0)
            return name;

        if (dot != std::string::npos && dot > hash)
        {
            const std::string between = name.substr(hash + 1, dot - hash - 1);
            bool allDigits = !between.empty();
            for (char c : between)
                if (c < '0' || c > '9') { allDigits = false; break; }

            if (allDigits)
                return name.substr(0, hash) + name.substr(dot);
        }
        else if (dot == std::string::npos)
        {
            const std::string after = name.substr(hash + 1);
            bool allDigits = !after.empty();
            for (char c : after)
                if (c < '0' || c > '9') { allDigits = false; break; }

            if (allDigits)
                return name.substr(0, hash);
        }

        return name;
    }


    // Returns true if the directory contains any file whose NormalizeName()
    // matches normalizedName. Handles the case where the file on disk still
    // has the "#N" suffix (e.g. "Eternal alpha#8.lua") but our item's
    // normalizedName is "Eternal alpha.lua".
    bool AnyVariantExistsInDir(const std::wstring& localDir,
                                const std::string&  normalizedName)
    {
        const std::wstring pattern = localDir + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;

        bool found = false;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            const std::wstring wFileName(fd.cFileName);
            const int sz = WideCharToMultiByte(CP_UTF8, 0,
                               wFileName.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (sz <= 0) continue;

            std::string u8(sz - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wFileName.c_str(), -1,
                                u8.data(), sz, nullptr, nullptr);

            if (NormalizeName(u8) == normalizedName)
            {
                found = true;
                break;
            }
        }
        while (FindNextFileW(h, &fd));

        FindClose(h);
        return found;
    }

    bool HttpGet(const std::wstring& url, std::vector<unsigned char>& data)
    {
        URL_COMPONENTS parts{};
        wchar_t host[256]{}, path[2048]{};
        parts.dwStructSize     = sizeof(parts);
        parts.lpszHostName     = host;
        parts.dwHostNameLength = _countof(host);
        parts.lpszUrlPath      = path;
        parts.dwUrlPathLength  = _countof(path);
        parts.dwSchemeLength   = static_cast<DWORD>(-1);

        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return false;

        HINTERNET session = WinHttpOpen(L"nl-reloaded/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;

        HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
        if (!connect) { WinHttpCloseHandle(session); return false; }

        std::wstring pq(path);
        if (parts.lpszExtraInfo && parts.dwExtraInfoLength)
            pq.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

        const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(connect, L"GET", pq.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }

        const wchar_t headers[] = L"Accept: application/vnd.github+json\r\n";
        bool ok = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1),
                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                  WinHttpReceiveResponse(request, nullptr);

        DWORD status = 0, statusSz = sizeof(status);
        if (ok)
            ok = WinHttpQueryHeaders(request,
                     WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                     WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSz,
                     WINHTTP_NO_HEADER_INDEX) &&
                 status >= 200 && status < 300;

        while (ok)
        {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail)) { ok = false; break; }
            if (!avail) break;
            const size_t old = data.size();
            data.resize(old + avail);
            DWORD read = 0;
            if (!WinHttpReadData(request, data.data() + old, avail, &read)) { ok = false; break; }
            data.resize(old + read);
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return ok;
    }

    std::string JsonStringValue(const std::string& obj, const char* key)
    {
        const std::string needle = std::string("\"") + key + "\"";
        size_t pos = obj.find(needle);
        if (pos == std::string::npos) return {};
        pos = obj.find(':', pos + needle.size());
        if (pos == std::string::npos) return {};
        pos = obj.find('"', pos + 1);
        if (pos == std::string::npos) return {};

        std::string value;
        bool escaped = false;
        for (++pos; pos < obj.size(); ++pos)
        {
            const char ch = obj[pos];
            if (escaped) { value.push_back(ch); escaped = false; continue; }
            if (ch == '\\') { escaped = true; continue; }
            if (ch == '"') break;
            value.push_back(ch);
        }
        return value;
    }

} // namespace

bool FetchWorkshopItems(const char*               githubPrefix,
                        std::vector<WorkshopItem>& out,
                        WorkshopLogCallback        log)
{
    Log(log, "[*] Fetching list from GitHub...");

    std::vector<unsigned char> treeData;
    if (!HttpGet(kGithubTreeUrl, treeData))
    {
        Log(log, "[-] Failed to fetch GitHub tree.");
        return false;
    }

    const std::string json(treeData.begin(), treeData.end());
    const size_t prefixLen = std::char_traits<char>::length(githubPrefix);

    size_t pos = 0;
    while ((pos = json.find('{', pos)) != std::string::npos)
    {
        const size_t end = json.find('}', pos + 1);
        if (end == std::string::npos) break;

        const std::string obj = json.substr(pos, end - pos + 1);
        pos = end + 1;

        if (JsonStringValue(obj, "type") != "blob") continue;

        const std::string path = JsonStringValue(obj, "path");
        if (path.rfind(githubPrefix, 0) != 0) continue;

        const std::string relative = path.substr(prefixLen);
        if (!IsSafeRelativePath(relative)) continue;

        const size_t sl = relative.find_last_of("/\\");
        const std::string rawName = sl == std::string::npos
            ? relative
            : relative.substr(sl + 1);

        const std::string normalized = NormalizeName(rawName);

        bool duplicate = false;
        for (const WorkshopItem& existing : out)
        {
            if (existing.normalizedName == normalized)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        WorkshopItem item;
        item.name           = normalized;
        item.githubPath     = path;
        item.relative       = relative;
        item.normalizedName = normalized;
        item.rawName        = rawName;
        out.push_back(std::move(item));
    }

    return true;
}

bool DownloadWorkshopItem(const WorkshopItem& item,
                          const std::wstring& localDir,
                          WorkshopLogCallback log)
{
    const std::wstring normalizedRelative = Utf8ToWide(item.normalizedName);
    const std::wstring localPath = localDir + L"\\" + normalizedRelative;

    const std::wstring parent = ParentDir(localPath);
    if (!parent.empty() && !CreateDirectoryRecursive(parent))
    {
        LogFormat(log, "[-] Failed to create folder for %s", item.name.c_str());
        return false;
    }

    const std::wstring url = std::wstring(kRawBaseUrl) + UrlEncodePath(item.githubPath);

    std::vector<unsigned char> data;
    if (!HttpGet(url, data))
    {
        LogFormat(log, "[-] Failed to download %s", item.name.c_str());
        return false;
    }

    HANDLE f = CreateFileW(localPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        LogFormat(log, "[-] Failed to open file for writing: %s", item.name.c_str());
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(f);

    if (!ok || written != static_cast<DWORD>(data.size()))
    {
        LogFormat(log, "[-] Failed to save %s", item.name.c_str());
        return false;
    }

    LogFormat(log, "[+] Downloaded %s", item.name.c_str());
    return true;
}

bool DeleteWorkshopItem(const WorkshopItem& item, const std::wstring& localDir)
{
    bool deleted = false;

    // Delete normalized name (e.g. "Eternal alpha.lua")
    const std::wstring normalized = localDir + L"\\" + Utf8ToWide(item.normalizedName);
    if (DeleteFileW(normalized.c_str()) != FALSE)
        deleted = true;

    // Delete raw name if different (e.g. "Eternal alpha#8.lua")
    if (item.rawName != item.normalizedName)
    {
        const std::wstring raw = localDir + L"\\" + Utf8ToWide(item.rawName);
        if (DeleteFileW(raw.c_str()) != FALSE)
            deleted = true;
    }

    // Scan and delete any remaining variant with a #N suffix that normalizes
    // to the same name (covers cases the two fast paths above miss).
    const std::wstring pattern = localDir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            const std::wstring wFileName(fd.cFileName);
            const int sz = WideCharToMultiByte(CP_UTF8, 0,
                               wFileName.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (sz <= 0) continue;

            std::string u8(sz - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wFileName.c_str(), -1,
                                u8.data(), sz, nullptr, nullptr);

            if (NormalizeName(u8) == item.normalizedName)
            {
                const std::wstring fullPath = localDir + L"\\" + wFileName;
                if (DeleteFileW(fullPath.c_str()) != FALSE)
                    deleted = true;
            }
        }
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    return deleted;
}

bool IsWorkshopItemLocal(const WorkshopItem& item, const std::wstring& localDir)
{
    // 1. Fast path: exact normalized name on disk.
    const std::wstring normalized = localDir + L"\\" + Utf8ToWide(item.normalizedName);
    if (GetFileAttributesW(normalized.c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;

    // 2. Fast path: exact raw name on disk (e.g. file still has #N suffix).
    if (item.rawName != item.normalizedName)
    {
        const std::wstring raw = localDir + L"\\" + Utf8ToWide(item.rawName);
        if (GetFileAttributesW(raw.c_str()) != INVALID_FILE_ATTRIBUTES)
            return true;
    }

    // 3. Slow path: scan the directory for any file that normalizes to the
    //    same name. Needed when GitHub lists only the clean name (no #N) but
    //    the file on disk still carries the suffix, or vice-versa.
    return AnyVariantExistsInDir(localDir, item.normalizedName);
}
