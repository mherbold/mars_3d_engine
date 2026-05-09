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
