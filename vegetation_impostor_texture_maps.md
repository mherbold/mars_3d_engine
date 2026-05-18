# vegetation_impostor_texture_maps.md

# Vegetation Impostor Texture Maps for a Pure Path Tracing D3D12 Engine

## Overview

In a modern pure path tracing renderer, vegetation impostors should not be treated as simple billboard textures.

Instead, they should function as compact ray-traceable approximations of complex vegetation geometry.

A vegetation impostor represents:

- Approximate volumetric geometry
- Surface shading information
- Alpha coverage
- Material classification
- Depth reconstruction data
- Optional translucency and multi-layer depth

The goal is to reduce geometric complexity while preserving:

- Silhouette quality
- Lighting fidelity
- Reflection correctness
- Shadow accuracy
- Temporal stability

---

# Recommended LOD Stack

```text
LOD0 = Full geometry
LOD1 = Simplified geometry
LOD2 = Clustered foliage cards
LOD3 = Octahedral depth impostor
LOD4 = Distant canopy billboard/proxy
```

Impostors should only appear once projected screen size becomes sufficiently small.

Before switching to impostors, geometry should already be heavily simplified.

---

# Octahedral Impostor Atlases

Modern impostors should use octahedral directional atlases.

Instead of a single billboard:

```text
1 image
```

multiple view directions are baked:

```text
8x8   = 64 views
12x12 = 144 views
16x16 = 256 views
```

Each view stores multiple texture maps.

At runtime:

```text
View direction -> octahedral atlas coordinate
```

The renderer selects and blends or stochastically chooses nearby views.

---

# Core Texture Maps

# 1. BaseColorOpacity

## Purpose

Stores:

- Albedo
- Alpha coverage

## Suggested Format

```text
RGBA8_UNORM
BC7
```

## Layout

```text
RGB = Base color / albedo
A   = Opacity coverage
```

## Usage

Used for:

- Primary visibility
- Alpha rejection
- Diffuse shading
- Silhouette reconstruction

## Important Notes

Opacity should be treated as stochastic coverage rather than traditional alpha blending.

Recommended approach:

```cpp
if (Random(ray, sample) > opacity)
    IgnoreHit();
```

Avoid standard transparency blending in path tracing.

---

# 2. NormalRoughness

## Purpose

Stores:

- Surface normal
- Roughness

## Suggested Format

```text
BC7
BC5 + R8
RGBA8
```

## Layout

```text
RGB = Encoded normal
A   = Roughness
```

## Usage

Used for:

- Lighting
- Reflections
- BRDF evaluation
- Denoiser normal buffers

## Important Notes

Normals should ideally be stored in:

```text
Object space
or
Local impostor space
```

Avoid baking normals directly in world space.

Without normals, impostors will appear visually flat.

---

# 3. DepthMaterialAOThickness

## Purpose

Stores:

- Depth reconstruction
- Material classification
- Ambient occlusion
- Thickness/translucency data

## Suggested Format

```text
RGBA8
RG16
R16G16B16A16_FLOAT
```

## Layout

### Minimal Layout

```text
R = Depth
G = Material ID
B = Ambient occlusion
A = Thickness/transmission
```

### Higher Precision Layout

```text
R16 = Depth
G16 = Material/attribute
```

## Usage

Depth is critical for:

- Parallax correction
- Reflection correctness
- GI hit reconstruction
- Shadow accuracy
- Reducing flat-card appearance

Material ID is used to separate:

```text
0 = Empty
1 = Bark
2 = Leaf front
3 = Leaf back
4 = Twig
```

Thickness/transmission is used for:

- Leaf translucency
- Backlighting
- Shadow attenuation

---

# Optional Advanced Texture Maps

# 4. MultiDepth

## Purpose

Improves volumetric reconstruction.

Single-depth impostors behave like heightfields and struggle with:

- Tree holes
- Overlapping branches
- Interior canopy structure

Multi-depth maps improve reconstruction quality.

## Suggested Layouts

### Two-layer depth

```text
R = Front depth
G = Back depth
```

### Four-layer depth

```text
RGBA = 4 sorted depth layers
```

## Suggested Formats

```text
RG16_FLOAT
RGBA16_FLOAT
```

## Usage

Used for:

- Better silhouettes
- Interior foliage reconstruction
- Reduced popping
- More stable reflections
- Better secondary rays

---

# 5. TransmittanceThickness

## Purpose

Stores translucency behavior.

Important for realistic vegetation rendering.

## Suggested Format

```text
RG8
RGBA8
```

## Layout

```text
R = Transmission amount
G = Thickness
```

## Usage

Used for:

- Sunlight transmission
- Leaf glow
- Colored shadowing
- Approximate subsurface scattering

Example:

```cpp
shadowThroughput *= lerp(1.0, leafTint, transmission);
```

Without transmission data, forests often appear unnaturally dark.

---

# Recommended Shipping Configurations

# Minimal High-Quality Setup (Recommended)

## Texture Set

### Texture 0

```text
BaseColorOpacity
```

### Texture 1

```text
NormalRoughness
```

### Texture 2

```text
DepthMaterialAOThickness
```

## Total

```text
3 texture maps
```

This is the recommended balance between:

- Quality
- Memory usage
- Runtime complexity
- Authoring cost

---

# Deluxe High-Fidelity Setup

## Texture Set

### Texture 0

```text
BaseColorOpacity
```

### Texture 1

```text
NormalRoughness
```

### Texture 2

```text
DepthFrontBack
```

### Texture 3

```text
MaterialAttributes
```

### Texture 4

```text
TransmittanceThickness
```

## Total

```text
5 texture maps
```

This configuration improves:

- Reflections
- GI quality
- Shadow realism
- Temporal stability

---

# Runtime Representation

# BLAS Representation

Impostors should not simply be flat quads.

Preferred representations:

```text
Small box/slab
```

or

```text
Convex impostor volume
```

This allows depth reconstruction inside the impostor volume.

---

# Ray Tracing Workflow

## Ray Hit

```text
Ray intersects impostor proxy geometry
```

## Runtime Steps

### 1. Compute local impostor UV

```text
Project hit into impostor atlas space
```

### 2. Select octahedral view frame

```text
Based on ray direction
```

### 3. Sample opacity

```cpp
if stochasticAlphaReject()
    IgnoreHit();
```

### 4. Sample depth

```text
Reconstruct approximate local hit position
```

### 5. Sample shading attributes

```text
Base color
Normal
Roughness
Material ID
Transmission
```

### 6. Evaluate shading

Proceed as approximate surface shading.

---

# Ray-Type Specific Usage

# Primary Rays

Use:

- Full normals
- Full depth
- Full material data

Highest quality path.

---

# Shadow Rays

Use:

- Opacity
- Transmission
- Optional simplified depth

May aggressively simplify.

---

# Diffuse GI Rays

Use:

- Base color
- Opacity
- Simplified normals
- Transmission

Can use lower quality impostors.

---

# Reflection Rays

Use:

- Normals
- Roughness
- Depth

Quality depends on reflection roughness.

---

# LOD Transitions

# Recommended Technique

Use stochastic dither transitions.

Avoid:

```text
Alpha fade out mesh
Alpha fade in impostor
```

Instead:

```cpp
float lodT = ComputeTransitionFactor();

float noise = BlueNoise(...);

if (noise < lodT)
    useMesh();
else
    useImpostor();
```

This works well with:

- TAA
- Temporal accumulation
- Path tracing convergence
- Denoisers

This is the modern successor to classic stencil/Z dithering techniques.

---

# Opacity Micromaps

For near and medium foliage geometry, DXR Opacity Micromaps are strongly recommended.

Benefits:

- Reduced AnyHit cost
- Faster alpha-tested traversal
- Better RT performance

Suggested usage:

```text
Near foliage:
    Real geometry + OMM

Far foliage:
    Impostors
```

Do not attempt to use OMMs as the impostor system itself.

---

# Final Recommendations

## Strongly Recommended

```text
3 texture maps:
    BaseColorOpacity
    NormalRoughness
    DepthMaterialAOThickness
```

## Most Important Data

```text
Opacity
Depth
Normals
Base color
Material classification
```

## Critical Insight

Without depth:

```text
The impostor behaves like a billboard.
```

Without normals:

```text
The impostor behaves like a flat sticker.
```

Without stochastic opacity:

```text
The impostor behaves like a rectangle.
```

The goal of a modern impostor system is not merely to fake geometry.

The goal is to provide a compact, ray-traceable approximation of vegetation volume suitable for physically-based path tracing.