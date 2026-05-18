// =============================================================================
// main.cpp
// MARS Test Application — Win32 entry point (M5)
//
// Creates one Win32 window per configured monitor, initialises the MARS
// renderer (D3D12 device, multi-monitor swap chains, DXR path tracing)
// and runs the message loop.
//
// Display configuration is read from display.json in the working directory.
// If the file is absent a single 1280x720 window is created on monitor 0.
//
// Scene is loaded from test_scene.marsscene in the working directory.
// If absent, the renderer falls back to clear-color present.
// =============================================================================

#include <windows.h>
#include <mars_engine/mars_engine.h>

#include <stdexcept>
#include <format>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>

// attach_debug_console — no longer spawns a console window; all engine log
// output is routed through OutputDebugStringA and appears in the VS Output
// (Debug) window via the MARS_LOG macro in the engine library.
static void attach_debug_console() {}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static mars::Renderer   g_renderer;
static mars::FlyCamera  g_fly_cam;
static bool             g_running      = true;

// Raw mouse input state
static bool  g_mouse_captured = false;
static float g_mouse_dx       = 0.0f;
static float g_mouse_dy       = 0.0f;
static bool  g_keys[256]      = {};

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
            for (const auto& ws : g_windows)
            {
                if (ws.hwnd == hwnd)
                {
                    g_renderer.on_resize(ws.output_index, w, h);
                    float aspect = static_cast<float>(w) / static_cast<float>(h);
                    g_fly_cam.set_aspect(aspect);
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
        if (wparam == VK_ESCAPE) { g_running = false; DestroyWindow(hwnd); }
        if (wparam == VK_F9)     { MARS_LOG("[Main] F9 pressed - calling save_screenshot(0)"); g_renderer.save_screenshot(0); MARS_LOG("[Main] save_screenshot(0) returned"); }
        if (wparam < 256) g_keys[wparam] = true;
        return 0;
    case WM_KEYUP:
        if (wparam < 256) g_keys[wparam] = false;
        return 0;
    case WM_LBUTTONDOWN:
    {
        // Capture mouse on left-click for fly cam.
        // Register for raw mouse input so deltas are cursor-position independent.
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;  // HID_USAGE_PAGE_GENERIC
        rid.usUsage     = 0x02;  // HID_USAGE_GENERIC_MOUSE
        rid.dwFlags     = RIDEV_INPUTSINK;
        rid.hwndTarget  = hwnd;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));

        SetCapture(hwnd);
        ShowCursor(FALSE);
        g_mouse_captured = true;

        // Centre cursor so it stays in the window
        RECT rc;
        GetClientRect(hwnd, &rc);
        POINT centre = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
        ClientToScreen(hwnd, &centre);
        SetCursorPos(centre.x, centre.y);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        // Unregister raw input
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;
        rid.usUsage     = 0x02;
        rid.dwFlags     = RIDEV_REMOVE;
        rid.hwndTarget  = nullptr;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));

        ReleaseCapture();
        ShowCursor(TRUE);
        g_mouse_captured = false;
        return 0;
    }
    case WM_INPUT:
    {
        if (!g_mouse_captured) return 0;
        UINT size = 0;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                        nullptr, &size, sizeof(RAWINPUTHEADER));
        if (size == 0) return 0;
        std::vector<BYTE> buf(size);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                            buf.data(), &size, sizeof(RAWINPUTHEADER)) != size)
            return 0;
        const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buf.data());
        if (raw->header.dwType == RIM_TYPEMOUSE)
        {
            g_mouse_dx += static_cast<float>(raw->data.mouse.lLastX);
            g_mouse_dy += static_cast<float>(raw->data.mouse.lLastY);
        }
        // Re-centre cursor to prevent it from leaving the window
        RECT rc;
        GetClientRect(hwnd, &rc);
        POINT centre = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
        ClientToScreen(hwnd, &centre);
        SetCursorPos(centre.x, centre.y);
        return 0;
    }
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
    attach_debug_console();

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

    std::vector<mars::DisplayConfig> configs;
    if (has_display_json)
        g_renderer.display_manager().load_config(display_json_path, configs);

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
        L"MARS 3D Engine - M5 [Center]",
        L"MARS 3D Engine - M5 [Left]",
        L"MARS 3D Engine - M5 [Right]",
        L"MARS 3D Engine - M5 [Overhead]",
        L"MARS 3D Engine - M5 [Custom]",
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

    // ---- Load scene --------------------------------------------------------
    std::string scene_path = "test_scene.marsscene";
    MARS_LOG("[Main] Working directory : '{}'", std::filesystem::current_path().string());
    MARS_LOG("[Main] Scene path        : '{}'", std::filesystem::absolute(scene_path).string());
    MARS_LOG("[Main] Scene file exists : {}", std::filesystem::exists(scene_path) ? "YES" : "NO");

    if (std::filesystem::exists(scene_path))
    {
        bool ok = g_renderer.load_scene(scene_path);
        MARS_LOG("[Main] load_scene() returned: {}", ok ? "true" : "false");
    }
    else
    {
        MARS_LOG("[Main] WARNING: scene file not found — renderer will show clear-color fallback.");
    }

    // ---- Initialise fly camera ---------------------------------------------
    {
        uint32_t w = configs[0].width  ? configs[0].width  : 1280;
        uint32_t h = configs[0].height ? configs[0].height :  720;
        float aspect = static_cast<float>(w) / static_cast<float>(h);

        // Use camera from scene if available, otherwise default
        mars::Vec3 cam_pos  = { 0.0f, 2.0f, 5.0f };
        float      fov      = 90.0f;
        if (!g_renderer.scene().cameras().empty())
        {
            const mars::CameraDesc& c = g_renderer.scene().cameras()[0];
            cam_pos = c.position;
            fov     = c.fov_deg;
        }
        // yaw=0° → fwd points in +Z; rays fire in -fwd=-Z, toward the scene from (0,1.5,5).
        // pitch=+11° → fwd.y > 0; rays tilt slightly down to match the scene look-at target.
        g_fly_cam.init(cam_pos, 0.0f, 11.0f, fov, aspect, 0.1f, 10000.0f);
    }

    // ---- Frame timer -------------------------------------------------------
    auto prev_time = std::chrono::steady_clock::now();

    // ---- FPS display in window title (diagnostic) -------------------------
    float  fps_accum    = 0.0f;
    int    fps_frames   = 0;
    float  fps_display  = 0.0f;
    float  peak_dt_ms   = 0.0f;
    float  fps_interval = 0.5f;  // update title every 0.5 s

    // ---- Main message + render loop ----------------------------------------
    MSG msg{};
    while (g_running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) { g_running = false; break; }
        }

        if (!g_running) break;

        auto   now = std::chrono::steady_clock::now();
        float  dt  = std::chrono::duration<float>(now - prev_time).count();
        dt         = std::min(dt, 0.1f);  // clamp to avoid spiral of death
        prev_time  = now;

        // Update FPS / peak-dt display in window title
        fps_accum  += dt;
        peak_dt_ms  = std::max(peak_dt_ms, dt * 1000.0f);
        ++fps_frames;
        if (fps_accum >= fps_interval)
        {
            fps_display = static_cast<float>(fps_frames) / fps_accum;
            const mars::Vec3 cam_pos = g_fly_cam.camera().position();
            wchar_t title[192];
            swprintf_s(title, L"MARS  |  %.1f fps  |  peak dt: %.1f ms  |  xyz: %.1f  %.1f  %.1f",
                       fps_display, peak_dt_ms,
                       cam_pos.x, cam_pos.y, cam_pos.z);
            SetWindowTextW(hwnds[0], title);
            fps_accum  = 0.0f;
            fps_frames = 0;
            peak_dt_ms = 0.0f;
        }

        // Build movement vector from WASD / QE / Space
        mars::Vec3 move = {};
        if (g_keys['W'])              move.z += 1.0f;
        if (g_keys['S'])              move.z -= 1.0f;
        if (g_keys['D'])              move.x += 1.0f;
        if (g_keys['A'])              move.x -= 1.0f;
        if (g_keys['E'] || g_keys[VK_SPACE]) move.y += 1.0f;
        if (g_keys['Q'])              move.y -= 1.0f;

        g_fly_cam.update(dt, move, g_mouse_dx, g_mouse_dy);
        g_mouse_dx = 0.0f;
        g_mouse_dy = 0.0f;

        // Push camera to renderer (all outputs share the same fly cam for now)
        g_fly_cam.camera().advance_frame(0);
        const mars::Camera& cam = g_fly_cam.camera();
        for (uint32_t oi = 0; oi < g_renderer.display_manager().output_count(); ++oi)
            g_renderer.set_camera(oi, cam.position(), cam.view_inv(), cam.proj_inv());

        g_renderer.update(dt);

        try { g_renderer.render_frame(); }
        catch (const std::exception& e)
        {
            MessageBoxA(nullptr, e.what(), "MARS Render Error", MB_ICONERROR | MB_OK);
            g_running = false;
        }
    }

    g_renderer.shutdown();
    return static_cast<int>(msg.wParam);
}
