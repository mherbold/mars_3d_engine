# PLAN.md — MARS 3D Engine: High-Level Implementation Plan

> **Target hardware:** NVIDIA RTX 4000 / 5000 series (or equivalent AMD RDNA 3/4)  
> **Graphics API:** DirectX 12 + DirectX Raytracing (DXR) — no legacy fallback  
> **Display:** HDR output **fully supported and required as an engine feature** (HDR10 / scRGB); engine gracefully falls back to SDR tone-mapping on non-HDR displays
> **Language:** C++23 · **Build:** CMake ≥ 3.28

---

## Table of Contents
1. [Repository & Build Infrastructure](#1-repository--build-infrastructure)
2. [Core Engine Architecture](#2-core-engine-architecture)
3. [Graphics Device & Command Infrastructure](#3-graphics-device--command-infrastructure)
4. [Multi-Monitor Display System](#4-multi-monitor-display-system)
5. [Path-Tracing Renderer](#5-path-tracing-renderer)
6. [Denoising & Post-Processing](#6-denoising--post-processing)
7. [Scene Management & File Format](#7-scene-management--file-format)
8. [Asset Pipeline](#8-asset-pipeline)
9. [Camera System](#9-camera-system)
10. [Procedural Ecosystem (Vegetation & Trees)](#10-procedural-ecosystem-vegetation--trees)
11. [Crowd Rendering](#11-crowd-rendering)
12. [Particle System](#12-particle-system)
13. [Weather System](#13-weather-system)
14. [Decal System (Tire Tracks / Skid Marks)](#14-decal-system-tire-tracks--skid-marks)
15. [Animation System](#15-animation-system)
16. [Lighting & Shadow Strategy](#16-lighting--shadow-strategy)
17. [Reflection System](#17-reflection-system)
18. [Debug & Profiling Overlay (Developer-Only)](#18-debug--profiling-overlay-developer-only)
19. [Game UI System](#19-game-ui-system)
20. [Displacement Mapping](#20-displacement-mapping)
21. [Test Application](#21-test-application)

---

## 1. Repository & Build Infrastructure

### Goals
- Clean CMake super-build: `engine/` (static lib) and `test_app/` (Win32 exe)
- NuGet or `FetchContent` for third-party dependencies
- Single-header public API (`mars_engine/mars_engine.h`)
- Separate `shaders/` tree with a CMake custom target that compiles HLSL → DXIL via **DXC**

### Key Tasks
- [x] Add Windows Agility SDK (D3D12 1.614+) via NuGet — `Microsoft.Direct3D.D3D12` 1.614.1 in `C:/mars_deps/nuget/`
- [x] Add DirectXTK12, DirectXTex, DirectXMesh via NuGet / vcpkg — all 2026.x versions extracted, CMake targets wired
- [x] Add Assimp, nlohmann/json via `FetchContent` — Assimp 5.4.3, nlohmann/json 3.11.3
- [x] Add ImGui via `FetchContent` *(developer/debug builds only — not the game UI)* — 1.91.8, gated by `MARS_ENABLE_DEV_UI`
- [x] Add D3D12 Memory Allocator (D3D12MA) via `FetchContent` — 2.0.1
- [x] Add HarfBuzz via `FetchContent` (Unicode text shaping for game UI) — 9.0.0 (`HB_HAVE_FREETYPE=OFF` until M16)
- [x] Add FreeType via `FetchContent` (font metrics for msdfgen and HarfBuzz at build time) — 2.13.3 (wired at M16)
- [x] Add msdfgen as CMake build-tool target (offline MSDF atlas generation) — 1.12 (`MSDFGEN_CORE_ONLY=ON` until M16)
- [x] Add NVIDIA DLSS 4 SDK — Streamline SDK v2.11.1; `cmake/StreamlineSDK.cmake`; all `sl.*` / `nvngx_*` DLLs auto-deployed
- [x] Add NVIDIA Shader Library / Aftermath — Nsight Aftermath 2025.5.0; `cmake/NsightAftermath.cmake`; DLLs auto-deployed
- [x] CMake shader compilation target: `.hlsl` → `.dxil` — `cmake/CompileHLSL.cmake`; DXC from Windows SDK 10.0.26100.0
- [x] CI-friendly directory layout; `.gitignore` for `build/`; `CMakePresets.json` with debug/release presets
- [ ] Add WinPixEventRuntime via NuGet *(deferred to M15)*

---

## 2. Core Engine Architecture

```
MarsEngine (singleton)
├── DeviceContext          — D3D12 device, queues, descriptor heaps
├── DisplayManager         — enumerates monitors, owns DisplayOutput list
│   └── DisplayOutput[]    — per-monitor swap chain + viewport
├── ResourceManager        — GPU buffer / texture pool, upload heap
├── SceneManager           — loads / unloads scenes, owns the scene graph
│   └── Scene
│       ├── SceneNode[]    — transform hierarchy
│       ├── MeshInstance[] — geometry + material refs
│       ├── Light[]
│       ├── Camera[]
│       └── PerSceneConfig — ecosystem density maps, weather params, particle emitter defs, crowd paths…
├── Renderer               — top-level render orchestrator
│   ├── PathTracer         — DXR pipeline, TLAS/BLAS management
│   ├── Denoiser           — DLSS 4 / NRD integration
│   └── PostProcessor      — bloom, lens flare, film grain, tone mapping, chromatic aberration
├── AnimationSystem
├── ParticleSystem
├── WeatherSystem
├── DecalSystem
├── CrowdSystem               — CPU boid simulation, Copy-queue upload, crowd TLAS registration
├── UISystem                  — game UI (HUD, menus, overlays); production code, all builds
│   └── UIRenderer            — geometry upload, rasterization pass, HDR composite
└── DebugOverlay               — ImGui; developer/debug builds only
```

### Lifecycle
```
MarsEngine::Initialize(config)
  → DeviceContext::Create()
  → DisplayManager::EnumerateOutputs()
  → ResourceManager::Initialize()
  → SceneManager::Initialize()
  → AnimationSystem::Initialize()
  → ParticleSystem::Initialize()
  → WeatherSystem::Initialize()
  → DecalSystem::Initialize()
  → CrowdSystem::Initialize()
  → Renderer::Initialize()
  → UISystem::Initialize()

per frame:
  AnimationSystem::Update(dt)       ← evaluate animation clips → CPU bone palettes
  AnimationSystem::DispatchSkinning() ← GPU compute: apply bone palette → skinned vertex buffers (must complete before BLAS refit)
  ParticleSystem::Update(dt)
  WeatherSystem::Update(dt)
  DecalSystem::Update(dt)         ← emit new decals, evict oldest from ring buffer, upload structured buffer
  CrowdSystem::Update(dt)         ← run CPU boid simulation, upload agent positions to GPU structured buffer via Copy queue
  SceneManager::BuildFramePacket()
  UISystem::BuildDrawLists()        ← traverse widget tree once per frame (display-agnostic)
  Renderer::RenderFrame(packet)
    for each DisplayOutput:
      PathTracer::Trace()           ← includes BLAS refit; requires skinned vertex buffers to be ready
      Denoiser::Denoise()
      PostProcessor::Process()
      UIRenderer::DispatchMinimap() ← secondary ray-gen → Minimap UAV texture
      UIRenderer::Render()          ← rasterization composite onto this display's frame
      DisplayOutput::Present()      ← MFG runs here if enabled

MarsEngine::Shutdown()
```

---

## 3. Graphics Device & Command Infrastructure

| Component | Technology |
|---|---|
| Device creation | `D3D12CreateDevice` + Agility SDK |
| Feature check | DXR Tier 1.1, SM 6.6, Enhanced Barriers, Wave Intrinsics |
| Command queues | Direct (3D+RT), Compute (async denoiser), Copy (streaming) |
| Descriptor heaps | CBV/SRV/UAV bindless heap (1M slots), RTV heap, Sampler heap |
| Memory | D3D12MA (D3D12 Memory Allocator by AMD/GPUOpen) |
| Swap chain | `IDXGISwapChain4` — **HDR10 / scRGB fully supported**; `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` (PQ) or `_G10_NONE_P709` (scRGB linear) activated when available; graceful SDR fallback via ACES tone-map; tearing support via `DXGI_PRESENT_ALLOW_TEARING` |
| Synchronization | `ID3D12Fence` per queue; frame-in-flight ring (2 frames — double-buffered; keeps input latency minimal before DLSS MFG adds its own) |
| GPU debug | NVIDIA Aftermath SDK for GPU crash dumps |
| PIX integration | WinPixEventRuntime for GPU captures |

### Bindless Rendering
All textures and buffers are registered in a single large CBV/SRV/UAV heap.
Shaders receive a root constant containing a per-draw "descriptor table index",
eliminating per-draw descriptor binding.

---

## 4. Multi-Monitor Display System

### Design
- On startup, enumerate all `IDXGIOutput` objects attached to the adapter.
- For each monitor the user has enabled in the config, create a `DisplayOutput`:
  - `IDXGISwapChain4` — **HDR10 (PQ / ST.2084) and scRGB fully supported**; engine queries `CheckColorSpaceSupport` and activates HDR color space when available, falling back to SDR tone-mapping on non-HDR displays
  - Associated `Camera` (with its own frustum, transform, and FOV)
  - Optional `MonitorAngle` metadata for surround / overhead setups
- The renderer dispatches one full path-trace pass per `DisplayOutput` per frame.
- Monitors can share TLAS/scene data but each has its own ray-generation shader dispatch.

### Config (in `.marsscene` or separate `display.json`)
```json
{
  "displays": [
    { "monitor_index": 0, "role": "center",   "yaw_offset_deg":   0, "fov": 90 },
    { "monitor_index": 1, "role": "left",     "yaw_offset_deg": -40, "fov": 50 },
    { "monitor_index": 2, "role": "right",    "yaw_offset_deg":  40, "fov": 50 },
    { "monitor_index": 3, "role": "overhead", "pitch_offset_deg": 70, "fov": 60 }
  ]
}
```

---

## 5. Path-Tracing Renderer

### Pipeline Overview
1. **Acceleration Structure Build** — build / update BLAS per mesh (including compute-skinned and wind-deformed meshes), compose TLAS
2. **Ray Generation** — one ray per pixel at the internal render resolution (which is lower than output resolution when DLSS upscaling is active — e.g. 67% at Quality preset, 50% at Performance)
3. **Hit shaders** — PBR material evaluation (GGX BRDF, Disney Principled)
4. **Miss shaders** — three-way sky branch: `sky_mode 0` = debug procedural cube, `sky_mode 1` = analytic physical sky, `sky_mode 2` = HDRI equirectangular texture lookup
5. **Lighting & Global Illumination** — Next Event Estimation (NEE) + Multiple Importance Sampling (MIS); Russian Roulette path termination, up to N bounces (configurable)
6. **Denoising** — DLSS 4 Ray Reconstruction (primary): raw noisy frames + motion vectors fed directly into DLSS; temporal integration handled internally. NRD fallback: explicit temporal accumulation buffer (weighted blend with reprojection) applied first, then NRD denoiser.
7. **Upscaling** — DLSS 4 Super Resolution reconstructs full output resolution from the internal render resolution

### HLSL Shader Files
```
engine/shaders/
├── path_trace.hlsl            ← ray generation, closest hit, miss
├── shadow_ray.hlsl            ← shadow visibility rays
├── material/
│   ├── pbr_brdf.hlsli         ← GGX / Disney BRDF
│   └── material_data.hlsli    ← bindless material fetch
├── sky/
│   ├── physical_sky.hlsli     ← Hillaire atmospheric scattering
│   └── hdri_sky.hlsli
├── gi/
│   ├── restir_di.hlsl         ← ReSTIR Direct Illumination
│   └── restir_gi.hlsl         ← ReSTIR Global Illumination
└── common/
    ├── math.hlsli
    ├── random.hlsli            ← PCG hash / Halton sequence
    └── bindless.hlsli
```

### Key Algorithms
| Feature | Algorithm |
|---|---|
| Direct lighting | ReSTIR DI (Spatiotemporal Reservoir Resampling) |
| Global illumination | BRDF-importance-sampled MC path tracing with configurable bounce depth (`gi_bounce_count`); Russian Roulette termination; DLSS-RR denoise |
| Sky model | Three-mode enum: `Debug` (procedural colour-coded cube + grid), `Physical` (analytic atmosphere in HLSL), `HDRI` (equirectangular `.exr`/`.hdr` texture). Selected via `"type"` in the `"skybox"` scene block. EXR loading uses tinyexr v1.0.9. |
| BRDF | Disney Principled PBR / GGX + Smith |
| Anti-aliasing / upscale | DLSS 4 Super Resolution — Transformer model (RTX 40/50 series); FSR 4 / FSR 3 as AMD fallback |
| Denoising | DLSS 4 Ray Reconstruction (primary) or NVIDIA NRD (fallback) |
| Caustics | Naturally captured by multi-bounce path tracing; optionally boosted by light-path connection strategies (BDPT-style shadow connections) — no separate photon mapping pass |

---

## 6. Denoising & Post-Processing

| Pass | Tech |
|---|---|
| Denoiser | DLSS 4 Ray Reconstruction — Transformer model (primary), NRD (fallback) |
| Upscaler | DLSS 4 Super Resolution — Transformer model (RTX 40/50 series); FSR 4 / FSR 3 as AMD fallback |
| Bloom | Dual Kawase blur on the linear HDR buffer — applied **before** tone mapping so it operates on physical luminance values |
| Lens flare | Procedural flare with occlusion tested via a lightweight visibility ray cast to the light source (no depth buffer exists) |
| Film grain | Added in linear light **before** tone mapping; grain amplitude scaled to scene luminance so it maps through PQ/ACES without banding |
| Tone mapping | **HDR10 (PQ / ST.2084) fully supported** — ACES filmic curve maps linear scene luminance (post-bloom, post-grain) to ST.2084 PQ on HDR displays; scRGB path available for Windows HDR; graceful ACES SDR tone-map on non-HDR displays; peak brightness configurable per display (nits) |
| Chromatic aberration | Optional compute shader post-process on the final (tone-mapped) image UAV; configurable per-display |
| UI composite | **Game UI (`UIRenderer`)** composited over path-traced frame via rasterization (all builds); **ImGui** additionally composited in dev/debug builds only |
| Frame generation | DLSS 4 Multi Frame Generation — operates on the fully composited output image; up to 3 additional generated frames (4× total, RTX 50 series); 1 additional frame (2× total, RTX 40 series); runs immediately before Present |

### Albedo AOV / Normals AOV / Roughness AOV
Dedicated AOV UAV textures are allocated in `PathTracer` as of M6 (alongside `m_outputs`, `m_mv_outputs`, `m_depth_outputs`). All four buffers are tagged each frame in `Denoiser::tag_resources()`:
- `kBufferTypeAlbedo` / `kBufferTypeSpecularAlbedo` → `m_albedo_outputs` / `m_specular_albedo_outputs`
- `kBufferTypeNormals` / `kBufferTypeRoughness` → `m_normals_aov_outputs` / `m_roughness_aov_outputs`

As of M7, the closest-hit shader writes real G-buffer data each frame:
1. Diffuse base color (no lighting) → albedo UAV
2. Specular/metallic color (F0) → specular albedo UAV
3. World-space geometric normal → normals UAV
4. Perceptual roughness → roughness UAV (R8_UNORM)

### DLSS-RR Temporal Contract (established M6, validated post-M8)
The following invariants must be maintained for artifact-free DLSS-RR output:
- `kBufferTypeDepth` contains NDC projected depth: `saturate(clip.z / clip.w)`. Never ray distance.
- Motion vectors are NDC-space deltas (`prevNDC - currNDC`), Y-negated (screen-space convention), multiplied by `{0.5, 0.5}`. `mvecScale = {0.5f, 0.5f}`.
- `cameraMotionIncluded = eTrue` — MVs encode full-scene motion (camera + objects).
- `clipToPrevClip` and `prevClipToClip` are identity matrices when `cameraMotionIncluded = eTrue`.
- `view_proj` uploaded to shaders is `proj * view` (column-vector convention).
- `prev_view_proj` is seeded to the current VP on the very first frame to produce zero MVs on frame 0.
- Path-tracer inputs (noisy color, MVs, depth, AOVs) are allocated at **render resolution**; denoised output is allocated at **display resolution**.
- Sky/background pixels write motion vectors by projecting a far-plane world point through both `view_proj` and `prev_view_proj`.

---

## 7. Scene Management & File Format

### `.marsscene` JSON Schema (abbreviated)
```json
{
  "scene": {
    "name": "Test Track",
    "skybox": { "type": "hdri", "hdri": "models/hdri/sky.exr", "sun_direction": [0.3, 0.8, 0.1], "sun_intensity": 10 },
    "wind": [0.0, 0.0, 0.0],
    "models": [
      {
        "id": "track_mesh",
        "file": "assets/models/track.fbx",
        "transform": { "position": [0,0,0], "rotation": [0,0,0], "scale": [1,1,1] }
      }
    ],
    "ecosystem": {
      "enabled": true,
      "density_map": "assets/textures/vegetation_density.png",
      "species": [ { "mesh": "assets/models/oak_tree.fbx", "lod_levels": 3 } ]
    },
    "crowd": { "enabled": false },
    "weather": { "type": "clear", "wind_direction": [1,0,0], "wind_speed": 5.0 },
    "lights": [
      { "type": "directional", "direction": [0.3,0.8,0.1], "intensity": 10, "color": [1,0.98,0.95] }
    ],
    "cameras": [
      { "id": "player_cam", "fov": 90, "near": 0.1, "far": 10000 }
    ]
  }
}
```

### SceneManager Responsibilities
- Parse `.marsscene` on load; hot-reload support (file watcher)
- Build and maintain the scene graph (transform hierarchy)
- Manage BLAS / TLAS updates each frame (static vs. dynamic geometry)
- Stream assets in/out based on camera distance (background thread + Copy queue)

---

## 8. Asset Pipeline

| Asset Type | Format | Importer |
|---|---|---|
| 3D models | FBX, glTF 2.0, OBJ | Assimp |
| Textures | DDS (BC7/BC6H), PNG, EXR | DirectXTex |
| HDR skies | EXR / HDR | DirectXTex |
| Animations | FBX embedded, glTF | Assimp |
| Audio | — | Out of scope (Phase 1) |

### Runtime Asset Flow
```
Raw file → Assimp/DirectXTex → Intermediate CPU buffer
         → Upload heap → Default heap (GPU resident)
         → Descriptor registered in bindless heap
```

Meshes are stored as interleaved vertex buffers (position | normal | tangent | uv | bone weights) in raw structured buffers, and index buffers in `DXGI_FORMAT_R32_UINT`.

---

## 9. Camera System

- `Camera` owns: view matrix, projection matrix, frustum planes, jitter sequence (sub-pixel offsets required as DLSS 4 input; temporal accumulation is handled internally by DLSS RR, not by the camera), and previous-frame view/projection matrices consumed by the path tracer to compute per-pixel motion vectors
- Motion vectors rendered by the path tracer as a UAV output pass each frame (required by DLSS 4 / NRD); the Camera supplies the previous-frame matrices needed for this computation
- `FlyCamera` (test app): WASD + mouse look, configurable speed & sensitivity
- `RacingCamera` (future): follows a physics vehicle with spring arm
- Per-`DisplayOutput` camera offset (yaw/pitch/roll) applied on top of base camera transform for surround setups

---

## 10. Procedural Ecosystem (Vegetation and Trees)

### SpeedTree Asset Compatibility
The engine targets compatibility with SpeedTree ORCA v2 assets (NVIDIA Open Research Content Archive).
Each species ships with:
- HighPoly/ - full-detail FBX mesh (used for near LOD)
- LowPoly/ - reduced FBX mesh (used for mid LOD / OMM cards)
- Textures/ - DDS textures using the GGX PBR channel convention:
  - BaseColor - RGB = base colour, A = Opacity mask (foliage alpha cutout)
  - Specular - R = Occlusion, G = Roughness, B = Metalness
  - Normal - DirectX convention (Y-up, right-handed tangent space)

Free assets on hand: Boston Fern, European Linden, Hedge, Japanese Maple, Red Maple Young, White Oak.

---

### LOD Tiers

| Tier | Distance | Representation | Alpha Handling |
|---|---|---|---|
| LOD 0 - Near | < ~50 m | HighPoly FBX mesh (full geometry, alpha-tested leaves/cards) | Opacity Micromaps (OMM, DXR 1.2) - hardware alpha-test; no AnyHit shader invoked |
| LOD 1 - Mid | 50-200 m | LowPoly FBX mesh (simplified cards) | Opacity Micromaps (OMM) |
| LOD 2 - Far cluster | 200-600 m | Simplified alpha cards (generated offline per species) | Opacity Micromaps (OMM) |
| LOD 3 - Impostor | > 600 m | Octahedral impostor with packed depth + opacity | Procedural TLAS geometry; depth parallax corrects reflections/shadows/grazing |

---

### Octahedral Impostors (LOD 3)
Far-distance trees use octahedral impostors, not flat camera-facing billboards. Pure flat alpha cards produce visible artifacts in a path tracer: wrong depth in reflections, incorrect self-shadowing, bad appearance at grazing angles. Octahedral impostors fix this:

- Pre-bake a hemispherical grid of view directions onto an octahedral atlas (e.g. 16x16 views) storing radiance/BaseColor+Opacity and per-texel encoded depth offset from the impostor centre plane.
- At ray hit time the intersection shader reconstructs the approximate surface normal and position from the stored depth, enabling a per-ray parallax correction - the ray effectively pierces the volume at the right depth, giving correct reflections, correct shadow termination, and correct appearance at grazing angles.
- The impostor is registered as a procedural AABB primitive in the TLAS with a custom intersection shader; no rasterization is involved.

---

### LOD Transition - Stochastic Dither (no alpha blending)
The transition between any two LOD tiers uses stochastic (noise-based) selection, not alpha blending of the full mesh. Alpha-blending two LOD levels simultaneously causes double energy, ray-tracing visibility weirdness (both BLAS entries visible simultaneously), and shadow/reflection mismatch.

Instead, each ray independently selects one LOD tier using a stable per-instance, per-pixel hash:

- Primary visibility rays: blue-noise pixel hash for low-discrepancy screen coverage.
- Secondary rays (reflections, GI): hashed instance + sample index (no screen-space coherence needed).
- Temporal scrambling: scramble the hash seed per-frame with a low-amplitude counter so the stochastic selection integrates over time; DLSS/denoiser resolves the blend.

Example: float h = Hash01(instanceId, pixelId, sampleIndex); bool useHigherLod = (h >= lodT);

---

### Alpha Foliage - Opacity Micromaps (OMM)
For all LOD tiers that use alpha-tested leaf/card geometry, the engine uses DXR 1.2 Opacity Micromaps to avoid expensive AnyHit shader invocations per ray:

- OMMs encode per-micro-triangle opacity states (Opaque / Transparent / Unknown) into a compact bitmask attached to the BLAS.
- The BVH traversal hardware uses the OMM to skip transparent micro-triangles or promote opaque ones, without entering the AnyHit shader.
- Unknown micro-triangles fall through to AnyHit as normal (safety net for sub-texel detail).
- OMMs are baked offline per mesh + BaseColor alpha channel during asset conditioning, stored alongside the BLAS.

OMM is the canonical modern D3D12/DXR 1.2 answer. Fallback (hardware without OMM support): AnyHit shader performing manual alpha test.

---

### Procedural Vegetation Motion

> **Note:** The global `WindDesc` wind system (CPU layered oscillator, spring inertia, per-species `m_species_wind_states`) has been **removed**. Vegetation motion is now fully deterministic and time-based — no wind direction input, no physics simulation.

Deformation is implemented in `engine/shaders/vegetation_wind.hlsl`, dispatched per-species per-frame by `Renderer::render_frame_path_traced()` via `PathTracer::dispatch_vegetation_wind()`.

#### Trunk Bend
The trunk is modelled as a fixed-base cantilever beam. A constant-amplitude bend rotates in a full circle over `primary_bend_circle_time` seconds. Displacement follows the analytic beam deflection curve `d(h) = (3h² − h³) / 2`, enforcing zero displacement and zero slope at the root. A **per-species pseudorandom phase offset** (golden-ratio Knuth hash of the species index) prevents multiple species from animating in synchronised lockstep.

#### Leaf Flutter
Individual leaf vertices flutter at constant amplitude controlled by `wind_leaf_flutter`. Flutter is restricted by a spatial mask (`h > 0.55` AND `off_axis > 0.5`) to upper-canopy off-axis vertices only.

#### Schema (`SpeciesDesc`)

| Field | Purpose |
|---|---|
| `primary_bend_strength` | Trunk bend amplitude |
| `primary_bend_circle_time` | Seconds for one full circular rotation of the bend direction |
| `wind_leaf_flutter` | Constant-amplitude leaf flutter strength |

#### GPU Pipeline
- Deformation dispatched per-species per-frame (one dispatch per mesh submesh)
- Deformed vertices written to a species-shared GPU output buffer
- BLAS refit issued immediately after each UAV barrier on the output buffer
- TLAS rebuilt once per frame after all BLAS refits complete

---

### Cloth / Flag Wave Motion

Cloth uses the canonical XPBD solver (INTEGRATE → CONSTRAIN×N → FINALIZE) extended with a new **PASS 3 (WAVE)** that runs before INTEGRATE each substep. The wave pass injects a traveling sine-wave positional offset into `pos_curr`, producing the flag-wave appearance without a physical wind force.

#### Schema (`ClothDesc`)

| Field | Purpose |
|---|---|
| `wind_direction` | Vec3 — direction the wave travels across the cloth |
| `wave_amplitude` | Float — peak positional displacement of the wave |

---

### Summary Table

| Feature | Approach |
|---|---|
| Placement | GPU-driven: compute shader reads density map, emits instance transforms into a GPU instance buffer |
| LOD selection | GPU-driven compute pass: estimates projected solid angle per instance, writes LOD level + BLAS index; no CPU readback |
| LOD transition | Stochastic dither - per-ray hash selects one LOD tier; denoiser resolves; no simultaneous dual-LOD alpha blend |
| Near/mid foliage (LOD 0-1) | SpeedTree HighPoly/LowPoly FBX + alpha cards + Opacity Micromaps (OMM, DXR 1.2) |
| Far cluster foliage (LOD 2) | Simplified alpha cards + Opacity Micromaps |
| Far impostor (LOD 3) | Octahedral impostor (16x16 view grid) with packed depth; procedural AABB in TLAS; parallax-corrected intersection |
| Alpha handling | OMM (primary); AnyHit alpha test (OMM-unavailable fallback) |
| Vegetation motion | Procedural time-based circular trunk bend + constant leaf flutter; BLAS refit + TLAS rebuild each frame |
| Cloth/flag motion | XPBD solver + PASS 3 WAVE (traveling sine wave via per-instance `wind_direction` + `wave_amplitude`) |
| BLAS/TLAS | BLAS per species (instanced); TLAS updated each frame for dynamic instances |
| Culling | GPU-driven: frustum + max-distance cull in compute prepass before TLAS construction; no Hi-Z (no raster depth buffer) |
| Asset compatibility | SpeedTree ORCA v2 - FBX meshes, DDS textures (BaseColor/Specular/Normal), GGX PBR channel convention |

---

## 11. Crowd Rendering

| Feature | Approach |
|---|---|
| Instance count | Target: 500–2000 visible pedestrians/spectators |
| Skeleton | Per-instance bone palette in structured buffer; GPU skinning via compute |
| Animation | Pre-baked animation texture (ATB — Animation Texture Baking); blended clips |
| Variety | ~10 unique meshes × color/texture variation parameter |
| LOD | GPU-driven compute pass estimates projected solid angle per crowd instance and writes the LOD level and BLAS index into the instance args buffer; no CPU readback required |
| TLAS update | CPU boid simulation updates agent world positions each frame; positions uploaded to a GPU structured buffer via the Copy queue; a GPU compute pass then writes per-instance TLAS transforms and re-registers crowd instances before PathTracer::Trace() |
| Collision avoidance | Simple boid-style CPU simulation for nearby agents; scripted paths for distant |
| Distant impostors | Very distant crowd replaced by pre-baked multi-angle radiance cards registered as procedural TLAS geometry (no rasterization) |

---

## 12. Particle System

| Feature | Detail |
|---|---|
| Emitter types | Point, box, sphere, mesh-surface, trail |
| Simulation | GPU compute (Append/Consume buffers); up to 1M particles |
| Rendering | Ray-traced volumetric particles for smoke/fire; small solid-angle particles (sparks, rain streaks) represented as oriented procedural primitives in the TLAS and intersected by rays |
| Lighting | Particles inside the TLAS receive illumination naturally via ray hits; shadow/occlusion rays from particle hit shaders use NEE like any other surface |
| Integration | Particle positions registered in TLAS as procedural geometry (volume primitives) |

---

## 13. Weather System

| Effect | Technique |
|---|---|
| Rain | GPU particle streaks; rain drop ripples on wet surfaces via animated normal maps sampled at ray hit time (no screen-space pass) |
| Puddles | Procedural puddle normal maps; reflections handled by the path tracer — near-zero-roughness PBR on wet geometry; optionally boosted by the flipped ray-gen pass described in §17 |
| Clouds | Physically-based volumetric clouds (ray-marched density field, Schneider 2015+) |
| Fog | Heterogeneous exponential height fog; volumetric light shafts computed via in-scattering during ray-marched volume traversal (no screen-space post pass) |
| Wet surfaces | PBR material blend: dry↔wet controlled by weather intensity float |
| Snow | Particle system + snow accumulation on surfaces via compute-written coverage map |

---

## 14. Decal System (Tire Tracks / Skid Marks)

- **Projected decals** stored in a world-space decal buffer (position, orientation, extents, material)
- At path-trace hit time, the closest-hit shader queries a **CPU-built spatial hash / BVH uploaded to a structured buffer**; the shader iterates only decals whose world-space bounding boxes overlap the hit point and blends the decal albedo/normal/roughness on top of the surface material
- **Ring buffer** of N decal slots; oldest evicted when full
- Tire tracks: emitted by vehicle physics at a configurable frequency based on slip angle
- Skid marks: additional roughness reduction + darkening of albedo

---

## 15. Animation System

| Feature | Approach |
|---|---|
| Skeletal animation | CPU: evaluate animation clip → bone palette; GPU: vertex skinning compute |
| Blend trees | Simple 1D/2D blend trees; cross-fade between clips |
| Procedural animation | IK solver (FABRIK) for foot placement |
| Rigid animation | Node transform animation (flags, doors, wheels) via float-curve evaluation |
| Waving flags | XPBD cloth solver (INTEGRATE → CONSTRAIN×N → FINALIZE) extended with PASS 3 WAVE: traveling sine-wave offset injected into `pos_curr` each substep via per-instance `wind_direction` + `wave_amplitude`; updated positions written back before BLAS refit |
| BLAS update | Dynamic meshes: BLAS refit each frame (no full rebuild for small deformations) |

---

## 16. Lighting & Shadow Strategy

| Light Type | Technique |
|---|---|
| Sun / directional | Analytic light; ReSTIR DI shadow ray |
| Point / spot lights | ReSTIR DI candidate reservoir |
| Area lights | Direct area-light sampling via NEE (surface-area sampling) + MIS weighting with BRDF lobe; handled naturally by the path tracer — no LTC approximation needed |
| Emissive surfaces | Sampled as lights via power-proportional selection |
| Sky / IBL | Importance-sampled HDRI or physical sky |
| Global illumination | BRDF-importance-sampled MC path tracing (`gi_bounce_count`=1 default, 2 for richer inter-reflection; reservoir reuse deferred) |
| Shadow bias | Ray offset along geometric normal (no shadow maps) |

All shadows are ray-traced — no shadow maps, no cascades, no baked lightmaps.

---

## 17. Reflection System

- All reflections are handled naturally by the path tracer (specular rays follow BRDF sampling)
- Mirror-like surfaces (car body, wet road): near-zero roughness → mirror reflection rays
- Screen-Space Reflections (SSR): **not used** — path tracer handles this correctly
- Planar water reflections: optionally render a dedicated flipped ray-gen pass to concentrate sample budget on the reflection surface and reduce noise (more expensive than natural path tracing; used for quality, not performance)
- Reflection denoising: DLSS 4 Ray Reconstruction separates reflection signal for targeted denoising

---

## 18. Debug & Profiling Overlay *(Developer-Only)*

> ⚠️ **This overlay is strictly for developer/debug builds.** It is not the game UI.
> It is never shown to end users and is stripped from shipping builds.

- **ImGui** rendered in a rasterization pass composited after path tracing — *developer tool only*
- Panels: frame time, GPU timers per pass, ray count, TLAS stats, weather params, camera info
- **PIX** integration: `WinPixEventRuntime` GPU event markers on every major pass
- **NVIDIA Aftermath**: GPU crash dump with shader variable inspection
- **RenderDoc** compatibility: engine exposes a `--renderdoc` flag that loads `renderdoc.dll`
- Compiled in only when `MARS_ENABLE_DEV_UI` CMake option is ON (default ON for Debug, OFF for Release/Shipping)

---

## 19. Game UI System

> The game UI is a **first-class, production-quality subsystem** entirely separate from ImGui.
> It is responsible for all player-facing heads-up display, menus, overlays, and in-game widgets.

### Design Goals
- Resolution-independent vector rendering — looks identical at 1080p, 4K, and ultrawide
- **HDR-aware (mandatory):** all UI colors authored in linear color space (scRGB); final composite respects the active display color space — written to the HDR swap chain on HDR displays, tone-mapped to SDR on non-HDR displays
- Fully GPU-rendered: zero GDI/GDI+ calls at runtime
- Supports per-monitor DPI scaling and multi-monitor layouts
- Skinnable / themeable via data-driven style sheets (JSON)
- Scriptable layout — UI screens described in `.marsui` JSON files, hot-reloadable

### Font Rendering
| Feature | Approach |
|---|---|
| Font representation | **Multi-channel Signed Distance Field (MSDF)** — generated offline via `msdfgen` |
| Glyph atlas | Packed BC7 texture atlas (RGB channels carry the three MSDF distance fields); one atlas per font family + weight |
| Rendering | Custom HLSL pixel shader: MSDF alpha reconstruction → crisp edges at any scale |
| Sub-pixel rendering | Perceptual fringe correction for LCD clarity — **SDR LCD displays only**; disabled on HDR and OLED panels where sub-pixel layout differs and the correction causes color fringing |
| Unicode support | Full Unicode glyph shaping via HarfBuzz (OpenType feature support) |
| Fallback glyphs | Cascading font stack (similar to CSS) for CJK, Arabic, RTL scripts |
| Emoji | Separate RGBA atlas for color emoji glyphs (COLR/CPAL or CBDT/CBLC source data rendered to RGBA at atlas-build time) |

MSDF is the current state of the art for real-time GPU font rendering — it produces
perfectly sharp glyphs at any scale and rotation from a single low-resolution atlas,
with no blurriness at large sizes and no aliasing at small sizes.

### UI Rendering Pipeline
```
// Once per frame (display-agnostic):
UISystem::BuildDrawLists()         ← traverse widget tree, emit draw commands (CPU only)
  → UIRenderer::UploadGeometry()  ← vertices/indices → GPU upload buffer

// Once per DisplayOutput (inside Renderer::RenderFrame, after PathTracer::Trace):
UIRenderer::DispatchMinimap()      ← low-res secondary ray-gen dispatch → Minimap UAV texture
UIRenderer::Render(displayOutput)  ← rasterization pass (no ray tracing needed for other UI)
    ├── Textured quads          (images, icons, backgrounds)
    ├── MSDF text glyphs        (font rendering shader)
    ├── Vector shapes           (rounded rectangles, circles via SDF in shader)
    └── Animated elements       (shader-driven, driven by UI timeline)
  → composite on top of this display's path-traced frame (alpha blend)
```

### Widget System
| Widget | Description |
|---|---|
| `Label` | MSDF text, multi-line, alignment, kerning, line spacing |
| `Image` | Textured quad with optional rounded corners (shader SDF) |
| `Button` | Label + Image + hover/press state animations |
| `ProgressBar` | Speed/lap/fuel/health indicators |
| `Gauge` | Circular or arc gauge (tachometer, speedometer) |
| `Minimap` | Top-down view rendered via a dedicated low-resolution secondary ray-gen dispatch into a UAV texture; simplified scene representation (static TLAS only, 1 bounce, no denoiser) |
| `Leaderboard` | Scrollable ranked list with animated position changes |
| `Notification` | Slide-in/slide-out toast overlay |
| `Screen` | Full-screen UI layer (main menu, pause menu, results screen) |

### HUD Layout (Racing Game Example)
- Speedometer gauge (bottom-left) — shader-driven arc needle animation
- Gear indicator — large MSDF digit, color-coded by optimal shift point
- Lap timer / split times — monospace MSDF, sub-frame precision
- Minimap (bottom-right) — live top-down render
- Position indicator (top-center)
- Weather / condition icons
- Damage / fuel indicators
- Animated "Wrong Way" / "New Lap" / "Penalty" notifications

### Asset Files
```
engine/ui/
├── fonts/
│   ├── racing_display.msdf.png     ← MSDF atlas
│   ├── racing_display.msdf.json    ← glyph metrics
│   └── ui_body.msdf.png / .json
├── screens/
│   ├── hud.marsui                  ← HUD layout definition
│   ├── main_menu.marsui
│   └── results.marsui
├── themes/
│   └── default.json                ← colors, spacing, animation curves
└── textures/
    └── icons/                      ← weather, damage, flags etc.
```

### Third-Party Libraries for Game UI
| Library | Purpose |
|---|---|
| **msdfgen** | Offline MSDF atlas generation from TTF/OTF fonts (build tool, not runtime) |
| **HarfBuzz** | Unicode text shaping, OpenType features, RTL/BiDi support |
| **FreeType** | Font metrics and glyph outlines (used by msdfgen + HarfBuzz at build time) |
| **nlohmann/json** | `.marsui` screen file and theme file parsing (already in dep list) |

---

## 20. Displacement Mapping

### Overview
Displacement mapping adds sub-millimeter surface detail (tire treads, track kerbs, cobblestones, terrain micro-detail) without authoring high-poly meshes. In a pure path-tracing engine, displaced geometry must exist as real triangles in the BLAS — there is no rasterization tessellation stage to exploit.

### Strategy

| Tier | Hardware | Approach | Runtime Cost |
|---|---|---|---|
| **Baseline** | All hardware (AMD RDNA 3/4 + NVIDIA RTX) | Offline bake: displace mesh vertices at asset import time using the height map, write a new pre-displaced mesh into the BLAS | Zero — treated as normal static geometry |
| **Enhancement** | NVIDIA RTX 30/40/50 only | NVIDIA Displacement Micro-Maps (DMM) via `NV_displacement_micromap` DXR extension | Near-zero runtime; hardware-accelerated micro-triangle traversal |

The **offline bake** is the cross-platform foundation and is sufficient for all static displaced geometry (track surfaces, terrain, kerbs, walls). The NVIDIA DMM path is an optional enhancement for maximum quality and performance on RTX hardware.

### Offline Bake Pipeline (Baseline — All Hardware)

```
.marsscene model entry
  → AssetImporter::import_mesh()          ← Assimp loads base mesh + height map path
  → DisplacementBaker::bake()             ← CPU: subdivide + displace vertices along normals
      ├── Subdivide mesh to target density (configurable tessellation factor per material)
      ├── Sample height map (bicubic) at each new vertex UV
      ├── Displace vertex position along interpolated normal by height × displacement_scale
      └── Recompute normals + tangents (MikkTSpace) on displaced mesh
  → GpuMeshBuffer (displaced mesh)        ← uploaded and registered in bindless heap as normal
  → BLAS built from displaced GpuMeshBuffer
```

No runtime cost after import. Displaced meshes are cached to disk (`.displaced.bin` sidecar files) so baking only re-runs when the source mesh or height map changes (content hash check).

### NVIDIA DMM Enhancement Path (RTX 30/40/50 Only)

- Detected at runtime via `D3D12_FEATURE_DATA_D3D12_OPTIONS` + NVIDIA extension query
- If available: `DisplacementBaker` emits an OMM/DMM micro-mesh structure instead of a fully-expanded triangle mesh; handed to the BLAS builder as `D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE` with the NV micro-map extension
- Falls back transparently to the baseline baked mesh if DMM is not available

### Material System Integration

Height maps are a new optional entry in the PBR material descriptor:

```json
{
  "material": {
    "base_color":    "textures/track_albedo.dds",
    "normal_map":    "textures/track_normal.dds",
    "roughness_map": "textures/track_roughness.dds",
    "height_map":    "textures/track_height.dds",
    "displacement_scale": 0.05,
    "tessellation_factor": 8
  }
}
```

| Field | Type | Description |
|---|---|---|
| `height_map` | string (path) | Greyscale R8 or R16 texture; mid-grey (0.5) = no displacement |
| `displacement_scale` | float (metres) | Max displacement distance along surface normal |
| `tessellation_factor` | int | Subdivision multiplier applied before baking (1 = no subdivision, 8 = 8× linear) |

### Scope
- Targets **static geometry only** (track surfaces, terrain, buildings, kerbs)
- Dynamic/skinned meshes are excluded — BLAS refit on a heavily subdivided displaced mesh would be prohibitively expensive
- Procedurally bent vegetation uses per-vertex bend (§10), not displacement

### Key Tasks
- [ ] `DisplacementBaker` class: CPU subdivide + normal-displaced vertex generation + MikkTSpace normal/tangent recompute
- [ ] Height map cache: content-hash `.displaced.bin` sidecar; skip bake on cache hit
- [ ] `AssetImporter` integration: detect `height_map` field in material, invoke `DisplacementBaker` before GPU upload
- [ ] Material descriptor extended: `height_map`, `displacement_scale`, `tessellation_factor` fields
- [ ] `.marsscene` schema updated to support the new material fields
- [ ] NVIDIA DMM detection + enhancement path *(deferred until baseline is validated)*

---

## 21. Test Application (`mars_test_app`)

### Purpose
A minimal Win32 host application to exercise the engine during development.

### Features
- **Win32 window creation** with raw input support (mouse, keyboard, gamepad via XInput)
- **Scene loading**: reads a `.marsscene` file path from command line (or a hardcoded default)
- **Free-fly camera**: WASD + mouse look; scroll wheel adjusts speed; `F` toggles between
  free-fly and a locked orbit camera
- **Developer debug panel** (ImGui, dev builds only): toggle with `~`; shows FPS, GPU timers, weather controls, LOD bias
- **Multi-monitor support**: reads `display.json` from the working directory to configure outputs
- **Hot reload**: presses `R` to reload the current scene without restarting
- **Screenshot**: `F12` saves an EXR screenshot (HDR) to the working directory

### Startup Sequence
```
WinMain
  → parse command line (scene file path, display config path)
  → MarsEngine::Initialize(config)
  → MarsEngine::LoadScene(scenePath)   ← public API; engine delegates to SceneManager internally
  → enter message loop
      → handle WM_SIZE (resize swap chains)
      → handle WM_INPUT (camera update)
      → MarsEngine::RenderFrame()
  → MarsEngine::Shutdown()
```

---
