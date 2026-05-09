// =============================================================================
// display_manager.h
// MARS 3D Engine — Multi-Monitor Display Manager
//
// DisplayManager enumerates DXGI outputs on startup and creates one
// DisplayOutput per monitor entry listed in the display configuration.
// Each DisplayOutput owns its own swap chain, HDR color-space detection,
// and per-frame RTV heap.
//
// Configuration (loaded from display.json or set programmatically):
//   {
//     "displays": [
//       { "monitor_index": 0, "role": "center",   "yaw_offset_deg":   0, "fov": 90 },
//       { "monitor_index": 1, "role": "left",     "yaw_offset_deg": -40, "fov": 50 },
//       { "monitor_index": 2, "role": "right",    "yaw_offset_deg":  40, "fov": 50 },
//       { "monitor_index": 3, "role": "overhead", "pitch_offset_deg": 70, "fov": 60 }
//     ]
//   }
//
// If no config is provided (or the config is empty), a single DisplayOutput
// is created for monitor 0 with role "center" and 90° FOV.
// =============================================================================

#pragma once

#include "../engine_api.h"
#include "../d3d12_agility.h"
#include "device_context.h"
#include "display_output.h"

#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace mars
{

using Microsoft::WRL::ComPtr;

// =============================================================================
// MonitorRole — semantic role of a monitor in a multi-display setup
// =============================================================================
enum class MonitorRole : uint8_t
{
    Center   = 0,
    Left     = 1,
    Right    = 2,
    Overhead = 3,
    Custom   = 4,
};

// =============================================================================
// DisplayConfig — configuration for a single monitor / DisplayOutput
// =============================================================================
struct DisplayConfig
{
    uint32_t    monitor_index     = 0;       // DXGI output index on the adapter
    MonitorRole role              = MonitorRole::Center;
    float       yaw_offset_deg   = 0.0f;    // Horizontal camera offset for surround
    float       pitch_offset_deg = 0.0f;    // Vertical camera offset (e.g. overhead)
    float       roll_offset_deg  = 0.0f;
    float       fov              = 90.0f;   // Horizontal field of view in degrees
    uint32_t    width            = 0;       // 0 = use monitor native resolution
    uint32_t    height           = 0;
};

// =============================================================================
// DisplayManager
// =============================================================================
class MARS_ENGINE_API DisplayManager
{
public:
    DisplayManager()  = default;
    ~DisplayManager() { shutdown(); }

    DisplayManager(const DisplayManager&)            = delete;
    DisplayManager& operator=(const DisplayManager&) = delete;

    // Enumerate DXGI outputs on the adapter and create one DisplayOutput per
    // entry in `configs`.  If `configs` is empty, a single default output is
    // created for monitor 0.
    //
    // Each HWND in `hwnds` corresponds to the DisplayConfig at the same index.
    // For multi-monitor use, pass one HWND per display.  For single-window
    // multi-viewport use, pass the same HWND for every entry.
    //
    // `device_ctx` must outlive this object.
    void init(DeviceContext&                   device_ctx,
              const std::vector<DisplayConfig>& configs,
              const std::vector<HWND>&          hwnds);

    void shutdown();

    // Call after all windows have been resized.  `output_index` is the index
    // into the outputs array, not the monitor index.
    void resize(uint32_t output_index, uint32_t width, uint32_t height);

    // Load display configuration from a JSON file.
    // Returns false if the file cannot be read; falls back to a single default output.
    bool load_config(const std::string& json_path, std::vector<DisplayConfig>& out_configs);

    // ---- Accessors ----------------------------------------------------------
    uint32_t         output_count()                  const { return static_cast<uint32_t>(m_outputs.size()); }
    DisplayOutput&   output(uint32_t index)                { return *m_outputs[index]; }
    const DisplayOutput& output(uint32_t index)      const { return *m_outputs[index]; }
    const DisplayConfig& config(uint32_t index)      const { return m_configs[index]; }

    // Convenience: iterate outputs
    std::vector<std::unique_ptr<DisplayOutput>>&       outputs()       { return m_outputs; }
    const std::vector<std::unique_ptr<DisplayOutput>>& outputs() const { return m_outputs; }

private:
    void create_outputs(DeviceContext&                   device_ctx,
                        const std::vector<DisplayConfig>& configs,
                        const std::vector<HWND>&          hwnds);

    std::vector<std::unique_ptr<DisplayOutput>> m_outputs;
    std::vector<DisplayConfig>                  m_configs;
    bool                                        m_initialised = false;
};

} // namespace mars
