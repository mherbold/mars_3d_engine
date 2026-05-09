// =============================================================================
// display_manager.cpp
// MARS 3D Engine — Multi-Monitor Display Manager implementation
// =============================================================================

#include "mars_engine/renderer/display_manager.h"

#include <stdexcept>
#include <format>
#include <fstream>
#include <sstream>

// nlohmann/json is pulled in via FetchContent (available from M0).
// The header is accessed through the engine's include path.
#include <nlohmann/json.hpp>

namespace mars
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static MonitorRole role_from_string(const std::string& s)
{
    if (s == "left")     return MonitorRole::Left;
    if (s == "right")    return MonitorRole::Right;
    if (s == "overhead") return MonitorRole::Overhead;
    if (s == "custom")   return MonitorRole::Custom;
    return MonitorRole::Center;
}

// ---------------------------------------------------------------------------
// load_config
// ---------------------------------------------------------------------------
bool DisplayManager::load_config(const std::string& json_path, std::vector<DisplayConfig>& out_configs)
{
    std::ifstream file(json_path);
    if (!file.is_open())
        return false;

    try
    {
        nlohmann::json root = nlohmann::json::parse(file);

        const auto& displays = root.at("displays");
        for (const auto& d : displays)
        {
            DisplayConfig cfg;
            cfg.monitor_index     = d.value("monitor_index",     0u);
            cfg.role              = role_from_string(d.value("role", std::string("center")));
            cfg.yaw_offset_deg    = d.value("yaw_offset_deg",    0.0f);
            cfg.pitch_offset_deg  = d.value("pitch_offset_deg",  0.0f);
            cfg.roll_offset_deg   = d.value("roll_offset_deg",   0.0f);
            cfg.fov               = d.value("fov",               90.0f);
            cfg.width             = d.value("width",             0u);
            cfg.height            = d.value("height",            0u);
            out_configs.push_back(cfg);
        }
    }
    catch (const std::exception&)
    {
        out_configs.clear();
        return false;
    }

    return !out_configs.empty();
}

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------
void DisplayManager::init(DeviceContext&                   device_ctx,
                          const std::vector<DisplayConfig>& configs,
                          const std::vector<HWND>&          hwnds)
{
    if (m_initialised) return;

    std::vector<DisplayConfig> effective_configs = configs;

    // Default: single output on monitor 0 if nothing configured.
    if (effective_configs.empty())
    {
        DisplayConfig def;
        def.monitor_index = 0;
        effective_configs.push_back(def);
    }

    if (hwnds.size() < effective_configs.size())
        throw std::runtime_error("DisplayManager::init — not enough HWNDs provided");

    create_outputs(device_ctx, effective_configs, hwnds);
    m_initialised = true;
}

void DisplayManager::shutdown()
{
    if (!m_initialised) return;
    for (auto& output : m_outputs)
        output->shutdown();
    m_outputs.clear();
    m_configs.clear();
    m_initialised = false;
}

// ---------------------------------------------------------------------------
// resize
// ---------------------------------------------------------------------------
void DisplayManager::resize(uint32_t output_index, uint32_t width, uint32_t height)
{
    if (output_index >= m_outputs.size())
        return;
    m_outputs[output_index]->resize(width, height);
    m_configs[output_index].width  = width;
    m_configs[output_index].height = height;
}

// ---------------------------------------------------------------------------
// create_outputs (private)
// ---------------------------------------------------------------------------
void DisplayManager::create_outputs(DeviceContext&                   device_ctx,
                                    const std::vector<DisplayConfig>& configs,
                                    const std::vector<HWND>&          hwnds)
{
    // Enumerate all DXGI outputs on the selected adapter so we can map
    // monitor_index → DXGI output desktop coordinates / native resolution.
    std::vector<ComPtr<IDXGIOutput>> dxgi_outputs;
    {
        ComPtr<IDXGIOutput> out;
        for (UINT i = 0;
             device_ctx.adapter()->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND;
             ++i)
        {
            dxgi_outputs.push_back(out);
            out.Reset();
        }
    }

    m_outputs.reserve(configs.size());
    m_configs = configs;

    for (uint32_t i = 0; i < static_cast<uint32_t>(configs.size()); ++i)
    {
        const DisplayConfig& cfg = configs[i];
        HWND hwnd = hwnds[i];

        uint32_t w = cfg.width;
        uint32_t h = cfg.height;

        // If width/height not specified in config, derive from DXGI output or
        // fall back to the HWND client area.
        if (w == 0 || h == 0)
        {
            if (cfg.monitor_index < static_cast<uint32_t>(dxgi_outputs.size()))
            {
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(dxgi_outputs[cfg.monitor_index]->GetDesc(&desc)))
                {
                    RECT& r = desc.DesktopCoordinates;
                    w = static_cast<uint32_t>(r.right  - r.left);
                    h = static_cast<uint32_t>(r.bottom - r.top);
                }
            }

            // If DXGI query failed or monitor not found, use HWND client area.
            if (w == 0 || h == 0)
            {
                RECT client{};
                GetClientRect(hwnd, &client);
                w = static_cast<uint32_t>(client.right  - client.left);
                h = static_cast<uint32_t>(client.bottom - client.top);
            }

            // Final safety minimum.
            if (w == 0) w = 1280;
            if (h == 0) h = 720;
        }

        // Store resolved dimensions back into the configs copy.
        m_configs[i].width  = w;
        m_configs[i].height = h;

        auto display = std::make_unique<DisplayOutput>();
        display->init(device_ctx, hwnd, w, h);
        m_outputs.push_back(std::move(display));
    }
}

} // namespace mars
