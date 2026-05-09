// =============================================================================
// main.cpp
// MARS Test Application — Win32 entry point (M1)
//
// Creates a Win32 window, initialises the MARS renderer (D3D12 device,
// swap chain, clear-color present) and runs the message loop.
// =============================================================================

#include <windows.h>
#include <mars_engine/mars_engine.h>

#include <stdexcept>
#include <string>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static mars::Renderer g_renderer;
static bool           g_running       = true;
static uint32_t       g_client_width  = 1280;
static uint32_t       g_client_height = 720;

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
        if (w > 0 && h > 0 && g_renderer.display_output().swap_chain())
        {
            g_client_width  = w;
            g_client_height = h;
            g_renderer.on_resize(w, h);
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

    // Compute window size so the client area is exactly g_client_width x g_client_height.
    RECT rect = { 0, 0, static_cast<LONG>(g_client_width), static_cast<LONG>(g_client_height) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0,
        L"MarsTestApp",
        L"MARS 3D Engine - M1",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd)
        return -1;

    ShowWindow(hwnd, nShowCmd);
    UpdateWindow(hwnd);

    // Initialise renderer.
    try
    {
        g_renderer.init(hwnd, g_client_width, g_client_height);
    }
    catch (const std::exception& e)
    {
        MessageBoxA(hwnd, e.what(), "MARS Engine Init Failed", MB_ICONERROR | MB_OK);
        return -1;
    }

    // Main message + render loop.
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
                MessageBoxA(hwnd, e.what(), "MARS Render Error", MB_ICONERROR | MB_OK);
                g_running = false;
            }
        }
    }

    g_renderer.shutdown();
    return static_cast<int>(msg.wParam);
}
