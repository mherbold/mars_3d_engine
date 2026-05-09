# PROGRESS.md — MARS 3D Engine Implementation Progress

Last updated: 2025-07

---

## Legend
| Symbol | Meaning |
|---|---|
| ✅ | Complete |
| 🔄 | In progress |
| 🔲 | Not started |
| ❌ | Blocked / deferred |

---

## Current Focus
> **Nothing implemented yet.** Starting from the empty CMake scaffold.  
> Next step: **Milestone M0** — wire up third-party dependencies and the CMake shader compilation target.

---

## Milestone Status

| Milestone | Title | Status | Notes |
|---|---|---|---|
| M0 | Repo scaffold, CMake, third-party deps | 🔲 | |
| M1 | D3D12 device init, swap chain, clear-color present | 🔲 | |
| M2 | Multi-monitor display system | 🔲 | |
| M3 | Asset pipeline: FBX/glTF load + GPU upload | 🔲 | |
| M4 | DXR pipeline: primary rays, basic PBR hit shader | 🔲 | |
| M5 | Scene file parser, static scene rendering | 🔲 | |
| M6 | DLSS 4 integration (upscale, Multi Frame Generation, Ray Reconstruction) | 🔲 | Requires DLSS SDK NDA access |
| M7 | ReSTIR DI — direct lighting + ray-traced shadows | 🔲 | |
| M8 | ReSTIR GI — multi-bounce global illumination | 🔲 | |
| M9 | Animation system + skeletal mesh rendering | 🔲 | |
| M10 | Ecosystem / vegetation + wind | 🔲 | |
| M11 | Particle system | 🔲 | |
| M12 | Weather system (rain, clouds, fog) | 🔲 | |
| M13 | Decal system (tire tracks, skid marks) | 🔲 | |
| M14 | Crowd rendering | 🔲 | |
| M15 | Debug overlay, PIX, Aftermath, profiling *(dev-only)* | 🔲 | |
| M16 | Game UI system: MSDF fonts, widget tree, HUD | 🔲 | |
| M17 | Test app polish | 🔲 | |
| M18 | Performance tuning, peak-brightness calibration, HDR display polish | 🔲 | HDR support is cross-cutting from M1; M18 is final calibration only |

---

## Detailed Log

### M0 — Repo Scaffold, CMake, Third-Party Deps
- 🔲 Add Windows Agility SDK (D3D12) to CMakeLists
- 🔲 Add DirectXTK12, DirectXTex, DirectXMesh
- 🔲 Add Assimp via FetchContent
- 🔲 Add nlohmann/json via FetchContent
- 🔲 Add ImGui via FetchContent *(developer debug overlay only — not the game UI)*
- 🔲 Add HarfBuzz via FetchContent (Unicode text shaping for game UI)
- 🔲 Add FreeType via FetchContent (font metrics for msdfgen and HarfBuzz at build time)
- 🔲 Add msdfgen as CMake build-tool target (offline MSDF atlas generation)
- 🔲 Add D3D12 Memory Allocator (D3D12MA) via FetchContent
- 🔲 Add WinPixEventRuntime via NuGet
- 🔲 Add NVIDIA Aftermath SDK (manual — requires NDA download)
- 🔲 Add NVIDIA DLSS 4 SDK (manual — requires NDA download) — Multi Frame Generation, Transformer-based SR & Ray Reconstruction
- 🔲 CMake custom target: compile HLSL shaders via DXC
- 🔲 Create `engine/shaders/` directory structure
- 🔲 Verify full solution builds cleanly

### M1 — D3D12 Device Init, Swap Chain, Clear-Color Present
- 🔲 `DeviceContext` class: adapter enumeration, device creation, feature level check
- 🔲 Verify DXR Tier 1.1 support; hard-exit if not met
- 🔲 Three command queues (Direct, Compute, Copy)
- 🔲 Bindless CBV/SRV/UAV descriptor heap (1M slots)
- 🔲 `DisplayOutput` class: swap chain creation, resize handling
- 🔲 HDR color space detection (`CheckColorSpaceSupport`): activate HDR10/PQ or scRGB when available, SDR ACES fallback otherwise
- 🔲 First frame: clear back buffer to a color and present
- 🔲 Test app: Win32 window + message loop driving M1 code

### M2 — Multi-Monitor Display System
- 🔲 `DisplayManager`: enumerate `IDXGIOutput` objects
- 🔲 Parse `display.json` config
- 🔲 Create one `DisplayOutput` per enabled monitor
- 🔲 Per-display `Camera` with yaw/pitch/fov offsets
- 🔲 Test: render a different clear color on each monitor

### M3 — Asset Pipeline
- 🔲 `MeshLoader`: Assimp → interleaved vertex / index buffer on GPU
- 🔲 `TextureLoader`: DirectXTex → `ID3D12Resource` (BC7/BC6H DDS preferred)
- 🔲 `ResourceManager`: handle pool, upload heap, copy queue flush
- 🔲 Bindless registration of textures

### M4 — DXR Pipeline
- 🔲 DXR pipeline state object (RTPSO)
- 🔲 Shader table: ray-gen, miss, hit group records
- 🔲 BLAS build for static mesh
- 🔲 TLAS build
- 🔲 Ray-gen shader: one ray per pixel, write to UAV
- 🔲 Closest-hit shader: basic PBR (albedo × NdotL)
- 🔲 Miss shader: solid sky color
- 🔲 Display result via copy to swap chain back buffer

### M5 — Scene File Parser & Static Scene
- 🔲 `SceneLoader`: parse `.marsscene` JSON
- 🔲 Instantiate `MeshInstance`, `Light`, `Camera` objects
- 🔲 Build TLAS from loaded instances
- 🔲 Render a multi-object static scene

### M15 — Debug Overlay *(Developer-Only: ImGui, PIX, Aftermath)*
- 🔲 ImGui integration (debug builds only, stripped from shipping)
- 🔲 Per-pass GPU timer panels
- 🔲 WinPixEventRuntime GPU event markers
- 🔲 NVIDIA Aftermath crash dump integration
- 🔲 `--renderdoc` flag + `renderdoc.dll` injection support

### M16 — Game UI System
- 🔲 `UISystem` class: widget tree, per-frame dirty tracking
- 🔲 `UIRenderer`: upload geometry, rasterization pass, composite onto path-traced frame
- 🔲 MSDF font atlas pipeline: msdfgen offline tool → BC4 atlas + JSON metrics
- 🔲 HarfBuzz integration: Unicode shaping, OpenType features, RTL/BiDi
- 🔲 MSDF text shader (HLSL): correct alpha reconstruction, sub-pixel fringe
- 🔲 Core widgets: `Label`, `Image`, `Button`, `ProgressBar`, `Gauge`
- 🔲 Advanced widgets: `Label` multi-line, `Minimap` (secondary low-res ray-gen dispatch into UAV texture), `Leaderboard`, `Notification`, `Screen`
- 🔲 `.marsui` JSON screen file parser and hot-reload
- 🔲 Theme/style sheet system (JSON): colors, spacing, animation curves
- 🔲 HDR-aware color pipeline for UI (linear → tone map per display)
- 🔲 Per-monitor DPI scaling support

---

## Known Issues / Blockers
- DLSS 4 SDK and NVIDIA Aftermath SDK require NDA/registered developer access from NVIDIA.
  These will need to be downloaded manually and placed in `third_party/` before M6 and M15 can proceed.
- FSR 3 (AMD FidelityFX Super Resolution) is the open-source fallback for non-DLSS hardware;
  can be integrated via the GPUOpen FidelityFX SDK on GitHub.

---

## Decisions Made
| Date | Decision |
|---|---|
| 2025-07 | DirectX 12 + DXR chosen as the sole graphics API — no Vulkan, no OpenGL |
| 2025-07 | DLSS 4 as primary upscaler/denoiser/frame generator (Transformer model, Multi Frame Generation); FSR 4 / FSR 3 as open-source AMD fallback |
| 2025-07 | ReSTIR DI + ReSTIR GI chosen for lighting; no baked lightmaps |
| 2025-07 | `.marsscene` JSON format chosen for scene description files |
| 2025-07 | D3D12 Memory Allocator (D3D12MA) chosen for GPU memory management |
| 2025-07 | ImGui scoped to developer/debug builds only — not the game UI |
| 2025-07 | MSDF (Multi-channel Signed Distance Field) chosen for game UI font rendering |
| 2025-07 | HarfBuzz chosen for Unicode text shaping (OpenType, RTL/BiDi) |
| 2025-07 | Game UI described in `.marsui` JSON files; themed via JSON style sheets |
| 2025-07 | **HDR is a required engine feature** — HDR10/PQ and scRGB paths must always be fully implemented; engine gracefully falls back to SDR tone-mapping on non-HDR displays |
