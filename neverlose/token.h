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

    struct PromptState
    {
        std::string initial_value;
        std::string result;
        HWND edit = nullptr;
        HFONT font = nullptr;
        bool accepted = false;
    };

    inline LRESULT CALLBACK prompt_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        auto* state = reinterpret_cast<PromptState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_NCCREATE:
        {
            auto* create = reinterpret_cast<CREATESTRUCTA*>(lparam);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }
        case WM_CREATE:
        {
            state = reinterpret_cast<PromptState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
            state->font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

            HWND label = CreateWindowExA(
                0,
                "STATIC",
                "Create or sign in on the website, then paste your token here.",
                WS_CHILD | WS_VISIBLE,
                16, 16, 388, 34,
                hwnd,
                nullptr,
                GetModuleHandleA(nullptr),
                nullptr
            );

            state->edit = CreateWindowExA(
                WS_EX_CLIENTEDGE,
                "EDIT",
                state->initial_value.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                16, 60, 388, 24,
                hwnd,
                reinterpret_cast<HMENU>(1001),
                GetModuleHandleA(nullptr),
                nullptr
            );

            HWND save = CreateWindowExA(
                0,
                "BUTTON",
                "Save Token",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                226, 100, 86, 26,
                hwnd,
                reinterpret_cast<HMENU>(IDOK),
                GetModuleHandleA(nullptr),
                nullptr
            );

            HWND cancel = CreateWindowExA(
                0,
                "BUTTON",
                "Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                318, 100, 86, 26,
                hwnd,
                reinterpret_cast<HMENU>(IDCANCEL),
                GetModuleHandleA(nullptr),
                nullptr
            );

            SendMessageA(label, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
            SendMessageA(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
            SendMessageA(save, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
            SendMessageA(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
            SetFocus(state->edit);
            return 0;
        }
        case WM_COMMAND:
        {
            const WORD id = LOWORD(wparam);
            if (id == IDOK && state && state->edit)
            {
                int length = GetWindowTextLengthA(state->edit);
                if (length > 0)
                {
                    std::string buffer(static_cast<size_t>(length) + 1, '\0');
                    GetWindowTextA(state->edit, buffer.data(), length + 1);
                    state->result.assign(buffer.c_str(), static_cast<size_t>(length));
                }
                else
                {
                    state->result.clear();
                }

                state->accepted = true;
                DestroyWindow(hwnd);
                return 0;
            }

            if (id == IDCANCEL)
            {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        }

        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }

    inline std::string prompt_for_token(const std::string& current_value)
    {
        const char* class_name = "NeverloseTokenPrompt";

        WNDCLASSEXA wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = prompt_wnd_proc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = class_name;
        RegisterClassExA(&wc);

        PromptState state{};
        state.initial_value = current_value;

        HWND hwnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
            class_name,
            "Neverlose Token",
            WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, 430, 170,
            GetForegroundWindow(),
            nullptr,
            GetModuleHandleA(nullptr),
            &state
        );

        if (!hwnd)
            return {};

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        MSG msg{};
        while (IsWindow(hwnd) && GetMessageA(&msg, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageA(hwnd, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
        }

        if (!state.accepted)
            return {};

        return trim(state.result);
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

inline bool save_auth_token_to_disk(const std::string& token)
{
    std::ofstream out(token_detail::token_file_path(), std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    out << token;
    return out.good();
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

    const std::string entered = token_detail::prompt_for_token(g_auth_token_storage);
    if (!token_detail::is_valid_token(entered))
    {
        MessageBoxA(
            GetForegroundWindow(),
            "Invalid token.",
            "Neverlose Token",
            MB_OK | MB_ICONWARNING
        );
        auth_token = "";
        return false;
    }

    g_auth_token_storage = entered;
    auth_token = g_auth_token_storage.c_str();

    save_auth_token_to_disk(g_auth_token_storage);

    std::printf("[token] loaded %zu-byte token\n", g_auth_token_storage.size());
    return true;
}
