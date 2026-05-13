#pragma once

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>

inline std::string g_auth_token_storage;
inline const char* auth_token = "";

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace token_detail
{
    inline std::string trim(std::string value)
    {
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch)
            {
                return !std::isspace(ch);
            }));

        value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch)
            {
                return !std::isspace(ch);
            }).base(), value.end());

        return value;
    }

    inline bool is_valid_token(const std::string& token)
    {
        if (token.size() < 32 || token.size() > 128)
            return false;

        return std::all_of(token.begin(), token.end(), [](unsigned char ch)
            {
                return std::isalnum(ch) || ch == '-';
            });
    }

    inline std::string token_file_path()
    {
        char module_path[MAX_PATH]{};
        if (!GetModuleFileNameA(reinterpret_cast<HMODULE>(&__ImageBase), module_path, MAX_PATH))
            return "neverlose_token.txt";

        std::string path = module_path;
        const size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos)
            path.erase(slash + 1);
        else
            path.clear();

        path += "neverlose_token.txt";
        return path;
    }

}

inline bool load_auth_token_from_disk()
{
    std::ifstream in(token_detail::token_file_path(), std::ios::binary);
    if (!in)
        return false;

    std::string token((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    token = token_detail::trim(token);
    if (!token_detail::is_valid_token(token))
        return false;

    g_auth_token_storage = std::move(token);
    auth_token = g_auth_token_storage.c_str();
    return true;
}

inline bool ensure_auth_token_loaded(bool force_prompt = false)
{
    if (!force_prompt && token_detail::is_valid_token(g_auth_token_storage))
    {
        auth_token = g_auth_token_storage.c_str();
        return true;
    }

    if (!force_prompt && load_auth_token_from_disk())
        return true;

    g_auth_token_storage.clear();
    auth_token = "";
    return false;
}
