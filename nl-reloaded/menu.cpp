#include <atomic>
#include <mutex>
#include <thread>
#include <d3d9.h>
#include <windowsx.h>
#include <string>
#include <vector>
#include <algorithm>
#include <winhttp.h>
#include <mmsystem.h>
#include <cstdio>

#include "menu.h"
#include "game_paths.h"
#include "workshop_sync.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "winmm.lib")

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    LPDIRECT3D9         g_d3d = nullptr;
    LPDIRECT3DDEVICE9   g_device = nullptr;
    D3DPRESENT_PARAMETERS g_d3dpp{};

    std::mutex               g_log_mutex;
    std::vector<std::string> g_logs;
    std::atomic_bool         g_running = false;
    std::atomic_bool         g_finished = false;
    std::atomic_int          g_result = 0;
    std::thread              g_worker;

    bool                     g_burmalda_enabled = false;
    LPDIRECT3DTEXTURE9       g_burmalda_tex = nullptr;
    int                      g_burmalda_tex_w = 0;
    int                      g_burmalda_tex_h = 0;
    std::wstring             g_burmalda_mp3_path;
    bool                     g_burmalda_playing = false;
    bool                     g_burmalda_pending_disable = false;

    constexpr const wchar_t* kBurmaldaImgUrl =
        L"https://raw.githubusercontent.com/Neverlose-Reloaded1337/Neverlose-Reloaded/main/mel.png";
    constexpr const wchar_t* kBurmaldaMp3Url =
        L"https://raw.githubusercontent.com/Neverlose-Reloaded1337/Neverlose-Reloaded/main/burmalda.mp3";
    constexpr const wchar_t* kBurmaldaCacheImg = L"nl_cloud\\mel.png";
    constexpr const wchar_t* kBurmaldaCacheMp3 = L"nl_cloud\\burmalda.mp3";

    bool BurmaldaHttpGet(const std::wstring& url, std::vector<unsigned char>& out)
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
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return false;

        HINTERNET session = WinHttpOpen(L"nl-reloaded/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;

        HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
        if (!connect) { WinHttpCloseHandle(session); return false; }

        const DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(connect, L"GET", path,
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }

        bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE
            && WinHttpReceiveResponse(request, nullptr) != FALSE;

        DWORD status = 0; DWORD statusSz = sizeof(status);
        if (ok) ok = WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSz,
            WINHTTP_NO_HEADER_INDEX) != FALSE && status >= 200 && status < 300;

        while (ok)
        {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail)) { ok = false; break; }
            if (!avail) break;
            const size_t old = out.size();
            out.resize(old + avail);
            DWORD read = 0;
            if (!WinHttpReadData(request, out.data() + old, avail, &read)) { ok = false; break; }
            out.resize(old + read);
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return ok;
    }

    bool SaveFile(const std::wstring& path, const std::vector<unsigned char>& data)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const BOOL ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
        CloseHandle(f);
        return ok && written == static_cast<DWORD>(data.size());
    }

    void BurmaldaLoadTexture(LPDIRECT3DDEVICE9 device, const std::wstring& imgPath)
    {
        if (g_burmalda_tex) { g_burmalda_tex->Release(); g_burmalda_tex = nullptr; }

        HANDLE hFile = CreateFileW(imgPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return;

        DWORD fileSize = GetFileSize(hFile, nullptr);
        if (fileSize == 0 || fileSize == INVALID_FILE_SIZE) { CloseHandle(hFile); return; }

        std::vector<unsigned char> fileData(fileSize);
        DWORD bytesRead = 0;
        if (!ReadFile(hFile, fileData.data(), fileSize, &bytesRead, nullptr) || bytesRead != fileSize)
        {
            CloseHandle(hFile);
            return;
        }
        CloseHandle(hFile);

        int w = 0, h = 0, channels = 0;
        unsigned char* pixels = stbi_load_from_memory(
            fileData.data(), static_cast<int>(fileData.size()),
            &w, &h, &channels, 4);

        if (!pixels) return;

        LPDIRECT3DTEXTURE9 tex = nullptr;
        HRESULT hr = device->CreateTexture(
            static_cast<UINT>(w), static_cast<UINT>(h),
            1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);

        if (SUCCEEDED(hr) && tex)
        {
            D3DLOCKED_RECT locked{};
            if (SUCCEEDED(tex->LockRect(0, &locked, nullptr, 0)))
            {
                auto* dst = static_cast<unsigned char*>(locked.pBits);
                for (int y = 0; y < h; ++y)
                {
                    auto* row = dst + y * locked.Pitch;
                    const auto* src = pixels + y * w * 4;
                    for (int x = 0; x < w; ++x)
                    {
                        row[x * 4 + 0] = src[x * 4 + 2];
                        row[x * 4 + 1] = src[x * 4 + 1];
                        row[x * 4 + 2] = src[x * 4 + 0];
                        row[x * 4 + 3] = src[x * 4 + 3];
                    }
                }
                tex->UnlockRect(0);
            }

            g_burmalda_tex = tex;
            g_burmalda_tex_w = w;
            g_burmalda_tex_h = h;
        }

        stbi_image_free(pixels);
    }

    void BurmaldaStartMusic(const std::wstring& mp3Path)
    {
        std::wstring cmd = L"open \"" + mp3Path + L"\" type mpegvideo alias burmalda";
        mciSendStringW(cmd.c_str(), nullptr, 0, nullptr);
        mciSendStringW(L"play burmalda repeat", nullptr, 0, nullptr);
        g_burmalda_playing = true;
    }

    void BurmaldaStopMusic()
    {
        mciSendStringW(L"stop burmalda", nullptr, 0, nullptr);
        mciSendStringW(L"close burmalda", nullptr, 0, nullptr);
        g_burmalda_playing = false;
    }

    void BurmaldaEnable(LPDIRECT3DDEVICE9 device, const std::wstring& installPath)
    {
        const std::wstring imgPath = installPath + L"\\" + kBurmaldaCacheImg;
        const std::wstring mp3Path = installPath + L"\\" + kBurmaldaCacheMp3;

        if (GetFileAttributesW(imgPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            std::vector<unsigned char> data;
            if (BurmaldaHttpGet(kBurmaldaImgUrl, data))
                SaveFile(imgPath, data);
        }

        if (GetFileAttributesW(mp3Path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            std::vector<unsigned char> data;
            if (BurmaldaHttpGet(kBurmaldaMp3Url, data))
                SaveFile(mp3Path, data);
        }

        BurmaldaLoadTexture(device, imgPath);
        g_burmalda_mp3_path = mp3Path;
        BurmaldaStartMusic(mp3Path);
    }

    void BurmaldaDisable()
    {
        BurmaldaStopMusic();
        g_burmalda_pending_disable = true;
    }

    void BurmaldaFlushPendingDisable()
    {
        if (!g_burmalda_pending_disable) return;
        g_burmalda_pending_disable = false;
        if (g_burmalda_tex) { g_burmalda_tex->Release(); g_burmalda_tex = nullptr; }
        g_burmalda_tex_w = g_burmalda_tex_h = 0;
    }

    void RenderBurmaldaBackground()
    {
        if (!g_burmalda_tex) return;

        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();

        const float sw = io.DisplaySize.x;
        const float sh = io.DisplaySize.y;

        const float scaleX = sw / static_cast<float>(g_burmalda_tex_w);
        const float scaleY = sh / static_cast<float>(g_burmalda_tex_h);
        const float scale = (scaleX > scaleY) ? scaleX : scaleY;

        const float iw = g_burmalda_tex_w * scale;
        const float ih = g_burmalda_tex_h * scale;
        const float ox = (sw - iw) * 0.5f;
        const float oy = (sh - ih) * 0.5f;

        dl->AddImage(
            reinterpret_cast<ImTextureID>(g_burmalda_tex),
            ImVec2(ox, oy), ImVec2(ox + iw, oy + ih),
            ImVec2(0, 0), ImVec2(1, 1),
            IM_COL32(255, 255, 255, 200)
        );
    }

    void AddLog(const char* message)
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        g_logs.emplace_back(message ? message : "");
    }

    struct WorkshopTabState
    {
        std::mutex                items_mutex;
        std::vector<WorkshopItem> items;
        bool                      items_loaded = false;

        std::mutex                dl_mutex;
        std::vector<bool>         downloading;

        std::atomic_bool          fetching = false;
        std::thread               fetch_worker;
    };

    WorkshopTabState g_scripts_tab;
    WorkshopTabState g_configs_tab;

    bool CreateDeviceD3D(HWND hwnd)
    {
        g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (!g_d3d) return false;

        ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
        g_d3dpp.Windowed = TRUE;
        g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
        g_d3dpp.EnableAutoDepthStencil = TRUE;
        g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
        g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

        return g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING,
            &g_d3dpp, &g_device) >= 0;
    }

    void CleanupDeviceD3D()
    {
        if (g_device) { g_device->Release(); g_device = nullptr; }
        if (g_d3d) { g_d3d->Release();    g_d3d = nullptr; }
    }

    void ResetDevice()
    {
        ImGui_ImplDX9_InvalidateDeviceObjects();
        if (g_device->Reset(&g_d3dpp) == D3DERR_INVALIDCALL) IM_ASSERT(0);
        ImGui_ImplDX9_CreateDeviceObjects();
    }

    void ApplyDefaultFlatStyle()
    {
        ImGuiStyle& s = ImGui::GetStyle();
        s = ImGuiStyle();
        s.WindowRounding = s.ChildRounding = s.FrameRounding = 0.0f;
        s.PopupRounding = s.ScrollbarRounding = s.GrabRounding = s.TabRounding = 0.0f;
    }

    void StartFetch(WorkshopTabState& tab, const char* githubPrefix)
    {
        if (tab.fetching.load()) return;
        if (tab.fetch_worker.joinable()) tab.fetch_worker.join();

        {
            std::lock_guard<std::mutex> lk(tab.items_mutex);
            tab.items.clear();
            tab.items_loaded = false;
        }
        {
            std::lock_guard<std::mutex> lk(tab.dl_mutex);
            tab.downloading.clear();
        }

        tab.fetching = true;
        tab.fetch_worker = std::thread([&tab, githubPrefix]()
            {
                std::vector<WorkshopItem> fetched;
                FetchWorkshopItems(githubPrefix, fetched, nullptr);

                std::scoped_lock lk(tab.items_mutex, tab.dl_mutex);
                tab.items = std::move(fetched);
                tab.downloading.assign(tab.items.size(), false);
                tab.items_loaded = true;
                tab.fetching = false;
            });
    }

    void RenderWorkshopSubTab(WorkshopTabState& tab,
        const char* githubPrefix,
        const std::wstring& localDir,
        const char* tabId)
    {
        if (!tab.items_loaded && !tab.fetching.load())
            StartFetch(tab, githubPrefix);

        ImGui::BeginChild((std::string("##items_") + tabId).c_str(),
            ImVec2(0.0f, 0.0f), false,
            ImGuiWindowFlags_HorizontalScrollbar);

        std::scoped_lock lk(tab.items_mutex, tab.dl_mutex);

        if (tab.fetching.load())
        {
            ImGui::TextDisabled("Loading...");
        }
        else if (!tab.items_loaded || tab.items.empty())
        {
            ImGui::TextDisabled("No items found.");
        }
        else
        {
            for (int i = 0; i < static_cast<int>(tab.items.size()); ++i)
            {
                const WorkshopItem& item = tab.items[i];
                const bool isLocal = IsWorkshopItemLocal(item, localDir);
                const bool isDownloading = tab.downloading[i];

                bool checked = isLocal;
                const std::string cbId = "##cb_" + std::to_string(i);

                if (isDownloading) ImGui::BeginDisabled();

                if (ImGui::Checkbox(cbId.c_str(), &checked))
                {
                    if (!isLocal && !isDownloading)
                    {
                        tab.downloading[i] = true;

                        WorkshopItem  itemCopy = item;
                        std::wstring  dirCopy = localDir;

                        std::thread([&tab, i, itemCopy, dirCopy]() mutable
                            {
                                DownloadWorkshopItem(itemCopy, dirCopy, nullptr);

                                std::lock_guard<std::mutex> dlk2(tab.dl_mutex);
                                tab.downloading[i] = false;
                            }).detach();
                    }
                    else if (isLocal && !isDownloading)
                    {
                        DeleteWorkshopItem(item, localDir);
                    }
                }

                if (isDownloading) ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::TextUnformatted(item.name.c_str());
            }
        }

        ImGui::EndChild();
    }

    void RenderWorkshopTab(const std::wstring& installPath)
    {
        if (ImGui::BeginTabBar("##workshop_tabs"))
        {
            if (ImGui::BeginTabItem("Scripts"))
            {
                RenderWorkshopSubTab(g_scripts_tab,
                    "workshop/scripts/",
                    GetCsgoScriptsPath(installPath),
                    "scripts");
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Configs"))
            {
                RenderWorkshopSubTab(g_configs_tab,
                    "workshop/configs/",
                    GetCsgoConfigsPath(installPath),
                    "configs");
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }

    // Returns true on success, false on failure.
    // Reads csgo/steam.inf from installPath, replaces ClientVersion=<anything> with ClientVersion=2000258.
    bool FixSteamInf(const std::wstring& installPath, std::string& outMsg)
    {
        if (installPath.empty())
        {
            outMsg = "Error: CS2 install path not found.";
            return false;
        }

        const std::wstring filePath = installPath + L"\\csgo\\steam.inf";

        // Read file
        HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            outMsg = "Error: Could not open steam.inf (file not found?).";
            return false;
        }

        DWORD fileSize = GetFileSize(hFile, nullptr);
        if (fileSize == 0 || fileSize == INVALID_FILE_SIZE)
        {
            CloseHandle(hFile);
            outMsg = "Error: steam.inf is empty or unreadable.";
            return false;
        }

        std::string content(fileSize, '\0');
        DWORD bytesRead = 0;
        const BOOL readOk = ReadFile(hFile, content.data(), fileSize, &bytesRead, nullptr);
        CloseHandle(hFile);

        if (!readOk || bytesRead != fileSize)
        {
            outMsg = "Error: Failed to read steam.inf.";
            return false;
        }

        // Replace ClientVersion=<value> with ClientVersion=2000258
        const std::string key = "ClientVersion=";
        const std::string newValue = "ClientVersion=2000258";
        const size_t pos = content.find(key);
        if (pos == std::string::npos)
        {
            outMsg = "Error: ClientVersion key not found in steam.inf.";
            return false;
        }

        // Find end of the value (newline or end of string)
        size_t end = content.find('\n', pos);
        if (end == std::string::npos) end = content.size();
        // Trim \r if present
        size_t valueEnd = end;
        if (valueEnd > pos && content[valueEnd - 1] == '\r') --valueEnd;

        content.replace(pos, valueEnd - pos, newValue);

        // Write file back
        HANDLE hWrite = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hWrite == INVALID_HANDLE_VALUE)
        {
            outMsg = "Error: Could not write steam.inf (run as admin?).";
            return false;
        }

        DWORD written = 0;
        const BOOL writeOk = WriteFile(hWrite, content.data(),
            static_cast<DWORD>(content.size()), &written, nullptr);
        CloseHandle(hWrite);

        if (!writeOk || written != static_cast<DWORD>(content.size()))
        {
            outMsg = "Error: Write to steam.inf failed.";
            return false;
        }

        outMsg = "OK: ClientVersion set to 2000258.";
        return true;
    }

    void RenderSettingsTab(LPDIRECT3DDEVICE9 device, const std::wstring& installPath)
    {
        bool burmalda = g_burmalda_enabled;
        if (ImGui::Checkbox("Burmalda Mod", &burmalda))
        {
            g_burmalda_enabled = burmalda;
            if (burmalda)
                BurmaldaEnable(device, installPath);
            else
                BurmaldaDisable();
        }

        ImGui::Spacing();

        if (ImGui::Button("Fix Version", ImVec2(200.0f, 28.0f)))
        {
            std::string msg;
            FixSteamInf(installPath, msg);
        }
    }

    void RenderMainWindow(MenuLoaderCallback loader)
    {
        const std::wstring installPath = GetCsgoInstallPath();

        BurmaldaFlushPendingDisable();

        if (g_burmalda_enabled)
            RenderBurmaldaBackground();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Neverlose Reloaded", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

        ImGui::TextUnformatted("Neverlose Reloaded");
        ImGui::SameLine(ImGui::GetWindowWidth() - 88.0f);
        if (ImGui::Button("x", ImVec2(80.0f, 0.0f)))
            PostQuitMessage(0);

        if (ImGui::BeginTabBar("##root"))
        {
            if (ImGui::BeginTabItem("Loader"))
            {
                const char* btn = g_running ? "Running..." : "Start";
                if (ImGui::Button(btn, ImVec2(120.0f, 32.0f)) && !g_running)
                {
                    if (g_worker.joinable()) g_worker.join();
                    { std::lock_guard<std::mutex> lk(g_log_mutex); g_logs.clear(); }
                    g_running = true; g_finished = false; g_result = 0;
                    g_worker = std::thread([loader]()
                        {
                            g_result = loader(AddLog);
                            g_running = false;
                            g_finished = true;
                        });
                }

                ImGui::SameLine();
                if (g_finished)      ImGui::Text("Result: %d", g_result.load());
                else if (g_running)  ImGui::TextUnformatted("Working");
                else                 ImGui::TextUnformatted("Ready");

                if (g_finished && g_result.load() == 0) PostQuitMessage(0);

                ImGui::Spacing();
                ImGui::BeginChild("log", ImVec2(0.0f, 0.0f), true,
                    ImGuiWindowFlags_HorizontalScrollbar);
                {
                    std::lock_guard<std::mutex> lk(g_log_mutex);
                    for (const std::string& line : g_logs)
                        ImGui::TextUnformatted(line.c_str());
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                    ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Workshop"))
            {
                RenderWorkshopTab(installPath);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Settings"))
            {
                RenderSettingsTab(g_device, installPath);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;

        switch (msg)
        {
        case WM_SIZE:
            if (g_device && wParam != SIZE_MINIMIZED)
            {
                g_d3dpp.BackBufferWidth = LOWORD(lParam);
                g_d3dpp.BackBufferHeight = HIWORD(lParam);
                ResetDevice();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_NCHITTEST:
        {
            const LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
            if (hit != HTCLIENT) return hit;
            POINT cur{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &cur);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            if (cur.y >= 0 && cur.y < 32 && cur.x < rc.right - 100)
                return HTCAPTION;
            return HTCLIENT;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default: break;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

}

int RunMenu(HINSTANCE instance, MenuLoaderCallback loader)
{
    WNDCLASSEXW wc{
        sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L,
        instance, nullptr, nullptr, nullptr, nullptr,
        L"nl_reloaded_menu", nullptr
    };
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
        L"nl-reloaded", WS_POPUP,
        100, 100, 520, 400,
        nullptr, nullptr, wc.hInstance, nullptr);

    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
    SetWindowPos(hwnd, nullptr, 100, 100, 520, 400,
        SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    ApplyDefaultFlatStyle();
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(g_device);

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderMainWindow(loader);

        ImGui::EndFrame();
        g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

        g_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
            D3DCOLOR_RGBA(18, 20, 24, 255), 1.0f, 0);

        if (g_device->BeginScene() >= 0)
        {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_device->EndScene();
        }

        const HRESULT hr = g_device->Present(nullptr, nullptr, nullptr, nullptr);
        if (hr == D3DERR_DEVICELOST &&
            g_device->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
            ResetDevice();
    }

    if (g_worker.joinable())                    g_worker.join();
    if (g_scripts_tab.fetch_worker.joinable())  g_scripts_tab.fetch_worker.join();
    if (g_configs_tab.fetch_worker.joinable())  g_configs_tab.fetch_worker.join();

    BurmaldaStopMusic();
    if (g_burmalda_tex) { g_burmalda_tex->Release(); g_burmalda_tex = nullptr; }

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return g_result;
}