# AGENTS.md — Persistent Knowledge Base for the MARS 3D Engine Project

This file is maintained by AI coding agents to record persistent knowledge about
the project: coding conventions, toolchain decisions, architectural constraints,
and lessons learned.  Update it whenever a significant decision is made.

## Companion Documents
- **PLAN.md** — High-level feature and architecture plan for both projects.
- **PROGRESS.md** — Tracks what has been implemented, what is in-flight, and what is next.

---

## Agent Working Rules
- **Always update `PROGRESS.md`** at the end of every task — mark completed items ✅, and follow the **Agent:** instructions in this file.
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

## DLSS-RR Temporal Contract (Invariants — Do Not Break)

These rules were established through extensive debugging and must all hold simultaneously for DLSS-RR to produce artifact-free output.

| Rule | Detail |
|---|---|
| **Depth = NDC projected depth** | Write `saturate(clip.z / clip.w)` to `kBufferTypeDepth`. Never write world-space ray distance (`hit_t`). Sky/miss pixels write `1.0`. |
| **`view_proj` = `proj * view`** | The C++ frame-constants upload must compute `view_proj = proj * view`. HLSL uses column-vector `mul(M, v)`, so the order on the CPU side must match. Same rule applies to `prev_view_proj`. |
| **Motion vectors = NDC delta, Y negated, ×0.5** | Compute `mv = (prevClip.xy/prevClip.w) - (currClip.xy/currClip.w)`, negate Y, then multiply by `{0.5, 0.5}`. Set `sl::DLSSDOptions::mvecScale = {0.5f, 0.5f}`. This converts DLSS-RR's screen-space [0,1] convention from NDC [-1,+1]. |
| **Full-scene motion vectors (`cameraMotionIncluded = eTrue`)** | Motion vectors must encode total pixel displacement including camera movement. Sky pixels must project a far-plane world point (not `rayDir`). |
| **Identity clip matrices when MVs include camera motion** | Set `sl::Constants::clipToPrevClip` and `prevClipToClip` to identity `float4x4(1,…)` whenever `cameraMotionIncluded = eTrue`. Passing real previous-frame matrices doubles the camera motion. |
| **`prev_view_proj` seeded on frame 0** | In `Renderer::set_camera()`, on the first call initialise `cam.prev_view_proj = curr_view_proj` to produce zero motion vectors on frame 0 and prevent NaN/Inf in DLSS-RR's history buffer. |
| **Render ≠ Display resolution** | Path-tracer noisy-color, MVs, depth, and AOVs are allocated at **render resolution** (DLSS input). Denoised output UAV is allocated at **display resolution** (DLSS output). Never mix the two sizes in the same allocation path. |
