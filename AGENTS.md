# AGENTS.md — Persistent Knowledge Base for the MARS 3D Engine Project

This file is maintained by AI coding agents to record persistent knowledge about
the project: coding conventions, toolchain decisions, architectural constraints,
and lessons learned.  Update it whenever a significant decision is made.

## Companion Documents
- **PLAN.md** — High-level feature and architecture plan for both projects.
- **PROGRESS.md** — Tracks what has been implemented, what is in-flight, and what is next.

---

## Agent Working Rules
- **Always update `PROGRESS.md`** at the end of every task — mark completed items ✅, update "Current Focus", and add any new decisions to the "Decisions Made" table.
- **Always update `PLAN.md`** if any architectural decision, design detail, or milestone scope changes during a task — keep it current and accurate.
- Never leave `PROGRESS.md` or `PLAN.md` stale after making code changes.

---


Two CMake sub-projects live under the repo root:
| Sub-project | Location | Purpose |
|---|---|---|
| `mars_engine` | `engine/` | Static/shared library — the full path-tracing 3D engine |
| `mars_test_app` | `test_app/` | Win32 executable — loads a scene and lets the user fly around |

---

## Language & Build Standards
- **Language:** C++23 (set in CMakeLists.txt via `CMAKE_CXX_STANDARD 23`)
- **Build system:** CMake ≥ 3.28, generator: *Visual Studio 18 2026*
- **CMake version in use:** 4.2.3-msvc3

---

## Graphics API & Ray-Tracing Stack
- **Primary GPU API:** DirectX 12 (D3D12) — no legacy fallback
- **Ray tracing:** DirectX Raytracing (DXR) — Tier 1.1 required
- **Shader model:** SM 6.6+ (HLSL compiled via DXC / `dxcompiler.dll`)
- **Target hardware:** NVIDIA RTX 4000/5000 series or equivalent AMD RDNA 3/4
- **HDR output: REQUIRED engine feature** — full HDR10/PQ (`DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`) and scRGB (`_G10_NONE_P709`) support must always be implemented and maintained. On non-HDR displays the engine gracefully activates an SDR tone-mapping path — but the HDR code path must never be removed or left unimplemented.
- No OpenGL, Vulkan, or D3D11 code should ever be added

## Key Third-Party Libraries (to be integrated)
| Library | Purpose |
|---|---|
| Windows Agility SDK (D3D12) | Latest D3D12 runtime features independent of OS |
| DirectXTK12 | Texture loading, sprite fonts, input helpers |
| DirectXMesh / DirectXTex | Mesh processing, texture compression |
| Assimp | Model and scene file import |
| nlohmann/json | Scene description, UI screen files, and config file parsing |
| ImGui | **Developer/debug overlay only** — never shown to end users; stripped in shipping builds |
| msdfgen | Offline build tool: generates MSDF glyph atlases from TTF/OTF fonts |
| HarfBuzz | Runtime Unicode text shaping for the game UI (OpenType, RTL/BiDi) |
| FreeType | Font metrics / glyph outlines consumed by msdfgen and HarfBuzz at build time |
| DirectXMath | Math types (SIMD-optimised for Windows/D3D12); do not introduce glm |

---

## Coding Conventions
- Header files under `engine/include/mars_engine/` — public API only
- Implementation files under `engine/src/` — mirrored sub-folder structure
- All public engine symbols exported via `MARS_ENGINE_API` macro (see `engine_api.h`)
- Use `#pragma once` — no include guards
- File-level block comment header (see existing files for style)
- Prefer `std::` types in non-hot paths; use raw D3D12/DXR types in renderer internals
- No global mutable state outside of the engine singleton (`MarsEngine` class)
- HLSL shaders live under `engine/shaders/` — one sub-folder per pass

---

## Naming Conventions
| Construct | Convention | Example |
|---|---|---|
| Classes / structs | PascalCase | `SceneManager`, `PathTracer` |
| Free functions | PascalCase | `MarsEngine_Initialize()` |
| Methods | camelCase | `loadScene()`, `renderFrame()` |
| Member variables | `m_` prefix + camelCase | `m_device`, `m_scenePath` |
| Constants / enums | ALL_CAPS or `k`PascalCase | `MAX_BOUNCES`, `kMaxBounces` |
| HLSL files | snake_case | `path_trace.hlsl`, `denoiser_temporal.hlsl` |

---

## Game UI vs. Developer Overlay — Critical Distinction
- **ImGui** = developer tool ONLY. Debug panels, GPU timers, scene tweakers. Compiled out of shipping builds via `MARS_ENABLE_DEV_UI` CMake option.
- **Game UI (`UISystem`)** = player-facing HUD, menus, overlays. A first-class engine subsystem rendered via a custom rasterization pass composited onto the path-traced frame.
  - Fonts: **MSDF** (Multi-channel Signed Distance Field) — generated offline by `msdfgen`, rendered by a custom HLSL shader. Crisp at any resolution/scale.
  - Text shaping: **HarfBuzz** for full Unicode, OpenType features, RTL/BiDi.
  - Layout: data-driven `.marsui` JSON screen files, hot-reloadable.
  - Theming: JSON style sheets (colors, spacing, animation curves).
  - HDR-aware: all UI colors in linear space, tone-mapped per display.

---

## Scene File Format
- Master scene files use `.marsscene` extension (JSON under the hood)
- Schema documented in `PLAN.md` §Scene Management

---

## Multi-Monitor Architecture
- Each physical monitor maps to one `DisplayOutput` object
- Each `DisplayOutput` owns a D3D12 swap chain and one or more `Camera` views
- The renderer dispatches one ray-tracing pass per active `DisplayOutput` per frame
- Monitor topology (angles, offsets) is described in the scene/config file

---

## Notes & Lessons Learned
*(append new entries here as the project evolves)*
- 2025-07: Initial scaffold created. No rendering code yet.
- 2025-07: **Milestone M0 complete.** All third-party deps wired (Assimp 5.4.3, nlohmann/json 3.11.3, D3D12MA 2.0.1, HarfBuzz 9.0.0, FreeType 2.13.3, msdfgen 1.12, ImGui 1.91.8). NuGet deps (Agility SDK 1.614.1, DirectXTK12/Tex/Mesh 2026.x) extracted to `C:/mars_deps/nuget/`. Streamline SDK v2.11.1 and Nsight Aftermath 2025.5.0 stored under `C:/mars_deps/`. DXC sourced from Windows SDK 10.0.26100.0.
- 2025-07: **Milestone M1 complete.** D3D12 device, three command queues, bindless CBV/SRV/UAV heap (1M slots), swap chain with HDR color-space detection, first clear-color present.
- 2025-07: **Milestone M2 complete.** `DisplayManager` / `DisplayOutput` multi-monitor system operational; `display.json` config parsed via nlohmann/json; one Win32 window per monitor; per-output swap chain and resize.
- 2025-07: **Milestone M3 complete.** `AssetImporter` (Assimp), `TextureLoader` (DirectXTex), `GpuMeshBuffer`, and `ResourceManager` (D3D12MA) all implemented and building cleanly. Bindless SRV registration working.
- 2025-07: **Milestone M4 complete.** Full DXR pipeline operational: RTPSO built from `path_trace.dxil` (lib_6_3), global root signature (bindless heap + frame-constants CB), shader tables (ray-gen / miss / hit-group), BLAS/TLAS from scene meshes, RGBA16F UAV per output, `DispatchRays` each frame. Closest-hit evaluates GGX/Smith/Fresnel PBR with bindless textures and traces a shadow ray. Results blit to swap-chain back buffer via `copy_to_back_buffer()`.
- 2025-07: FetchContent builds must use `FETCHCONTENT_UPDATES_DISCONNECTED ON` globally to prevent re-clone failures caused by OneDrive file locks on `C:/mars_build/fetchcontent`.
- 2025-07: Build/fetch directories moved to `C:/mars_build/` to avoid OneDrive locking FetchContent git pack files. Set in `CMakePresets.json`.
- 2025-07: Wide string literals in Win32 window titles must use ASCII-only characters; em dashes and other multi-byte UTF-8 characters cause garbled titles without explicit `/utf-8` MSVC flag.
- 2025-07: `engine/CMakeLists.txt` uses `GLOB_RECURSE` for source discovery but also maintains an explicit `list(APPEND)` for new files so they are included immediately without a CMake re-run.
- 2025-07: HarfBuzz compiled with `HB_HAVE_FREETYPE=OFF` and msdfgen with `MSDFGEN_CORE_ONLY=ON` until font pipeline work begins in M16.
- 2025-07: Camera system (`camera.h` / `camera.cpp`) is scaffolded but not yet implemented; full `Camera` class (view/proj matrices, motion vectors, jitter) is part of M5.
- 2025-07: **Streamline `slSetTag` resource state must match actual D3D12 state.** The `sl::Resource::state` field passed in `tag_resources` must reflect the resource's *current* D3D12 state at the point of tagging, not `0` / `COMMON`. Streamline uses this value to issue its own internal legacy barrier transitions before and after DLSS evaluation. Passing `0` when resources are in `D3D12_RESOURCE_STATE_UNORDERED_ACCESS` causes GPU-Based Validation error `#1358: GPU_BASED_VALIDATION_INCOMPATIBLE_TEXTURE_LAYOUT` inside the DLSS-RR compute dispatch, crashing `ExecuteCommandLists` with `0x87A`. All UAV-backed resources tagged by the engine must use `r.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS` — **except** `DenoisedOutputUAV` / `kBufferTypeScalingOutputColor` (see rule below).
- 2025-07: **`DenoisedOutputUAV` must be created in `D3D12_RESOURCE_STATE_COMMON` and reset to `D3D12_BARRIER_LAYOUT_COMMON` via Enhanced Barrier after each `slEvaluateFeature` call.** Streamline's `slEvaluateFeature` (DLSS-RR) writes to the denoised output texture using the D3D12 Enhanced Barrier API internally, leaving it in `D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS`. On the *next* frame, Streamline reads `r.state` and issues a legacy `ResourceBarrier` from that state. The D3D12 spec forbids legacy barriers on any resource whose current Enhanced layout is not `D3D12_BARRIER_LAYOUT_COMMON`, causing `RESOURCE_BARRIER_INVALID_COMBINATION` (#526) + `GPU_BASED_VALIDATION_INCOMPATIBLE_TEXTURE_LAYOUT` (#1358) → fatal `0x87A` crash at `ExecuteCommandLists` on every frame after the first. Correct pattern: (1) allocate `DenoisedOutputUAV` in `D3D12_RESOURCE_STATE_COMMON`; (2) tag it as `kBufferTypeScalingOutputColor` with `r.state = D3D12_RESOURCE_STATE_COMMON`; (3) after `slEvaluateFeature`, issue a `D3D12_TEXTURE_BARRIER` via `ID3D12GraphicsCommandList7::Barrier()` transitioning `D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS → D3D12_BARRIER_LAYOUT_COMMON`. `COMMON` is the only Enhanced layout that re-enables legacy barrier compatibility and also permits implicit SRV promotion for the tone-map blit that follows.
- 2025-07: **All required `sl::Constants` fields must be explicitly set before calling `slSetConstants` / `slEvaluateFeature`.** Streamline silently skips `slEvaluateFeature` if any of the following fields are left at their zero/default values: `cameraPos`, `cameraUp`, `cameraRight`, `cameraFwd`, `cameraFOV`, `cameraAspectRatio`, `cameraPinholeOffset`, `prevClipToClip`, `motionVectorsInvalidValue`. This causes the denoised output texture to remain in its last-known layout (e.g. `COMMON`), while a post-evaluate Enhanced Barrier that assumes the resource was written (`LayoutBefore=UAV`) fires unconditionally, producing `INCOMPATIBLE_BARRIER_LAYOUT` (#1334) and a `0x87A` crash. Always derive these from camera data: `cameraPos/Right/Up/Fwd` from `view_inv` rows; `cameraFOV` via `2*atan(1/proj.m[1][1])`; `cameraAspectRatio` via `proj.m[1][1]/proj.m[0][0]`; `prevClipToClip` as an explicit identity (not zero-init); `motionVectorsInvalidValue = 1e8f`.
- 2025-07: **After `slEvaluateFeature` (DLSS-RR), path-tracer input resources are left in `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE`, not `COMMON`.** Streamline internally reads the noisy color, motion vector, and depth resources as SRVs during the DLSS-RR pass and leaves them in `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE` (0x80). The post-denoise barrier block that returns these resources to `D3D12_RESOURCE_STATE_UNORDERED_ACCESS` for the next frame must therefore set `StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE`. Using `StateBefore = D3D12_RESOURCE_STATE_COMMON` produces `RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH #527` and a `0x87A` crash at `ResourceBarrier`.
- 2025-07: **Never alias the same `ID3D12Resource` under multiple `sl::BufferType` tags in a single `slSetTagForFrame` call.** Tagging `noisy_color` as `kBufferTypeScalingInputColor`, `kBufferTypeAlbedo`, `kBufferTypeSpecularAlbedo`, `kBufferTypeNormals`, and `kBufferTypeRoughness` simultaneously (5 tags → the same resource) causes Streamline to emit C(5,2)=10 `ResourceBarrier` calls on the same subresource. This produces `RESOURCE_BARRIER_DUPLICATE_SUBRESOURCE_TRANSITIONS` warnings AND corrupts Streamline's internal state-tracking for its own internal resources (e.g. `sl.dlss_d.mvec`), which then issues a barrier with `Before=COMMON` when the actual tracked state is `PIXEL_SHADER_RESOURCE` → `RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH #527` → D3D12 debug break → `0x87A` crash at `ExecuteCommandLists`. Rule: tag each physical resource under exactly one `sl::BufferType` per call. Only tag `kBufferTypeNormals` / `kBufferTypeRoughness` when dedicated G-buffer textures are provided; omit them entirely otherwise.
