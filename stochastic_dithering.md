# stochastic_dithering.md

# Stochastic Dithering for a Pure Path Tracing D3D12 Engine

## Overview

In a modern pure path tracing renderer, stochastic dithering is not merely a raster-era visibility trick.

Instead, it becomes a Monte Carlo visibility sampling strategy.

Rather than blending between two representations:

- Full geometry
- Simplified geometry
- Impostors
- Transparency states

the renderer probabilistically selects one representation per sample or ray.

This approach integrates naturally into path tracing because path tracing itself is already based on stochastic integration.

Benefits include:

- Energy conservation
- Correct visibility behavior
- Temporal stability
- Proper shadowing
- Better denoiser compatibility
- Smooth LOD transitions
- Reduced popping artifacts

---

# Fundamental Concept

## Incorrect Traditional Approach

Classic alpha fading:

```text
finalColor =
    lerp(meshColor, impostorColor, t)
```

creates several major problems in a path tracer:

- Double energy contribution
- Conflicting depth values
- Incorrect shadowing
- Transparency artifacts
- Reflection instability
- GI corruption

This occurs because both representations exist simultaneously.

---

# Correct Modern Approach

Instead of blending results, probabilistically select one representation:

```cpp
if (random < transitionFactor)
    TraceMesh();
else
    TraceImpostor();
```

Only one representation exists per sample.

Over time, temporal accumulation converges toward the expected blended result.

---

# Monte Carlo Interpretation

The renderer computes:

```text
ExpectedValue(result)
    =
blend(mesh, impostor)
```

without actually blending the geometry.

The resulting noise becomes ordinary Monte Carlo integration noise, which path tracers are specifically designed to resolve.

---

# LOD Transition Metric

## Distance-Based Transition

Simplest approach:

```cpp
float t = saturate(
    (distance - lodStart) /
    (lodEnd - lodStart)
);
```

---

# Projected Size Transition

Better approach:

```cpp
float projectedSize =
    ComputeProjectedRadius();

float t =
    smoothstep(
        sizeA,
        sizeB,
        projectedSize
    );
```

Projected size is preferable because it accounts for:

- Field of view
- Screen resolution
- Camera zoom
- Perspective

---

# Ray-Aware Transition Metrics

For path tracing, transition quality should depend on ray importance.

Example:

```cpp
float importance =
    projectedSize *
    rayImportance *
    roughnessFactor *
    rayConeFactor;
```

This allows:

- Diffuse rays to use cheaper LODs
- Rough reflections to simplify earlier
- Sharp reflections to preserve detail

---

# Stochastic Visibility Selection

## Basic Random Selection

```cpp
float h =
    Hash(pixelX, pixelY, frameIndex);

bool useImpostor = h < t;
```

This works but usually produces excessive shimmer.

---

# Blue Noise Sampling

Modern path tracers typically use blue noise instead of white noise.

Benefits:

- Better spatial distribution
- Reduced clumping
- Improved temporal convergence
- Reduced visible grain
- More stable vegetation rendering

---

# Recommended Sampling Method

```cpp
float noise =
    BlueNoise(
        pixel.xy,
        frameIndex,
        sampleIndex,
        instanceId
    );

bool useImpostor =
    noise < transitionFactor;
```

This creates:

- Spatially distributed transitions
- Temporally stable convergence
- Visually smooth fading behavior

---

# Stable vs Animated Noise

## Fully Stable Noise

```cpp
noise =
    Hash(pixel.xy, instanceId);
```

### Advantages

- No shimmer
- Stable image

### Disadvantages

- Static stipple patterns
- Poor convergence
- Visible structured noise

---

# Fully Randomized Noise

```cpp
noise =
    Hash(pixel.xy, frameIndex);
```

### Advantages

- Fast convergence

### Disadvantages

- Excessive temporal shimmer
- Sparkling artifacts

---

# Recommended Hybrid Approach

Use:

- Spatial blue noise
- Temporally scrambled sequences

Example:

```cpp
noise =
    BlueNoise(pixel.xy)
    XOR OwenScramble(frameIndex);
```

This produces:

- Good spatial quality
- Good temporal convergence
- Reduced flicker

---

# Per-Ray-Class LOD

Different ray types should use different transition policies.

This is one of the major advantages of a path tracing renderer.

---

# Primary Rays

Highest quality representation.

```cpp
t = PrimaryRayTransition();
```

---

# Shadow Rays

Can simplify aggressively.

Distant leaf detail contributes little to final shadow quality.

```cpp
t *= ShadowRayAggression;
```

---

# Diffuse GI Rays

Can transition to impostors extremely early.

Diffuse GI is naturally low-frequency.

---

# Glossy Reflection Rays

Transition quality should depend on roughness.

- Rough reflections can use impostors
- Sharp reflections require better geometry

---

# Specular Rays

Usually require:

- Highest LOD
or
- Specialized handling

Mirror-like reflections reveal LOD artifacts immediately.

---

# Ray Cones

Ray cones are highly effective for vegetation LOD decisions.

A large blurry ray footprint does not require:

- Fine leaf silhouettes
- Branch detail
- Accurate alpha coverage

Example:

```cpp
float coneRadius =
    ComputeRayConeRadius(rayDepth);

lodBias +=
    coneRadius * coneScale;
```

This is often a better metric than raw distance.

---

# Stochastic Alpha Coverage

Traditional alpha testing:

```cpp
if (alpha < 0.5)
    IgnoreHit();
```

creates:

- Hard popping
- Aliasing
- Temporal instability
- Poor subpixel coverage

---

# Correct Probabilistic Coverage

Instead:

```cpp
float h = BlueNoise(...);

if (h > alpha)
    IgnoreHit();
```

This converts alpha into probabilistic coverage.

Benefits:

- Smooth silhouettes
- Reduced aliasing
- Stable vegetation edges
- Better temporal accumulation

This is essential for high-quality path traced foliage.

---

# Combined LOD and Alpha Sampling

LOD and alpha decisions can be combined naturally.

Example:

```cpp
float lodNoise =
    BlueNoise(...);

float alphaNoise =
    BlueNoise2(...);

if (lodNoise < lodTransition)
{
    if (alphaNoise < meshAlpha)
        TraceMesh();
}
else
{
    if (alphaNoise < impostorAlpha)
        TraceImpostor();
}
```

This allows:

- Smooth vegetation transitions
- Stable silhouettes
- Reduced popping

---

# Temporal Accumulation

Stochastic transitions assume the renderer uses:

- TAA
- Path accumulation
- Temporal denoisers
or
- Similar accumulation systems

Without accumulation:

- stochastic visibility appears noisy

With accumulation:

- transitions become extremely smooth

---

# Reservoir Sampling Considerations

If using:

- ReSTIR GI
- ReSTIR DI
- Reservoir resampling

care must be taken because changing LODs can destabilize reservoirs.

Potential issues:

- Visibility discontinuities
- Normal discontinuities
- Depth discontinuities

Mitigation strategies:

- Stabilize representation selection
- Slightly blend normals/depths
- Bias reservoir reuse
- Reduce abrupt visibility changes

---

# Recommended Vegetation Transition Stack

```text
Full geometry
    ->
Simplified geometry
    ->
Cluster foliage cards
    ->
Depth impostors
    ->
Canopy proxies
```

Each stage should use stochastic transitions.

Avoid abrupt whole-tree switching.

---

# Near Range

```text
Real geometry
Opacity Micromaps
```

Highest quality.

---

# Mid Range

```text
Reduced branch complexity
Clustered foliage
Reduced alpha complexity
```

---

# Far Mid Range

```text
Octahedral depth impostors
```

---

# Extreme Distance

```text
Canopy masses
Forest proxies
Volumetric approximations
```

---

# Opacity Micromaps

For near foliage:

- Use real geometry
- Use Opacity Micromaps

For distant foliage:

- Use impostors

This creates a natural performance gradient.

---

# Why Stochastic Transitions Converge

The stochastic system converges because:

```text
ExpectedValue(LODChoice)
    =
desired blended representation
```

The transition noise becomes ordinary integration noise.

Path tracers are designed to resolve integration noise through accumulation.

---

# Critical Design Rule

Transitions must be:

- Gradual
- Spatially distributed
- Temporally stable
- Low-frequency

Avoid:

```text
Entire tree switches instantly
```

Prefer:

```text
Subpixel probabilistic transitions
```

The human visual system tolerates distributed noise far better than coherent popping.

This is especially true for forests and vegetation.

---

# Recommended Modern Stack

## Use

- Blue-noise stochastic visibility
- Per-ray-class LOD
- Ray-cone-aware transitions
- Octahedral depth impostors
- Probabilistic alpha coverage
- Temporal accumulation
- Opacity Micromaps for near foliage

---

# Avoid

- Alpha fading between LODs
- Whole-tree popping
- Deterministic alpha thresholds
- Single-frame hard transitions
- Traditional transparency blending

---

# Final Insight

Classic stencil/Z dithering techniques from older raster engines were directionally correct.

Modern path tracing elevates the idea into a physically meaningful Monte Carlo visibility sampling strategy.

The renderer no longer fakes visibility transitions.

Instead, it statistically samples them.