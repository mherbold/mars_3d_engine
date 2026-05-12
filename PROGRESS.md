# PROGRESS.md — MARS 3D Engine Implementation Progress

Last updated: 2025-07 (M6 — DLSS 4 integration **complete**; app running at normal frame rate with no exceptions; all five crash bugs fixed; stable 44-second run validated; remaining Streamline/D3D12 warnings are benign — see Stable Runtime section below)

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

**Status:** M6 DLSS 4 integration — **complete**. All five crash bugs fixed; stable runtime validated (44-second run, normal frame rate, no exceptions thrown). Remaining Streamline/D3D12 log messages are benign — documented below.

### Bug 1 — FIXED ✅ — 0x87A crash at `ExecuteCommandLists` (duplicate resource tags corrupting Streamline mvec barrier state)

**Root cause:** `denoiser.cpp` `tag_resources()` aliased `noisy_color` as `kBufferTypeAlbedo`, `kBufferTypeSpecularAlbedo`, and (via fallback) `kBufferTypeNormals` / `kBufferTypeRoughness` in addition to `kBufferTypeScalingInputColor`. Tagging the same `ID3D12Resource` under 5 buffer types caused Streamline to emit C(5,2)=10 duplicate `ResourceBarrier` calls on the same subresource (`RESOURCE_BARRIER_DUPLICATE_SUBRESOURCE_TRANSITIONS`). This corrupted Streamline's internal state machine for `sl.dlss_d.mvec`, which then issued a barrier with `Before=COMMON` (0x0) when the actual D3D12-tracked state was `PIXEL_SHADER_RESOURCE` (0x80), triggering `RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH #527`. The D3D12 debug layer has break-on-error enabled for `#527`, which raised a `0x87A` structured exception at `ExecuteCommandLists`.

**Fix:** Removed `kBufferTypeAlbedo` / `kBufferTypeSpecularAlbedo` tags entirely from `denoiser.cpp`, and suppressed `kBufferTypeNormals` / `kBufferTypeRoughness` when no dedicated G-buffer resources are provided (`normals == nullptr && roughness == nullptr`).

**Rule established (see AGENTS.md):** Never alias the same `ID3D12Resource` under multiple `sl::BufferType` values in a single `slSetTagForFrame` call.

---

### Bug 3 — FIXED ✅ — 0x87A crash: `GPU_BASED_VALIDATION_INCOMPATIBLE_TEXTURE_LAYOUT` on `DenoisedOutputUAV` when `slEvaluateFeature` fails

**Root cause:** Streamline's internal `sl.dlss_d.mvec` resource was left in `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE` (0x80) at the end of frame N. At the start of frame N+1 Streamline's DLSS-RR plugin issued an internal legacy barrier assuming `Before=COMMON` (0x0), triggering `RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH #527`. The debug layer broke on `#527`, causing `slEvaluateFeature` to return a failure code, so `rr_evaluated = false`. Because `rr_evaluated = false`, the post-evaluate Enhanced Barrier (UAV→COMMON layout transition for `DenoisedOutputUAV`) was correctly skipped. However `use_denoised` was computed independently (`is_initialised() && denoised_resource != nullptr`) so it remained `true`. `copy_to_back_buffer` then bound `DenoisedOutputUAV` as an SRV while it was still in `D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS` (left by the previous successful frame), triggering `GPU_BASED_VALIDATION_INCOMPATIBLE_TEXTURE_LAYOUT #1358` and the 0x87A crash at `ExecuteCommandLists`.

**Fix:** `use_denoised` in `render_frame_path_traced` now also requires `rr_evaluated`:
```cpp
bool use_denoised = rr_evaluated &&
                    m_path_tracer.denoised_output_resource(oi) != nullptr;
```
`rr_evaluated` was moved outside the `if (m_denoiser.is_initialised())` block (initialised to `false`) so it is visible at the `use_denoised` call site. When `slEvaluateFeature` fails, both the Enhanced Barrier and the denoised readback are skipped, and the renderer falls back to the raw path-tracer UAV output.

---

### Bug 4 — FIXED ✅ — 0x87A crash: `RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH` on `DenoisedOutputUAV` on frames after a failed `slEvaluateFeature`

**Root cause:** Streamline **always** issues a legacy `COMMON → UAV` barrier on `DenoisedOutputUAV` before writing it, regardless of whether `slEvaluateFeature` subsequently succeeds or fails. After Bug 3's fix, when `rr_evaluated = false` the post-evaluate Enhanced Barrier (UAV → COMMON) was correctly skipped, but `DenoisedOutputUAV` was already in `D3D12_RESOURCE_STATE_UNORDERED_ACCESS` (from SL's partial execution). On the next frame, Streamline again attempted its `COMMON → UAV` barrier, but the actual D3D12-tracked state was `UAV` (0x8), causing `RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH #527` → 0x87A crash at `ExecuteCommandLists`.

**Fix:** Added `std::vector<bool> m_denoised_in_uav_state` (per output) to `Renderer`. The flag is set to `true` only when `rr_evaluated == true` (i.e. after a confirmed successful `slEvaluateFeature` call — see Bug 5 for why the original unconditional set was wrong). On `rr_evaluated == true` the existing Enhanced Barrier fires and the flag is cleared. On `rr_evaluated == false` with the flag set, a legacy `UAV → COMMON` cleanup barrier is issued and the flag is cleared, ensuring `DenoisedOutputUAV` is always in `COMMON` state before the next frame's Streamline barriers. Flag is also reset to `false` in `on_resize` since the texture is recreated in `COMMON`.

---

### Bug 2 — FIXED ✅ — Streamline errors: `Tag of buffer kBufferTypeAlbedo not set for frame N, viewport 0`

**Observed:** Every frame Streamline logged:
```
[streamline][error] resourceTaggingForFrame.cpp:309 Tag of buffer kBufferTypeAlbedo not set for frame N, viewport 0
[streamline][error] commonInterface.h:156 Failed to find global tag 'kBufferTypeAlbedo', please make sure to tag all required buffers
```
Also observed once per session: `Repeated slDLSSGSetOptions() call for frame 1` (benign race between present-hook and app code).

**Root cause:** `PathTracer` did not allocate dedicated AOV textures for albedo, specular albedo, world-space normals, or roughness. The DLSS-RR plugin (`sl.dlss_d`) requires `kBufferTypeAlbedo` as a mandatory input on every `slEvaluateFeature` call.

**Fix:**
1. **`path_tracer.h`** — Added 4 `std::vector<OutputTexture>` members: `m_albedo_outputs`, `m_specular_albedo_outputs`, `m_normals_aov_outputs`, `m_roughness_aov_outputs`; public accessors `albedo_resource(i)`, `specular_albedo_resource(i)`, `normals_aov_resource(i)`, `roughness_aov_resource(i)`.
2. **`path_tracer.cpp`** — `create_output_textures()` resizes all 4 vectors; `resize_output()` allocates each as `DXGI_FORMAT_R16G16B16A16_FLOAT` UAV at render resolution in `D3D12_RESOURCE_STATE_UNORDERED_ACCESS`; `release_output_textures()` releases all 4. Textures start black — no HLSL writes them until M7/M8.
3. **`denoiser.h`** — Extended `tag_resources()` signature with `albedo` and `specular_albedo` parameters (normals/roughness were already present).
4. **`denoiser.cpp`** — `tag_resources()` now tags `kBufferTypeAlbedo` and `kBufferTypeSpecularAlbedo` from dedicated resources; `kBufferTypeNormals` / `kBufferTypeRoughness` tagged when non-null. No resource is aliased across multiple buffer types.
5. **`renderer.cpp`** — Pre-evaluate UAV→COMMON barrier array expanded from 3 to 7 resources (adds albedo, specular, normals, roughness). `tag_resources()` call updated to pass all 4 AOV accessors. Post-evaluate PSR→UAV barrier array also expanded to 7.

**Note:** AOV textures remain black until M7/M8 HLSL shaders write real G-buffer data. DLSS-RR accepts the tags and stops logging errors; reconstruction quality is limited until real data is provided.

---

### Bug 5 — FIXED ✅ — `RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH #527` on `DenoisedOutputUAV` caused by premature `m_denoised_in_uav_state` flag set

**Observed:** After the Bug 2 fix (AOV textures allocated and all 7 buffers tagged), every frame produced:
```
[streamline][error] resourceTaggingForFrame.cpp:309 Tag of buffer kBufferTypeNormals not set for frame 0, viewport 0
D3D12 ERROR: Before state (0x8: D3D12_RESOURCE_STATE_UNORDERED_ACCESS) of resource
  'MARS::PathTracer::DenoisedOutputUAV[0]' does not match state (0x0: COMMON)
  [ RESOURCE_MANIPULATION ERROR #527: RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH ]
```
Streamline was also writing crash minidumps every frame, killing performance.

**Root cause — two compounding issues:**

1. **`kBufferTypeNormals` is a mandatory DLSS-RR input**, not optional. When it was erroneously omitted (an attempted workaround during earlier investigation), Streamline rejected `slEvaluateFeature` at tag-validation time — before issuing any GPU barriers, including the `COMMON→UAV` barrier on `DenoisedOutputUAV`.

2. **`m_denoised_in_uav_state[oi]` was set `true` unconditionally before `slEvaluateFeature` returned.** When SL aborted at validation and never touched `DenoisedOutputUAV`, the flag was already `true`. On the next frame the `else` branch of the flag check fired a cleanup barrier with `StateBefore=UAV` against a resource still in `COMMON` → `#527` → debug break → Streamline crash-dump → repeat every frame.

**Fix (`renderer.cpp` only):**
1. Restored `normals_aov_resource(oi)` and `roughness_aov_resource(oi)` to the pre-evaluate UAV→COMMON barrier array (back to 7 resources), the `tag_resources()` call, and the post-evaluate PSR→UAV barrier array. All mandatory DLSS-RR tags are now always provided.
2. Moved the `m_denoised_in_uav_state[oi] = true` assignment inside `if (rr_evaluated)`, so the flag is only set when `slEvaluateFeature` actually ran and issued GPU barriers. If SL rejects the call at tag-validation time, the flag stays `false` and no spurious cleanup barrier is emitted on the following frame.

**Rule established:** `m_denoised_in_uav_state` must only be set `true` after a confirmed successful `slEvaluateFeature` return — never speculatively before the call.

---

### Stable Runtime Validation — ✅ Confirmed

**Run date:** 2025-07  
**Duration:** ~44 seconds, normal frame rate, no exceptions, clean shutdown.

Streamline v2.11.1 initialised fully: `sl.common`, `sl.dlss`, `sl.dlss_d`, `sl.dlss_g`, `sl.reflex`, `sl.pcl` all loaded.
DLSSD context created at (853×480) → (1280×720).  
DLSS-G initialised (single generated frame, dynamic MFG not supported on this GPU).  
Shutdown completed without crash dumps.

#### Remaining warnings — all benign

| Warning | Source | Analysis | Action |
|---|---|---|---|
| `D3D or VK API hook is activated without device being created` (×9 at startup) | Streamline `pluginManager.cpp:1331` | Emitted during `initializePlugins` while plugins register API hooks before `slSetD3DDevice` is called. This is the correct `pre_device_init()` → `device_init()` ordering required by Streamline; the messages are internal to SL's startup sequence and do not indicate an engine bug. | None — benign; no code change needed. |
| `Kernel mvec.cs:main … already created!` (×1) | Streamline `d3d12.cpp:1201` | Both `sl.dlss` and `sl.dlss_d` share the same motion-vector compute shader. The second plugin to register it triggers this one-time internal dedup warning. | None — benign; internal to Streamline. |
| `Repeated slDLSSGSetOptions() call for the frame 1` (×1) | Streamline `defines.h:364` | Streamline logs this when `slDLSSGSetOptions` is called twice before the same Present. Likely caused by one call during `evaluate_dlss_g` init and a second during the first frame's `evaluate_dlss_g`. The warning indicates redundancy, not a functional error. | Low priority — could deduplicate the options call in `denoiser.cpp` in a future cleanup pass (M17/M18). |
| `Live Object : 120` at process termination | D3D12 debug layer | The `D3D12Device` proxy is released by Streamline with `ref count 26`, meaning Streamline's internal DLSS-RR/DLSS-G resources hold device references that outlive the proxy release. All listed live objects have `Refcount: 0` except Streamline-owned internal textures/pipelines (`Refcount: 1`). This is expected Streamline behaviour at process exit. The D3D12 debug layer note says "Process is terminating — using simple reporting", confirming this is a shutdown-time observation, not a runtime leak. | Low priority — add `ReportLiveObjects()` call in `DeviceContext::shutdown()` (M15) for a more informative named-object dump during development; no engine resource leak to fix now. |

---

## Milestone Status

| Milestone | Title | Status | Notes |
|---|---|---|---|
| M0 | Repo scaffold, CMake, third-party deps | ✅ | See M0 details below |
| M1 | D3D12 device init, swap chain, clear-color present | ✅ | See M1 details below |
| M2 | Multi-monitor display system | ✅ | See M2 details below |
| M3 | Asset pipeline: FBX/glTF load + GPU upload | ✅ | See M3 details below |
| M4 | DXR pipeline: primary rays, basic PBR hit shader | ✅ | See M4 details below |
| M5 | Scene file parser, static scene rendering | ✅ | See M5 details below |
| M6 | DLSS 4 integration (SR, RR, MFG) | ✅ | 5x crash bugs fixed; DLSS-RR and DLSS-G initialise and run; stable 44-second run validated; AOV textures black until M7/M8 shaders write real G-buffer data; remaining log messages are benign Streamline-internal warnings |

---

## M0 — Completed Work

### Automated (done)
| Item | Detail |
|---|---|
| Engine target | Converted `mars_engine` from `SHARED` to `STATIC`; DLL macros neutralised |
| `engine_api.h` | `MARS_ENGINE_API` kept as no-op placeholder for future use |
| Root `CMakeLists.txt` | `MARS_ENABLE_DEV_UI` option; `cmake/` module path; all FetchContent declarations |
| `cmake/CompileHLSL.cmake` | `target_compile_hlsl()` function; auto-locates DXC; per-file `// #profile` hint |
| Shader stubs | All HLSL/HLSLI files from the plan created under `engine/shaders/` |
| `engine/shaders/CMakeLists.txt` | `shaders_dxil` custom target; VS IDE source groups |
| FetchContent deps | Assimp 5.4.3, nlohmann/json 3.11.3, D3D12MA 2.0.1, HarfBuzz 9.0.0, FreeType 2.13.3, msdfgen 1.12 |
| Dev-only ImGui | ImGui 1.91.8 fetched and compiled only when `MARS_ENABLE_DEV_UI=ON` |
| `CMakePresets.json` | `MARS_ENABLE_DEV_UI=ON` for debug, `OFF` for release; build dirs moved to `C:/mars_build/` to avoid OneDrive locking FetchContent git pack files; `FETCHCONTENT_BASE_DIR=C:/mars_build/fetchcontent` |
| Build verification | `mars_engine.lib` and `mars_test_app.exe` compile clean (Debug) |

### Manual steps — all complete ✅
| Item | Detail |
|---|---|
| **Windows Agility SDK** | ✅ `Microsoft.Direct3D.D3D12` 1.614.1 extracted to `C:/mars_deps/nuget/`; wired via `cmake/AgilitySDK.cmake` |
| **DirectXTK12** | ✅ `directxtk12_desktop_win10` 2026.4.1.1 extracted; imported as `MARS::DirectXTK12` |
| **DirectXTex** | ✅ `directxtex_desktop_win10` 2026.5.8.1 extracted; imported as `MARS::DirectXTex` |
| **DirectXMesh** | ✅ `directxmesh_desktop_win10` 2026.5.8.1 extracted; imported as `MARS::DirectXMesh` |
| **NVIDIA Streamline SDK (DLSS 4)** | ✅ Streamline SDK v2.11.1 copied to `C:/mars_deps/`; `cmake/StreamlineSDK.cmake` provides `MARS::Streamline` target and `mars_deploy_streamline()` post-build DLL deploy |
| **NVIDIA Nsight Aftermath** | ✅ Aftermath SDK 2025.5.0 copied to `C:/mars_deps/`; `cmake/NsightAftermath.cmake` provides `MARS::NsightAftermath` target and `mars_deploy_aftermath()` post-build DLL deploy |
| **DXC on PATH** | ✅ `dxc.exe` found at `C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64`; `shaders_dxil` target armed |

### Deferred to later milestones
| Item | Detail | When |
|---|---|---|
| **FreeType + HarfBuzz font integration** | Currently `HB_HAVE_FREETYPE=OFF`; enable once FreeType is wired via `CMAKE_PREFIX_PATH` | M16 |
| **msdfgen with font input** | Currently `MSDFGEN_CORE_ONLY=ON`; enable `msdfgen-ext` once FreeType is wired | M16 |
| **WinPixEventRuntime** | NuGet package not yet wired into CMake | M15 |

---

## Detailed Log

### M0 — Repo Scaffold, CMake, Third-Party Deps
- ✅ Add Windows Agility SDK (D3D12) to CMakeLists
- ✅ Add DirectXTK12, DirectXTex, DirectXMesh
- ✅ Add Assimp via FetchContent
- ✅ Add nlohmann/json via FetchContent
- ✅ Add ImGui via FetchContent *(developer debug overlay only — not the game UI)*
- ✅ Add HarfBuzz via FetchContent (Unicode text shaping for game UI)
- ✅ Add FreeType via FetchContent (font metrics for msdfgen and HarfBuzz at build time)
- ✅ Add msdfgen as CMake build-tool target (offline MSDF atlas generation)
- ✅ Add D3D12 Memory Allocator (D3D12MA) via FetchContent
- 🔲 Add WinPixEventRuntime via NuGet *(deferred to M15)*
- ✅ Add NVIDIA Aftermath SDK — `cmake/NsightAftermath.cmake`; DLLs auto-deployed beside exe
- ✅ Add NVIDIA DLSS 4 SDK (via Streamline SDK v2.11.1) — `cmake/StreamlineSDK.cmake`; all `sl.*` and `nvngx_*` DLLs auto-deployed beside exe
- ✅ CMake custom target: compile HLSL shaders via DXC
- ✅ Create `engine/shaders/` directory structure
- ✅ Verify full solution builds cleanly

### M1 — D3D12 Device Init, Swap Chain, Clear-Color Present
- ✅ `DeviceContext` class: adapter enumeration, device creation, feature level check
- ✅ Verify DXR Tier 1.1 support; hard-exit if not met
- ✅ Three command queues (Direct, Compute, Copy)
- ✅ Bindless CBV/SRV/UAV descriptor heap (1M slots)
- ✅ `DisplayOutput` class: swap chain creation, resize handling
- ✅ HDR color space detection (`CheckColorSpaceSupport`): activate scRGB → HDR10/PQ → SDR fallback
- ✅ First frame: clear back buffer to deep navy and present
- ✅ Test app: Win32 window + message loop driving M1 code

### M2 — Multi-Monitor Display System
- ✅ `DisplayManager` class: enumerates `IDXGIOutput` objects on the selected adapter
- ✅ `load_config()`: parses `display.json` via nlohmann/json; falls back to single-monitor default
- ✅ `DisplayConfig` / `MonitorRole` structs for per-display yaw/pitch/fov offsets
- ✅ Creates one `DisplayOutput` per `DisplayConfig` entry (native-resolution auto-detection from DXGI output desc)
- ✅ `Renderer` updated: owns `DisplayManager`; single-monitor `init(hwnd, w, h)` overload preserved
- ✅ `render_frame()` loops over all outputs, clears each to a distinct color, presents all
- ✅ `on_resize(output_index, w, h)` per-output resize path
- ✅ Test app: creates one Win32 window per `DisplayConfig`; reads `display.json` from working directory
- ✅ Sample `display.json` added at repo root
- ✅ Build clean

### M3 — Asset Pipeline
- ✅ `AssetImporter` (`MeshLoader`): Assimp → interleaved vertex / index buffer on GPU
- ✅ `TextureLoader`: DirectXTex → `ID3D12Resource` (BC7/BC6H DDS preferred)
- ✅ `ResourceManager`: handle pool, D3D12MA allocator, copy queue flush
- ✅ `GpuMeshBuffer`: D3D12 VB/IB + bindless SRV registration
- ✅ Bindless registration of textures

### M4 — DXR Pipeline
- ✅ DXR pipeline state object (RTPSO) built from `path_trace.dxil` (`lib_6_3`)
- ✅ Global root signature: bindless CBV/SRV/UAV heap + frame constants CB
- ✅ Shader tables: ray-gen, miss, and hit-group records written
- ✅ BLAS build from scene `GpuMeshBuffer` instances
- ✅ TLAS build and update
- ✅ Ray-gen shader: one ray per pixel, result written to RGBA16F UAV
- ✅ Closest-hit shader: interpolated vertices via raw byte-address loads; GGX/Smith/Fresnel PBR BRDF; bindless texture fetch (base color, normal, metallic/roughness, emissive); shadow ray for directional sun
- ✅ Miss shader: sky gradient return
- ✅ `copy_to_back_buffer()`: blit UAV → swap-chain back buffer
- ✅ `Renderer` drives `PathTracer` for all `DisplayOutput` instances; clear-color fallback if not initialised

### M5 — Scene File Parser & Static Scene
- ✅ `SceneLoader`: parse `.marsscene` JSON
- ✅ Instantiate `MeshInstance`, `Light`, `Camera` objects
- ✅ Build TLAS from loaded instances
- ✅ Render a multi-object static scene

### M6 — DLSS 4 Integration (SR, RR, MFG)
- ✅ Streamline SDK v2.11.1 integrated; all `sl.*` / `nvngx_*` DLLs deployed
- ✅ DLSS 4 Super Resolution (SR) initialised and running
- ✅ DLSS 4 Ray Reconstruction (RR) initialised and running
- ✅ DLSS 4 Multi Frame Generation (MFG / DLSS-G) initialised and running
- ✅ AOV textures allocated in `PathTracer` (albedo, specular albedo, normals, roughness) — values black until M7/M8 shaders write real G-buffer data
- ✅ All mandatory Streamline buffer tags provided each frame (`kBufferTypeAlbedo`, `kBufferTypeSpecularAlbedo`, `kBufferTypeNormals`, `kBufferTypeRoughness`, `kBufferTypeScalingInputColor`, `kBufferTypeMotionVectors`, `kBufferTypeDepth`)
- ✅ Five crash bugs fixed; stable 44-second run validated — see *Current Focus* above for full investigation

---

## M3 — Completed Work

### New Files
| File | Purpose |
|---|---|
| `engine/include/mars_engine/math/math_types.h` | `Vec2`, `Vec3`, `Vec4`, `Mat4x4`, `Quaternion`, `Transform`, `AABB` — inline math types |
| `engine/include/mars_engine/asset/asset_types.h` | CPU-side: `Vertex`, `MeshData`, `MaterialData`, `ModelAsset`, `TextureRef` |
| `engine/include/mars_engine/asset/asset_importer.h` | `AssetImporter` — Assimp-based FBX/glTF/OBJ loader |
| `engine/src/asset/asset_importer.cpp` | Implementation: scene-graph traversal, mesh/material extraction |
| `engine/include/mars_engine/asset/gpu_mesh_buffer.h` | `GpuMeshBuffer` — D3D12 VB/IB + bindless SRV |
| `engine/src/asset/gpu_mesh_buffer.cpp` | Upload via Copy queue, D3D12MA DEFAULT-heap allocation |
| `engine/include/mars_engine/asset/texture_loader.h` | `GpuTexture`, `TextureLoader` — DirectXTex loader |
| `engine/src/asset/texture_loader.cpp` | DDS/PNG/EXR/HDR load, mip generation, bindless SRV registration |
| `engine/include/mars_engine/asset/resource_manager.h` | `ResourceManager`, `GpuModel` — owns allocator + asset caches |
| `engine/src/asset/resource_manager.cpp` | D3D12MA allocator init, `load_model()` / `load_texture()` |

### Updated Files
| File | Change |
|---|---|
| `engine/include/mars_engine/scene/scene.h` | `Scene` now holds `SceneModelInstance` list; `add_model()` wired to `ResourceManager` |
| `engine/src/scene/scene.cpp` | Scene load/unload/add_model implemented |
| `engine/include/mars_engine/mars_engine.h` | Added all new asset headers to public single-include |
| `engine/CMakeLists.txt` | Added asset source files, D3D12MA private include path |

---

## M4 — Completed Work

### New Files
| File | Purpose |
|---|---|
| `engine/include/mars_engine/renderer/path_tracer.h` | `PathTracer` — DXR pipeline, RTPSO, shader tables, BLAS/TLAS, per-output UAV textures, frame-constants ring buffer |
| `engine/src/renderer/path_tracer.cpp` | Full implementation: allocator, DXIL load, RTPSO build, shader table write, BLAS/TLAS construction, `DispatchRays`, UAV blit |
| `engine/include/mars_engine/renderer/frame_constants.h` | `FrameConstants` HLSL-compatible CB struct (camera matrices, frame index, sun direction/colour) |
| `engine/shaders/path_trace.hlsl` | Ray-gen, closest-hit (GGX PBR + shadow ray), and miss shaders |
| `engine/shaders/shadow_ray.hlsl` | Shadow visibility ray (any-hit returns 0; miss returns 1) |
| `engine/shaders/material/pbr_brdf.hlsli` | GGX / Smith / Fresnel BRDF functions |
| `engine/shaders/material/material_data.hlsli` | Bindless material parameter fetch |
| `engine/shaders/sky/physical_sky.hlsli` | Sky gradient stub (full Hillaire model deferred to M5+) |
| `engine/shaders/sky/hdri_sky.hlsli` | HDRI sky stub |
| `engine/shaders/common/bindless.hlsli` | Bindless heap accessor macros |
| `engine/shaders/common/math.hlsli` | Common HLSL math helpers |
| `engine/shaders/common/random.hlsli` | PCG hash / Halton sequence |
| `engine/shaders/gi/restir_di.hlsl` | ReSTIR DI stub (deferred to M7) |
| `engine/shaders/gi/restir_gi.hlsl` | ReSTIR GI stub (deferred to M8) |

### Updated Files
| File | Change |
|---|---|
| `engine/src/renderer/renderer.cpp` | Owns and drives `PathTracer`; `render_frame()` calls `begin_frame()` / `trace()` / `copy_to_back_buffer()` for every `DisplayOutput` |
| `engine/CMakeLists.txt` | Added `path_tracer.cpp` to sources; wired `shaders_dxil` dependency |

---

## M5 — Completed Work

### New Files
| File | Purpose |
|---|---|
| `engine/include/mars_engine/scene/scene_types.h` | `LightDesc`, `CameraDesc`, `SkyboxDesc` — JSON-mapped scene metadata types |
| `engine/include/mars_engine/scene/scene_loader.h` | `SceneLoader::load()` — public API for parsing `.marsscene` files |
| `engine/src/scene/scene_loader.cpp` | JSON parsing (nlohmann/json): resolves relative paths, loads models, populates lights/cameras/skybox |
| `engine/include/mars_engine/camera/camera.h` | `Camera` (view/proj matrices, display offsets, prev-frame storage) and `FlyCamera` (WASD + mouse-look) |
| `engine/src/camera/camera.cpp` | Camera matrix construction (right-handed, D3D12 depth [0,1]), rigid-body / projection inverse |
| `test_scene.marsscene` | Sample scene file: one directional sun light, one default camera, no mesh instances |

### Updated Files
| File | Change |
|---|---|
| `engine/include/mars_engine/scene/scene.h` | Added `SceneLoader` friend, M5 metadata members (`m_lights`, `m_cameras`, `m_skybox`), accessors, `load_from_file()` |
| `engine/src/scene/scene.cpp` | `load_from_file()` delegates to `SceneLoader`; `unload()` clears all M5 state |
| `engine/include/mars_engine/renderer/renderer.h` | Added `load_scene()`, `rebuild_tlas()`, `scene()`, `resource_manager()` accessors; added `ResourceManager` and `Scene` members |
| `engine/src/renderer/renderer.cpp` | `init_internal()` calls `ResourceManager::init()`; `shutdown()` calls `ResourceManager::shutdown()` and `Scene::unload()`; `load_scene()` builds BLAS/TLAS from loaded instances |
| `engine/include/mars_engine/mars_engine.h` | Added `scene_types.h` and `scene_loader.h` to public single-include |
| `engine/CMakeLists.txt` | Added `scene_loader.cpp`, `camera.cpp`, and M5 headers to source/header lists |
| `test_app/src/main.cpp` | FlyCamera integration: WASD + QE/Space + left-click mouse capture; loads `test_scene.marsscene`; seeds camera from scene `cameras[0]` if present |

---

## Future Milestone Checklists

### M7 — ReSTIR DI (Direct Lighting + Ray-Traced Shadows)
- 🔲 ReSTIR DI reservoir data structures (structured buffer, per-pixel reservoir)
- 🔲 Initial candidate sampling (light importance sampling)
- 🔲 Temporal reuse pass
- 🔲 Spatial reuse pass
- 🔲 Visibility ray for selected reservoir sample (shadow ray)
- 🔲 Write real sun/directional light data to `kBufferTypeNormals` / AOV textures in HLSL

### M8 — ReSTIR GI (Multi-Bounce Global Illumination)
- 🔲 Secondary ray dispatch (multi-bounce path tracing)
- 🔲 ReSTIR GI reservoir structures and temporal reuse
- 🔲 Spatial reuse pass for GI reservoirs
- 🔲 Write real G-buffer AOV data (albedo, specular albedo, roughness) in closest-hit HLSL

### M9 — Animation System + Skeletal Mesh Rendering
- 🔲 CPU clip evaluation → bone palette
- 🔲 GPU skinning compute shader (bone palette → skinned vertex buffer)
- 🔲 BLAS refit for skinned meshes each frame
- 🔲 1D / 2D blend trees; cross-fade between clips
- 🔲 FABRIK IK solver for foot placement
- 🔲 Rigid node animation (wheels, doors, flags)
- 🔲 Compute cloth simulation for waving flags (spring lattice, wind vector)

### M10 — Procedural Ecosystem / Vegetation + Wind
- 🔲 GPU-driven instance placement compute shader (reads density map)
- 🔲 GPU-driven LOD selection compute pass (projected solid angle → BLAS index)
- 🔲 Impostor billboard pre-bake and ray-traversal sampling
- 🔲 Wind compute shader: Bezier trunk/branch bend → vertex position write-back
- 🔲 Per-frame TLAS update for dynamic vegetation instances
- 🔲 GPU frustum culling prepass (no Hi-Z / no rasterized depth)

### M11 — Particle System
- 🔲 GPU compute particle simulation (Append/Consume buffers, up to 1M particles)
- 🔲 Emitter types: point, box, sphere, mesh-surface, trail
- 🔲 Ray-traced volumetric particles (smoke, fire) as procedural TLAS geometry
- 🔲 Oriented procedural primitives for sparks / rain streaks in TLAS
- 🔲 NEE / shadow rays from particle hit shaders

### M12 — Weather System
- 🔲 GPU rain particle streaks
- 🔲 Animated rain-drop ripple normal maps sampled at ray hit time
- 🔲 Puddle procedural normal maps + near-zero-roughness PBR wet geometry
- 🔲 Volumetric clouds (ray-marched density field, Schneider 2015+)
- 🔲 Heterogeneous exponential height fog + volumetric light shafts (in-scattering)
- 🔲 PBR dry↔wet material blend driven by weather intensity float
- 🔲 Snow particles + compute-written surface coverage map
- 🔲 Global wind vector integration (drives vegetation, flags, particle drift)

### M13 — Decal System (Tire Tracks / Skid Marks)
- 🔲 World-space decal buffer (position, orientation, extents, material)
- 🔲 CPU-built spatial hash / BVH uploaded to GPU structured buffer
- 🔲 Closest-hit shader decal query and material blend
- 🔲 Ring buffer of N slots; oldest-evict on overflow
- 🔲 Tire track emission from vehicle physics (slip angle threshold)
- 🔲 Skid mark: roughness reduction + albedo darkening

### M14 — Crowd Rendering
- 🔲 CPU boid simulation (nearby agents); scripted paths (distant)
- 🔲 Agent positions uploaded via Copy queue to GPU structured buffer
- 🔲 GPU compute: per-instance TLAS transform write + crowd TLAS registration
- 🔲 Per-instance bone palette in structured buffer; GPU skinning compute
- 🔲 Animation Texture Baking (ATB) with blended clip playback
- 🔲 GPU-driven LOD selection (projected solid angle → BLAS/LOD index)
- 🔲 Distant crowd impostors as pre-baked multi-angle radiance cards in TLAS

### M15 — Debug Overlay *(Developer-Only: ImGui, PIX, Aftermath)*
- 🔲 ImGui integration (debug builds only, stripped from shipping)
- 🔲 Per-pass GPU timer panels
- 🔲 WinPixEventRuntime GPU event markers
- 🔲 NVIDIA Aftermath crash dump integration (`ReportLiveObjects()` in `DeviceContext::shutdown()`)
- 🔲 `--renderdoc` flag + `renderdoc.dll` injection support
- 🔲 Add WinPixEventRuntime via NuGet; wire into CMake

### M16 — Game UI System
- 🔲 `UISystem` class: widget tree, per-frame dirty tracking
- 🔲 `UIRenderer`: upload geometry, rasterization pass, composite onto path-traced frame
- 🔲 Enable FreeType via `CMAKE_PREFIX_PATH`; enable `HB_HAVE_FREETYPE=ON`
- 🔲 Enable `msdfgen-ext` (disable `MSDFGEN_CORE_ONLY`)
- 🔲 MSDF font atlas pipeline: msdfgen offline tool → BC7 atlas + JSON metrics
- 🔲 HarfBuzz integration: Unicode shaping, OpenType features, RTL/BiDi
- 🔲 MSDF text shader (HLSL): correct alpha reconstruction, sub-pixel fringe correction (SDR only)
- 🔲 Core widgets: `Label`, `Image`, `Button`, `ProgressBar`, `Gauge`
- 🔲 Advanced widgets: `Minimap` (secondary low-res ray-gen dispatch into UAV), `Leaderboard`, `Notification`, `Screen`
- 🔲 `.marsui` JSON screen file parser and hot-reload
- 🔲 Theme/style sheet system (JSON): colors, spacing, animation curves
- 🔲 HDR-aware UI color pipeline (linear → tone map per display)
- 🔲 Per-monitor DPI scaling support

### M17 — Test App Polish
- 🔲 Hot-reload: `R` key reloads current scene without restart
- 🔲 Screenshot: `F12` saves HDR EXR to working directory
- 🔲 Gamepad support (XInput): analog steering, trigger accelerate/brake
- 🔲 Orbit camera (`F` toggle between free-fly and locked orbit)
- 🔲 Dev panel toggle with `~` key (ImGui, dev builds only)

### M18 — Performance Tuning, HDR Calibration, Shipping Polish
- 🔲 Per-display peak-brightness calibration (nits target)
- 🔲 ACES tone-map tuning for HDR10 / scRGB / SDR paths
- 🔲 `Repeated slDLSSGSetOptions()` call deduplication in `denoiser.cpp`
- 🔲 `ReportLiveObjects()` cleanup audit at process exit
- 🔲 FSR 4 / FSR 3 AMD fallback integration (GPUOpen FidelityFX SDK)
- 🔲 Final GPU timer profiling pass; pipeline bottleneck resolution

---

## Known Issues / Blockers
- WinPixEventRuntime has not been wired into CMake yet; deferred to M15.
- FreeType + HarfBuzz font integration and msdfgen-ext are deferred to M16 (not needed until UI work begins).
- FSR 3 (AMD FidelityFX Super Resolution) is the open-source fallback for non-DLSS hardware;
  can be integrated via the GPUOpen FidelityFX SDK on GitHub when needed.

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
| 2025-07 | NVIDIA SDKs delivered via Streamline SDK v2.11.1 (DLSS 4 SR, Frame Gen, Ray Reconstruction, Reflex, NIS) and Nsight Aftermath 2025.5.0; both stored under `C:/mars_deps/` and integrated via custom CMake modules |
| 2025-07 | DXC sourced from Windows SDK 10.0.26100.0; hardcoded as fallback hint in `cmake/CompileHLSL.cmake` — no separate install required |
| 2025-07 | `FETCHCONTENT_UPDATES_DISCONNECTED ON` set globally in root `CMakeLists.txt` to prevent FetchContent re-clone failures caused by OneDrive file locks on `C:/mars_build/fetchcontent` |
| 2025-07 | Wide string literals in Win32 window titles must use ASCII-only characters; em dashes and other multi-byte UTF-8 characters cause garbled titles without explicit `/utf-8` MSVC flag |
| 2025-07 | `engine/CMakeLists.txt` uses `GLOB_RECURSE` for source discovery but also maintains an explicit `list(APPEND)` for new files so they are included immediately without waiting for a CMake re-run |
