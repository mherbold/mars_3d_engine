// =============================================================================
// main.cpp
// MARS Test Application — Win32 entry point (M2)
//
// Creates one Win32 window per configured monitor, initialises the MARS
// renderer (D3D12 device, multi-monitor swap chains, clear-color present)
// and runs the message loop.
//
// Display configuration is read from display.json in the working directory.
// If the file is absent a single 1280×720 window is created on monitor 0.
// =============================================================================

#include <windows.h>
#include <mars_engine/mars_engine.h>

#include <stdexcept>
#include <string>
#include <vector>
#include <filesystem>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static mars::Renderer           g_renderer;
static bool                     g_running = true;

// Per-window state (one entry per DisplayOutput).
struct WindowState
{
    HWND     hwnd         = nullptr;
    uint32_t output_index = 0;
};
static std::vector<WindowState> g_windows;

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_SIZE:
    {
        uint32_t w = LOWORD(lparam);
        uint32_t h = HIWORD(lparam);
        if (w > 0 && h > 0 && g_renderer.display_manager().output_count() > 0)
        {
            // Find which output owns this HWND.
            for (const auto& ws : g_windows)
            {
                if (ws.hwnd == hwnd)
                {
                    g_renderer.on_resize(ws.output_index, w, h);
                    break;
                }
            }
        }
        return 0;
    }
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
        {
            g_running = false;
            DestroyWindow(hwnd);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ---------------------------------------------------------------------------
// create_window — helper to create one Win32 window
// ---------------------------------------------------------------------------
static HWND create_window(HINSTANCE hInstance, int nShowCmd,
                           const wchar_t* title,
                           uint32_t width, uint32_t height,
                           int x = CW_USEDEFAULT, int y = CW_USEDEFAULT)
{
    RECT rect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0,
        L"MarsTestApp",
        title,
        WS_OVERLAPPEDWINDOW,
        x, y,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);

    if (hwnd)
        ShowWindow(hwnd, nShowCmd);

    return hwnd;
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(
    _In_     HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_     LPSTR     /*lpCmdLine*/,
    _In_     int       nShowCmd)
{
    // Register window class.
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MarsTestApp";
    RegisterClassExW(&wc);

    // ---- Determine display configuration -----------------------------------
    std::string display_json_path = "display.json";
    bool        has_display_json  = std::filesystem::exists(display_json_path);

    // Load the config early so we know how many windows to create.
    std::vector<mars::DisplayConfig> configs;
    if (has_display_json)
        g_renderer.display_manager().load_config(display_json_path, configs);

    // Default: single 1280×720 window.
    if (configs.empty())
    {
        mars::DisplayConfig def;
        def.monitor_index = 0;
        def.width         = 1280;
        def.height        = 720;
        configs.push_back(def);
    }

    // ---- Create one Win32 window per display config ------------------------
    static const wchar_t* k_role_names[] = {
        L"MARS 3D Engine - M2 [Center]",
        L"MARS 3D Engine - M2 [Left]",
        L"MARS 3D Engine - M2 [Right]",
        L"MARS 3D Engine - M2 [Overhead]",
        L"MARS 3D Engine - M2 [Custom]",
    };

    std::vector<HWND> hwnds;
    hwnds.reserve(configs.size());

    for (uint32_t i = 0; i < static_cast<uint32_t>(configs.size()); ++i)
    {
        const auto& cfg = configs[i];
        uint32_t w = cfg.width  ? cfg.width  : 1280;
        uint32_t h = cfg.height ? cfg.height : 720;

        const wchar_t* title = (static_cast<size_t>(cfg.role) < std::size(k_role_names))
            ? k_role_names[static_cast<size_t>(cfg.role)]
            : k_role_names[0];

        HWND hwnd = create_window(hInstance, nShowCmd, title, w, h);
        if (!hwnd) return -1;

        UpdateWindow(hwnd);
        hwnds.push_back(hwnd);

        WindowState ws;
        ws.hwnd         = hwnd;
        ws.output_index = i;
        g_windows.push_back(ws);
    }

    // ---- Initialise renderer -----------------------------------------------
    try
    {
        if (has_display_json)
            g_renderer.init(display_json_path, hwnds);
        else
            g_renderer.init(hwnds[0], configs[0].width, configs[0].height);
    }
    catch (const std::exception& e)
    {
        MessageBoxA(nullptr, e.what(), "MARS Engine Init Failed", MB_ICONERROR | MB_OK);
        return -1;
    }

    // ---- Main message + render loop ----------------------------------------
    MSG msg{};
    while (g_running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
            {
                g_running = false;
                break;
            }
        }

        if (g_running)
        {
            try { g_renderer.render_frame(); }
            catch (const std::exception& e)
            {
                MessageBoxA(nullptr, e.what(), "MARS Render Error", MB_ICONERROR | MB_OK);
                g_running = false;
            }
        }
    }

    g_renderer.shutdown();
    return static_cast<int>(msg.wParam);
}

