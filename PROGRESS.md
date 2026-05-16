# PROGRESS.md — MARS 3D Engine Implementation Progress

---

## Current Focus
M10 (Ecosystem / Vegetation) is substantially implemented. Remaining M10 work: GPU frustum-cull prepass, offline OMM bake, offline impostor bake, per-frame TLAS update for dynamic instances.

Sky rendering (part of M5/M8 scene infrastructure) has been extended with three selectable modes via `"type"` in the `"skybox"` scene block:

- **`"debug"`** — the original procedural colour-coded cube-face sky with grids and crosshair.
- **`"physical"`** — a new analytic non-debug procedural atmosphere (Rayleigh/Mie approximation in `path_trace.hlsl`).
- **`"hdri"`** — equirectangular HDRI environment map loaded from an `.exr` or `.hdr` file specified by the `"hdri"` key.

HDRI loading uses **tinyexr v1.0.9** (+ split miniz from the same deps tree) for `.exr` files, wired into `texture_loader.cpp`. The implementation (tinyexr + miniz) is compiled in separate third-party translation units (`exr_loader.cpp`, `miniz.c`) with `/W0 /WX-` to keep upstream warnings out of the strict engine build. `test_scene.marsscene` is currently configured with `"type": "hdri"` pointing at `models/hdri/DaySkyHDRI007B_8K_HDR.exr`.

---

## Legend

| Symbol | Meaning |
|---|---|
| ✅ | Complete |
| 🔄 | In progress |
| 🔲 | Not started |
| ❌ | Blocked / deferred |

---

## Milestone Status
**Agents:** This section is for tracking the overall status of each major milestone in the engine development roadmap. Update this section with the current status of each milestone, along with any relevant notes or details about the implementation progress.

| Milestone | Title | Status | Notes |
|---|---|---|---|
| M0 | Repo scaffold, CMake, third-party deps | ✅ | HarfBuzz compiled with `HB_HAVE_FREETYPE=OFF`; msdfgen compiled with `MSDFGEN_CORE_ONLY=ON`; both deferred to M16. WinPixEventRuntime deferred to M15. |
| M1 | D3D12 device init, swap chain, clear-color present | ✅ | `DeviceContext` uses `ID3D12Device5`; bindless heap is 1 M slots (`k_bindless_heap_size`); 2-frame ring (`k_frame_count = 2`). `DisplayOutput` exposes `HdrMode` enum (SDR / HDR10 / scRGB); HDR10 and scRGB paths fully implemented. |
| M2 | Multi-monitor display system | ✅ | `DisplayManager::load_config()` parses `display.json`; falls back to single monitor 0 / 90° FOV / center role when absent. `MonitorRole` enum: Center / Left / Right / Overhead / Custom. |
| M3 | Asset pipeline: FBX/glTF load + GPU upload | ✅ | Vertex layout: position\|normal\|tangent\|uv\|bone\_indices\|bone\_weights; stride = 76 bytes (hardcoded in HLSL fetch). `GpuMeshBuffer` registers both a structured-buffer SRV (vertex) and a `ByteAddressBuffer` SRV (index) in the bindless heap. |
| M4 | DXR pipeline: primary rays, basic PBR hit shader | ✅ | Miss shader is a procedural colour-coded cube-face skybox (not a real sky); physical sky is planned for M8+. Motion vectors and linear depth UAVs are written by the ray-gen shader each frame. `copy_to_back_buffer()` accepts a `use_denoised` flag to blit `denoised_output_resource` instead of the raw path-tracer UAV. |
| M5 | Scene file parser, static scene rendering | ✅ | `SceneLoader` populates `Scene` with `SceneModelInstance`, `LightDesc`, `CameraDesc`, and `SkyboxDesc`. `FlyCamera` (WASD + mouse-look) lives in `camera.h` alongside `Camera`; it is the camera used by `test_app`. `Camera::advance_frame()` saves previous-frame matrices and applies Halton jitter for DLSS temporal accumulation. `SkyboxDesc::Type` now has three values: `Debug`, `Physical`, `HDRI`; selected via `"type"` in the `"skybox"` scene block. HDRI loads `.exr` files via tinyexr; `.hdr` files via DirectXTex `LoadFromHDRFile`. |
| M6 | DLSS 4 integration (SR, RR, MFG) | ✅ | DLSS-RR temporal stability fully resolved post-M8: depth buffer corrected to NDC projected depth; motion vectors corrected to NDC space with proper sign, Y-flip, and scale; `cameraMotionIncluded = eTrue`; `clipToPrevClip` set to identity when MVs carry full camera motion. See Notes in AGENTS.md. |
| M7 | ReSTIR DI + real G-buffer AOV writes | ✅ | FrameConstants extended with 4 AOV UAV slots; path_tracer.cpp root signature expanded (spaces 5–8); AOV textures allocated and filled each frame; ClosestHit writes real diffuse albedo / specular F0 / world normals / roughness to DLSS-RR G-buffer; ReSTIR DI direct lighting via RIS + shadow ray replaces the old flat NdotL; restir_di.hlsli contains shared reservoir math; DXR-dependent helpers (RIS_GenerateCandidates, ReservoirShade + shadow TraceRay) live in path_trace.hlsl; verified: dxc -T lib_6_3 compiles with -WX clean |
| M8 | ReSTIR GI — multi-bounce global illumination | ✅ | GI implemented as BRDF-importance-sampled MC estimator with configurable bounce depth (`gi_bounce_count`; 0=off, 1=single, 2=two-bounce). Russian Roulette path termination at depth >= 1. RTPSO max recursion = 3. Reservoir-based temporal/spatial reuse removed after debugging — see Notes. DLSS-RR denoises the noisy indirect signal. |
| M9 | Animation system + skeletal mesh rendering | ✅ | Cloth sim: canonical XPBD solver (INTEGRATE → CONSTRAIN×N → FINALIZE), 3-position-state buffers, implicit velocity, compliance-based constraints, `final_srv` inversion bug fixed. Global wind system (`WindDesc`) was implemented then **removed** — replaced by per-instance procedural cloth (`wind_direction`, `wave_amplitude`) and time-based tree bend. |
| M10 | Ecosystem / vegetation + wind | 🔄 In progress | Core placement, LOD selection, stochastic dither, impostor BLAS, AnyHit alpha-test, and procedural vegetation/cloth motion all implemented. Remaining: GPU frustum-cull prepass, offline OMM bake, offline impostor bake, per-frame TLAS update for dynamic instances. SpeedTree ORCA v2 compatible (European Linden confirmed working). |
| M11 | Particle system | 🔲 Not started |
| M12 | Weather system (rain, clouds, fog) | 🔲 Not started |
| M13 | Decal system (tire tracks, skid marks) | 🔲 Not started |
| M14 | Crowd rendering | 🔲 Not started |
| M15 | Debug overlay (dev-only: ImGui, PIX, Aftermath) | 🔲 Not started |
| M16 | Game UI system: MSDF fonts, widget tree, HUD | 🔲 Not started |
| M17 | Displacement mapping: offline bake pipeline + NVIDIA DMM path | 🔲 Not started |
| M18 | Test app polish: hot reload, screenshots, gamepad | 🔲 Not started |
| M19 | Performance tuning, peak-brightness calibration, HDR display polish | 🔲 Not started |

---

## Progess Checklist
**Agents:** This section is for tracking the implementation progress of each major feature or component of the engine, as outlined in the original plan. Update this section with checkboxes for each item as they are completed, and add any relevant notes or details about the implementation status.

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
- ✅ AOV textures allocated in `PathTracer` (albedo, specular albedo, normals, roughness)
- ✅ All mandatory Streamline buffer tags provided each frame (`kBufferTypeAlbedo`, `kBufferTypeSpecularAlbedo`, `kBufferTypeNormals`, `kBufferTypeRoughness`, `kBufferTypeScalingInputColor`, `kBufferTypeMotionVectors`, `kBufferTypeDepth`)
- ✅ Five crash bugs fixed; stable 44-second run validated — see *Current Focus* above for full investigation

### M7 — ReSTIR DI (Direct Lighting + Ray-Traced Shadows)
- ✅ ReSTIR DI reservoir data structures (structured buffer, per-pixel reservoir)
- ✅ Initial candidate sampling (light importance sampling)
- ✅ Temporal reuse pass
- ✅ Spatial reuse pass
- ✅ Visibility ray for selected reservoir sample (shadow ray)
- ✅ Write real world-space normals / diffuse albedo / specular F0 / roughness to AOV textures in closest-hit HLSL

### M8 — ReSTIR GI (Multi-Bounce Global Illumination)
- ✅ `GIReservoir` data structures (`restir_gi.hlsli`): x_world, x_normal, Lo, w_sum, W, M; init/update/finalize/merge helpers; cosine-hemisphere sampler
- ✅ Per-output GI reservoir structured buffer (GPU default-heap, `width × height × 2` elements, 48-byte stride, UAV in bindless heap)
- ✅ `FrameConstants` extended: `gi_reservoir_uav_slot`, `gi_enabled`
- ✅ Root signature space9 UAV range for `g_GIReservoirs[]`
- ✅ Secondary ray dispatch in `ClosestHit` (depth 0 → one cosine-hemisphere GI ray; depth 1 → no recursion)
- ✅ GI reservoir written per-pixel into ping-pong structured buffer (layer = `frame_index & 1`)
- ✅ Temporal reuse pass: previous-frame reservoir merged into current via `GIReservoirMerge`; M capped at 30 to bound temporal lag
- ✅ Spatial reuse pass: 4 jittered-offset neighbors from previous-frame layer merged, guarded by normal-similarity threshold (cos > 0.9)
- ✅ Multi-bounce GI radiance (`giContrib`) composited into final `payload.radiance` alongside direct light and emissive
- ✅ Reservoir temporal/spatial reuse removed; replaced with correct single-bounce MC estimator after reuse bugs caused brightness explosion and black indirect lighting
- ✅ Multi-bounce path tracing: `gi_bounce_count` in `FrameConstants` (0=off, 1=single, 2=two-bounce, N=N-bounce)
- ✅ `PrimaryPayload` extended with `throughput` field (accumulated BRDF/pdf weight across bounces)
- ✅ Russian Roulette path termination at depth >= 1 (luminance of throughput as survival probability)
- ✅ RTPSO max recursion depth raised from 2 to 3 (supports primary + 2 GI bounces)
- ✅ Shadow rays remain depth-0 only; AOV G-buffer writes remain depth-0 only
- ✅ Miss shader scales sky radiance by accumulated throughput at depth > 0
- ✅ `PathTracer::set_gi_bounce_count(uint32_t)` public API; default = 1 (backward-compatible)

### M9 — Animation System + Skeletal Mesh Rendering
- ✅ Skeleton and AnimationClip data structures (bone hierarchy, keyframes, interpolation)
- ✅ AnimationSystem class: clip evaluation, blend trees, cross-fade, bone palette computation
- ✅ AssetImporter extensions: import_skeleton() and import_animations() from FBX/glTF via Assimp
- ✅ GPU skinning compute shader (skinning.hlsl: bone palette → skinned vertex buffer)
- ✅ GpuMeshBuffer extensions: skinned vertex buffer UAV, bone palette SRV, enable_skinning(), upload_bone_palette()
- ✅ PathTracer integration: build_skinned_blas(), dispatch_skinning(), refit_blas() — skinning dispatch, bone palette upload, and per-frame BLAS refit
- ✅ Scene integration: animated mesh instances, clip assignment, playback control — SceneLoader parses `"animation"` blocks; Renderer creates AnimationState per instance, builds skinned BLASes, dispatches GPU skinning and BLAS refit each frame
- ✅ Rigid node animation (wheels, doors, flags) — `RigidNodeInstance` in scene types; `SceneLoader` parses `"rigid_nodes"` array; `Renderer::update()` evaluates clip, composes world transform, updates TLAS slot; TLAS rebuilt when any rigid node moves
- ✅ Compute cloth simulation for waving flags — `ClothDesc`/`ClothInstance` in scene types; `SceneLoader` parses `"cloth"` array; `GpuMeshBuffer::create_cloth_mesh()` procedural grid; `ClothGpuResources` ping-pong buffers; `cloth_sim.hlsl` compute (WAVE → INTEGRATE → CONSTRAIN×N → FINALIZE); `PathTracer::create_cloth_pipeline()` / `dispatch_cloth_sim()`; `Renderer` dispatches cloth each frame, refits BLAS, then rebuilds TLAS
- ✅ Cloth solver: XPBD compliance-based constraints; per-type separate accumulators (structural/shear/bend averaged independently); PASS 3 WAVE injects traveling sine wave into `pos_curr` each substep using per-instance `wind_direction` + `wave_amplitude` from `ClothDesc`; renderer cloth delta-time clamped to 1/30s to survive window-drag stalls
- ✅ Global wind system (`WindDesc`, CPU layered oscillator, `m_species_wind_states`, spring envelope) **removed** — replaced by per-instance procedural motion for cloth and trees
- ✅ ~~FABRIK IK solver for foot placement~~ (removed from scope — pit crew animations are pre-baked)

### M10 — Procedural Ecosystem / Vegetation + Wind

#### Asset Pipeline
- 🟡 SpeedTree ORCA v2 FBX import: HighPoly and LowPoly meshes per species, SpeedTree texture channel convention (BaseColor RGBA, Specular ORM, Normal DX) — `AssetImporter::import_vegetation_species()` implemented; `Renderer::setup_ecosystem()` scans HighPoly/LowPoly directories and loads each LOD via `ResourceManager::load_model()`
- 🔲 Offline OMM bake: per-mesh + BaseColor alpha channel → DXR 1.2 Opacity Micromap bitmasks stored alongside BLAS
- 🔲 Offline octahedral impostor bake: 16x16 view grid → packed radiance + depth atlas per species

#### Runtime Placement & Culling
- 🟡 GPU-driven instance placement compute shader (reads density map, emits instance transforms) — `vegetation_placement.hlsl` + `PathTracer::dispatch_vegetation_placement()` implemented; `EcosystemGpuResources` allocates instance/counter/species GPU buffers plus CPU readback buffers (`engine/include/mars_engine/asset/gpu_mesh_buffer.h`); `Renderer::place_and_register_vegetation()` runs placement synchronously at scene-load, reads back the atomic counter + instance records, appends each placed instance as a `CpuInstanceData` + `PathTracer::set_instance()` TLAS entry, and mirrors them into `EcosystemDesc::instances`; remaining: GPU frustum cull prepass + per-frame TLAS update for moving instances
- 🔲 GPU frustum + max-distance cull prepass (marks inactive instances before TLAS construction; no Hi-Z)

#### LOD System
- 🟡 GPU-driven LOD selection compute pass (projected solid angle per instance → LOD level + BLAS index; no CPU readback) — `vegetation_lod_selection.hlsl` + `PathTracer::dispatch_vegetation_lod_selection()` implemented; `Renderer::dispatch_ecosystem()` runs per-frame LOD selection
- ✅ Stochastic LOD dither: per-ray Hash01(instanceId, pixelId, sampleIndex) selects one LOD tier — no simultaneous dual-LOD alpha blend; blue-noise for primary rays, hashed instance/sample noise for secondary, temporal scrambling for denoiser convergence — implemented in `path_trace.hlsl` `AnyHit_Primary`

#### Foliage Alpha (Opacity Micromaps)
- 🔲 DXR 1.2 OMM integration: attach pre-baked OMM bitmasks to BLAS build for LOD 0-2 alpha-tested foliage
- ✅ AnyHit alpha-test fallback path for hardware without OMM support — stochastic AnyHit LOD-dither path in `path_trace.hlsl`

#### Octahedral Impostors (LOD 3)
- 🟡 Procedural AABB TLAS entry per far impostor instance — `PathTracer::build_vegetation_impostor_blas()` implemented and called from `Renderer::setup_ecosystem()` per species; impostor BLAS is selected dynamically once per-frame LOD selection promotes an instance to LOD3 (placement currently registers all instances at Near LOD)
- ✅ Custom intersection shader: octahedral UV lookup, depth-offset parallax correction, reconstructed surface normal and hit position — `Intersection_Impostor` / `ClosestHit_Impostor` in `path_trace.hlsl`
- ✅ Correct reflections, shadows, and grazing angles via per-ray depth reconstruction (not flat billboard)

#### Procedural Vegetation Motion & BLAS
- ✅ Vegetation deformation compute shader (`vegetation_wind.hlsl`): pure time-based circular trunk bend (`primary_bend_strength`, `primary_bend_circle_time`) + constant leaf flutter (`wind_leaf_flutter`); per-species pseudorandom phase offset; cantilever beam deflection curve for natural trunk shape; no wind direction or physics simulation
- ✅ `PathTracer::dispatch_vegetation_wind()` updated to new parameter layout (16 DWORDs); `SpeciesDesc` fields renamed (`primary_bend_strength`, `primary_bend_circle_time`, `wind_leaf_flutter`); scene loader and scene file updated
- ✅ Per-species output vertex buffer allocation (`enable_wind_deform()`) and per-frame dispatch wired in `Renderer::render_frame_path_traced()`
- ✅ BLAS refit each frame for deformed meshes — `PathTracer::build_vegetation_lod_blas()` builds ALLOW_UPDATE BLAS per LOD; refit called after each dispatch + UAV barrier
- 🔲 Per-frame TLAS update for dynamic vegetation instances

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

### M17 — Displacement Mapping
- 🔲 `DisplacementBaker` class: CPU mesh subdivision + height-map displacement along normals + MikkTSpace normal/tangent recompute
- 🔲 Content-hash sidecar cache (`.displaced.bin`): skip bake on cache hit; re-bake only when source mesh or height map changes
- 🔲 `AssetImporter` integration: detect `height_map` field in material descriptor, invoke `DisplacementBaker` before GPU upload
- 🔲 PBR material descriptor extended: `height_map`, `displacement_scale`, `tessellation_factor` fields
- 🔲 `.marsscene` schema updated to support new displacement material fields
- 🔲 NVIDIA DMM detection + enhancement path *(deferred until baseline offline bake is validated)*

### M18 — Test App Polish
- 🔲 Hot-reload: `R` key reloads current scene without restart
- 🔲 Screenshot: `F12` saves HDR EXR to working directory
- 🔲 Gamepad support (XInput): analog steering, trigger accelerate/brake
- 🔲 Orbit camera (`F` toggle between free-fly and locked orbit)
- 🔲 Dev panel toggle with `~` key (ImGui, dev builds only)

### M19 — Performance Tuning, HDR Calibration, Shipping Polish
- 🔲 Per-display peak-brightness calibration (nits target)
- 🔲 ACES tone-map tuning for HDR10 / scRGB / SDR paths
- 🔲 `Repeated slDLSSGSetOptions()` call deduplication in `denoiser.cpp`
- 🔲 `ReportLiveObjects()` cleanup audit at process exit
- 🔲 FSR 4 / FSR 3 AMD fallback integration (GPUOpen FidelityFX SDK)
- 🔲 Final GPU timer profiling pass; pipeline bottleneck resolution

---

## Known Issues / Blockers
**Agents:** This section is for recording any known issues or blockers that are currently preventing progress on the implementation. Update this section with any such issues, along with a brief description and any relevant context or links to discussions.
- WinPixEventRuntime has not been wired into CMake yet; deferred to M15.
- FreeType + HarfBuzz font integration and msdfgen-ext are deferred to M16 (not needed until UI work begins).
- FSR 3 (AMD FidelityFX Super Resolution) is the open-source fallback for non-DLSS hardware;
  can be integrated via the GPUOpen FidelityFX SDK on GitHub when needed.

---

## Remaining warnings — all benign
**Agents:** This section is for recording any warnings that are currently emitted during development, but have been investigated and determined to be benign (i.e. not indicative of an actual bug or issue in the engine). Update this section with any such warnings, along with a brief explanation of their source and why they are considered non-problematic.

| Warning | Source | Analysis | Action |
|---|---|---|---|
| `D3D or VK API hook is activated without device being created` (×9 at startup) | Streamline `pluginManager.cpp:1331` | Emitted during `initializePlugins` while plugins register API hooks before `slSetD3DDevice` is called. This is the correct `pre_device_init()` → `device_init()` ordering required by Streamline; the messages are internal to SL's startup sequence and do not indicate an engine bug. | None — benign; no code change needed. |
| `Kernel mvec.cs:main … already created!` (×1) | Streamline `d3d12.cpp:1201` | Both `sl.dlss` and `sl.dlss_d` share the same motion-vector compute shader. The second plugin to register it triggers this one-time internal dedup warning. | None — benign; internal to Streamline. |
| `Repeated slDLSSGSetOptions() call for the frame 1` (×1) | Streamline `defines.h:364` | Streamline logs this when `slDLSSGSetOptions` is called twice before the same Present. Likely caused by one call during `evaluate_dlss_g` init and a second during the first frame's `evaluate_dlss_g`. The warning indicates redundancy, not a functional error. | Low priority — could deduplicate the options call in `denoiser.cpp` in a future cleanup pass (M17/M18). |
| `Live Object : 120` at process termination | D3D12 debug layer | The `D3D12Device` proxy is released by Streamline with `ref count 26`, meaning Streamline's internal DLSS-RR/DLSS-G resources hold device references that outlive the proxy release. All listed live objects have `Refcount: 0` except Streamline-owned internal textures/pipelines (`Refcount: 1`). This is expected Streamline behaviour at process exit. The D3D12 debug layer note says "Process is terminating — using simple reporting", confirming this is a shutdown-time observation, not a runtime leak. | Low priority — add `ReportLiveObjects()` call in `DeviceContext::shutdown()` (M15) for a more informative named-object dump during development; no engine resource leak to fix now. |

---

## Decisions Made
**Agents:** This section is for recording any design decisions made during implementation, so that future agents can understand the rationale and constraints behind the current codebase. Update this section with any significant architectural, API, or third-party library choices made during development.

| Decision |
|---|
| **Global wind system removed.** `WindDesc`, the layered CPU oscillator (gust + direction wander + micro + IIR smoother), `m_species_wind_states`, and the spring-envelope inertia model have been fully removed from the engine. Replaced by two independent per-instance procedural systems: (1) vegetation uses a pure time-based circular bend in `vegetation_wind.hlsl`; (2) cloth uses a new PASS 3 (WAVE) traveling sine wave in `cloth_sim.hlsl`. |
| **Vegetation procedural motion uses time-only inputs — no wind vector.** `primary_bend_strength` (amplitude) and `primary_bend_circle_time` (seconds per full rotation) control the constant imperceptible trunk bend. `wind_leaf_flutter` controls constant-amplitude leaf flutter. This eliminates all CPU-side spring physics and GPU wind direction tracking. |
| **Cloth wave pass (Option A) added as PASS 3 in `cloth_sim.hlsl`.** Injects a traveling sine wave into `pos_curr` before INTEGRATE each substep. Per-instance `wind_direction` (Vec3) and `wave_amplitude` (float) are parsed from `ClothDesc` in the scene file, replacing the old global `wind` vector from `WindDesc`. |
| DirectX 12 + DXR chosen as the sole graphics API — no Vulkan, no OpenGL |
| DLSS 4 as primary upscaler/denoiser/frame generator (Transformer model, Multi Frame Generation); FSR 4 / FSR 3 as open-source AMD fallback |
| ReSTIR DI + ReSTIR GI chosen for lighting; no baked lightmaps |
| `.marsscene` JSON format chosen for scene description files |
| D3D12 Memory Allocator (D3D12MA) chosen for GPU memory management |
| ImGui scoped to developer/debug builds only — not the game UI |
| MSDF (Multi-channel Signed Distance Field) chosen for game UI font rendering |
| HarfBuzz chosen for Unicode text shaping (OpenType, RTL/BiDi) |
| Game UI described in `.marsui` JSON files; themed via JSON style sheets |
| **HDR is a required engine feature** — HDR10/PQ and scRGB paths must always be fully implemented; engine gracefully falls back to SDR tone-mapping on non-HDR displays |
| NVIDIA SDKs delivered via Streamline SDK v2.11.1 (DLSS 4 SR, Frame Gen, Ray Reconstruction, Reflex, NIS) and Nsight Aftermath 2025.5.0; both stored under `C:/mars_deps/` and integrated via custom CMake modules |
| DXC sourced from Windows SDK 10.0.26100.0; hardcoded as fallback hint in `cmake/CompileHLSL.cmake` — no separate install required |
| `FETCHCONTENT_UPDATES_DISCONNECTED ON` set globally in root `CMakeLists.txt` to prevent FetchContent re-clone failures caused by OneDrive file locks on `C:/mars_build/fetchcontent` |
| Wide string literals in Win32 window titles must use ASCII-only characters; em dashes and other multi-byte UTF-8 characters cause garbled titles without explicit `/utf-8` MSVC flag |
| `engine/CMakeLists.txt` uses `GLOB_RECURSE` for source discovery but also maintains an explicit `list(APPEND)` for new files so they are included immediately without waiting for a CMake re-run |
| Reflections (specular indirect) implemented via BRDF importance sampling in the path-tracer closest-hit shader: GGX VNDF sampling for metallic/specular lobes and cosine-hemisphere sampling for diffuse, merged into the same single-bounce indirect path as GI. No separate reflection milestone is needed. |
| **DLSS-RR `kBufferTypeDepth` requires NDC projected depth, not ray distance.** Resources written as world-space `hit_t` meters produce completely wrong disocclusion and reprojection. Always write `saturate(clip.z / clip.w)` where `clip = proj * view * worldPos`. Sky/miss pixels write `1.0`. |
| **DLSS-RR motion vectors must be NDC-space screen-space deltas with Y negated and scaled by 0.5.** Compute `delta = prevNDC.xy - currNDC.xy` then negate Y (DLSS-RR convention: Y+ = down) and multiply by `{0.5, 0.5}` (NDC range is ±1 = 2 units; DLSS-RR normalises to 1 = full screen width). Set `mvecScale = {0.5f, 0.5f}` in `sl::DLSSDOptions`. Set `cameraMotionIncluded = eTrue`. |
| **When `cameraMotionIncluded = eTrue`, `clipToPrevClip` and `prevClipToClip` must be identity matrices.** Passing real previous-frame clip matrices simultaneously causes DLSS-RR to apply camera reprojection twice — once from the motion vectors and once from the clip transform — making all motion appear at 2× speed. With full-scene motion vectors the clip transform matrices must be identity. |
| **`view_proj` passed to shaders must be `proj * view` (not `view * proj`).** HLSL `mul(M, v)` is column-vector (M × v). The C++ side must therefore compute `prev_view_proj = proj * view` when packing `FrameConstants`. Reversing the order produces garbage clip-space coordinates resulting in enormous garbage motion vectors. |
| **`prev_view_proj` must be seeded to the current VP on the very first frame.** Leaving it zero-initialised on frame 0 produces NaN/Inf motion vectors that permanently corrupt DLSS-RR's history buffer. Detect first frame in `Renderer::set_camera()` and initialise `cam.prev_view_proj = curr_view_proj`. |
| **Render resolution and display resolution must be explicitly separate throughout the pipeline.** DLSS-RR upscales from render resolution to display resolution. Path-tracer noisy-color, motion-vector, depth, and AOV textures must be allocated at render resolution; the denoised output UAV must be allocated at display resolution. Mixing the two causes the image to appear viewport-offset or cropped. |
| **Sky mode is a three-way enum (`Debug`, `Physical`, `HDRI`) written into `FrameConstants::sky_mode` and branched in the DXR miss shader.** `Debug` = 0 (coloured cube faces + grid + crosshair), `Physical` = 1 (analytic procedural atmosphere), `HDRI` = 2 (equirectangular texture sampled via `g_Textures[hdri_sky_slot]`). Selected by `"type"` in the `"skybox"` scene block. |
| **`DirectX::LoadFromHDRFile` only decodes Radiance `.hdr` — it cannot read OpenEXR `.exr` files.** Using it for `.exr` silently returns a failed HRESULT, causing `load_texture()` to return `UINT32_MAX` and the renderer to fall back to the debug sky. Fix: route `.exr` through tinyexr (`LoadEXR` → RGBA float32 → `DXGI_FORMAT_R32G32B32A32_FLOAT` `ScratchImage` → existing GPU upload path). |
| **Compile tinyexr and miniz in dedicated third-party translation units (`exr_loader.cpp`, `miniz.c`) with `/W0 /WX-` via `set_source_files_properties`.** This prevents upstream warnings from failing the strict `/WX` engine build. `texture_loader.cpp` includes `tinyexr.h` with `TINYEXR_USE_MINIZ 0` (declarations only); `exr_loader.cpp` defines `TINYEXR_IMPLEMENTATION` and `TINYEXR_USE_MINIZ 1` (one implementation TU). |

## Notes & Lessons Learned
**Agents:** This section is for recording any additional notes, observations, or lessons learned during implementation that may be useful for future agents working on the codebase. Update this section with any such insights, along with any relevant context or links to discussions.

| Note |
|---|
| FetchContent builds must use `FETCHCONTENT_UPDATES_DISCONNECTED ON` globally to prevent re-clone failures caused by OneDrive file locks on `C:/mars_build/fetchcontent`. |
| Build/fetch directories moved to `C:/mars_build/` to avoid OneDrive locking FetchContent git pack files. Set in `CMakePresets.json`. |
| Wide string literals in Win32 window titles must use ASCII-only characters; em dashes and other multi-byte UTF-8 characters cause garbled titles without explicit `/utf-8` MSVC flag. |
| `engine/CMakeLists.txt` uses `GLOB_RECURSE` for source discovery but also maintains an explicit `list(APPEND)` for new files so they are included immediately without a CMake re-run. |
| HarfBuzz compiled with `HB_HAVE_FREETYPE=OFF` and msdfgen with `MSDFGEN_CORE_ONLY=ON` until font pipeline work begins in M16. |
| `Camera` (`camera.h` / `camera.cpp`) is fully implemented as of M5: view/proj matrices, inverse matrices, Halton jitter, previous-frame matrices for motion vectors, and per-display yaw/pitch/roll offset support. `FlyCamera` (WASD + mouse-look) wraps `Camera` and is the camera used by `test_app`. |
| **Streamline `slSetTag` resource state must match actual D3D12 state.** The `sl::Resource::state` field passed in `tag_resources` must reflect the resource's *current* D3D12 state at the point of tagging, not `0` / `COMMON`. Streamline uses this value to issue its own internal legacy barrier transitions before and after DLSS evaluation. Passing `0` when resources are in `D3D12_RESOURCE_STATE_UNORDERED_ACCESS` causes GPU-Based Validation error `#1358: GPU_BASED_VALIDATION_INCOMPATIBLE_TEXTURE_LAYOUT` inside the DLSS-RR compute dispatch, crashing `ExecuteCommandLists` with `0x87A`. All UAV-backed resources tagged by the engine must use `r.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS` — **except** `DenoisedOutputUAV` / `kBufferTypeScalingOutputColor` (see rule below). |
| **`DenoisedOutputUAV` must be created in `D3D12_RESOURCE_STATE_COMMON` and reset to `D3D12_BARRIER_LAYOUT_COMMON` via Enhanced Barrier after each `slEvaluateFeature` call.** Streamline's `slEvaluateFeature` (DLSS-RR) writes to the denoised output texture using the D3D12 Enhanced Barrier API internally, leaving it in `D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS`. On the *next* frame, Streamline reads `r.state` and issues a legacy `ResourceBarrier` from that state. The D3D12 spec forbids legacy barriers on any resource whose current Enhanced layout is not `D3D12_BARRIER_LAYOUT_COMMON`, causing `RESOURCE_BARRIER_INVALID_COMBINATION` (#526) + `GPU_BASED_VALIDATION_INCOMPATIBLE_TEXTURE_LAYOUT` (#1358) → fatal `0x87A` crash at `ExecuteCommandLists` on every frame after the first. Correct pattern: (1) allocate `DenoisedOutputUAV` in `D3D12_RESOURCE_STATE_COMMON`; (2) tag it as `kBufferTypeScalingOutputColor` with `r.state = D3D12_RESOURCE_STATE_COMMON`; (3) after `slEvaluateFeature`, issue a `D3D12_TEXTURE_BARRIER` via `ID3D12GraphicsCommandList7::Barrier()` transitioning `D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS → D3D12_BARRIER_LAYOUT_COMMON`. `COMMON` is the only Enhanced layout that re-enables legacy barrier compatibility and also permits implicit SRV promotion for the tone-map blit that follows. |
| **All required `sl::Constants` fields must be explicitly set before calling `slSetConstants` / `slEvaluateFeature`.** Streamline silently skips `slEvaluateFeature` if any of the following fields are left at their zero/default values: `cameraPos`, `cameraUp`, `cameraRight`, `cameraFwd`, `cameraFOV`, `cameraAspectRatio`, `cameraPinholeOffset`, `prevClipToClip`, `motionVectorsInvalidValue`. This causes the denoised output texture to remain in its last-known layout (e.g. `COMMON`), while a post-evaluate Enhanced Barrier that assumes the resource was written (`LayoutBefore=UAV`) fires unconditionally, producing `INCOMPATIBLE_BARRIER_LAYOUT` (#1334) and a `0x87A` crash. Always derive these from camera data: `cameraPos/Right/Up/Fwd` from `view_inv` rows; `cameraFOV` via `2*atan(1/proj.m[1][1])`; `cameraAspectRatio` via `proj.m[1][1]/proj.m[0][0]`; `prevClipToClip` as an explicit identity (not zero-init); `motionVectorsInvalidValue = 1e8f`. |
| **After `slEvaluateFeature` (DLSS-RR), path-tracer input resources are left in `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE`, not `COMMON`.** Streamline internally reads the noisy color, motion vector, and depth resources as SRVs during the DLSS-RR pass and leaves them in `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE` (0x80). The post-denoise barrier block that returns these resources to `D3D12_RESOURCE_STATE_UNORDERED_ACCESS` for the next frame must therefore set `StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE`. Using `StateBefore = D3D12_RESOURCE_STATE_COMMON` produces `RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH #527` and a `0x87A` crash at `ResourceBarrier`. |
| **ReSTIR GI reservoir reuse requires extreme care with the `W` (unbiased contribution weight) field.** `GIReservoirFinalize` computes `W = w_sum / (M * p_hat)`. If `p_hat ≈ 0` (e.g. because `EvaluatePBR` returned zero due to a wrong `V` direction on secondary rays) then `W → ∞`. That infinity propagates through every subsequent `GIReservoirMerge` call (`merge weight = p_hat * b.W * b.M`), filling the reservoir buffer with `+INF` values within one frame and causing the brightness explosion on frame 1. The safe baseline is a plain MC estimator: `giContrib = (EvaluatePBR(…) / NdotL_sec) * Lo * PI` — no reservoir, no temporal state, no initialization dependency. Reservoir reuse can be re-introduced on top of that confirmed baseline once the estimator is validated. |
| **Never alias the same `ID3D12Resource` under multiple `sl::BufferType` tags in a single `slSetTagForFrame` call.** |
| **DLSS-RR temporal artifacts (ghosting, shimmer, blurring, trails) almost always have multiple compounding root causes.** The stable fix required aligning all of the following simultaneously: NDC depth (not ray distance), correct `proj * view` matrix order, proper previous-frame seeding, NDC motion vectors, Y-axis sign flip, `mvecScale = {0.5, 0.5}`, `cameraMotionIncluded = eTrue`, and identity `clipToPrevClip`. Fixing one in isolation often appeared to improve one artifact while creating a new one. Treat them as a single coherent contract, not independent knobs. |
| **For DLSS-RR sky / background motion vectors, project a far-plane point (not `ray.Direction`).** Reconstruct a world-space far-plane point from the camera for each sky pixel: `worldPos = cameraPos + rayDir * far_plane_distance`. Project that through both `view_proj` and `prev_view_proj` to get a finite, well-scaled sky motion vector. Using raw `ray.Direction` or a constant produces incorrect sky reprojection during camera rotation or zoom. |
| **ReSTIR GI reuse should only be introduced on top of a confirmed, stable single-bounce MC baseline.** When reservoir reuse caused brightness explosions and black indirect lighting simultaneously, the safest resolution was to revert to the plain MC estimator and validate it in isolation before re-introducing any temporal state. The single-bounce estimator (`giContrib = brdf * Lo * pi / pdf`) is the correct starting point for all future reuse work. |
| **Multi-bounce GI uses a throughput-accumulating payload, not per-bounce re-entry into the full ClosestHit logic.** `PrimaryPayload.throughput` carries the accumulated `BRDF/pdf` product. Each hit fires a secondary ray with `depth+1` and `throughput *= bounceWeight`. At depth >= 1, Russian Roulette (survival = `max_component(throughput)`) terminates paths stochastically. Shadow rays and AOV G-buffer writes remain depth-0 only. Miss shader scales sky radiance by `payload.throughput` at depth > 0. RTPSO max recursion is 3. The public toggle is `PathTracer::set_gi_bounce_count(uint32_t)` (0=off, 1=single-bounce default, 2=two-bounce). |
| **Reflections (specular indirect) do not require a separate render pass or a dedicated milestone.** They emerge naturally from the single-bounce indirect path when the secondary-ray direction is sampled from the BRDF lobe (GGX VNDF for specular, cosine hemisphere for diffuse) rather than from a fixed cosine-hemisphere sampler. The key fix was switching the indirect sampler to BRDF importance sampling in `pbr_brdf.hlsli`. |

---
