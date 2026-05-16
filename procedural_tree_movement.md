# Procedural Ecosystem Tree Motion System

The goal is NOT to simulate real wind.

The goal is to add a very subtle procedural animation layer to tree instances so that a forest does not look perfectly static in a path-traced renderer.

The system should be designed for a renderer where many tree instances reuse the same skinned mesh or same animated asset. Because of that, the animation must avoid making every tree appear to move identically.

The motion should be extremely subtle: just enough to make the forest feel alive, but not enough for the viewer to consciously notice repeated animation.

---

# High-Level Concept

Use a two-part procedural animation system:

```text
treeMotion =
    subtle trunk/branch bending
  + subtle leaf rustle/shimmer
```

The trunk/branch motion gives the whole tree a barely perceptible living sway.

The leaf motion breaks up the static look of the canopy by adding small, constant, local variation.

Neither system should simulate wind. They are purely art-directed procedural deformation layers.

---

# Key Constraint

The renderer may reuse the same skinned mesh animation across many tree instances.

Therefore, the system should rely on:

- Per-instance random orientation
- Per-instance phase offsets when available
- World-space or object-space procedural noise
- Height-based masks
- Leaf/branch masks
- Very subtle amplitudes

The goal is to hide repetition through small variation, not through unique authored animations.

---

# Vegetation Species Scene Controls

Each vegetation species should expose only three artist-facing scene parameters:

```text
TreeBendStrength
TreeBendCircleTimeSeconds
LeafRustleStrength
```

---

# Part 1: Trunk and Branch Motion

The trunk and main branches should have a very gentle bend.

This bend should rotate slowly around the vertical height axis of the tree.

Conceptually, the tree is always bending slightly, but the bend direction slowly rotates over time.

Example:

```text
bendAngle = very small value
bendDirection =
    float2(cos(time * rotationSpeed), sin(time * rotationSpeed))
```

The bend direction should complete a full rotation approximately every 10 to 20 seconds.

```text
rotationPeriod = 10 to 20 seconds
rotationSpeed = 2π / rotationPeriod
```

This creates a subtle circular sway instead of a back-and-forth mechanical sway.

---

# Why Rotating Bend Works Well

If every tree uses the same animation, a normal left-right sway becomes obvious.

But if each tree instance has a random world rotation, then the same local bend appears to point in different world-space directions.

So the shared animation becomes less noticeable.

Conceptually:

```text
Tree A local bend maps north-east
Tree B local bend maps south-west
Tree C local bend maps east
```

Even though the same procedural animation is reused, the forest appears more varied.

---

# Bend Strength

The bend must be extremely subtle.

The trunk should not look like it is waving around.

The viewer should only notice that the forest is not frozen.

Suggested behavior:

```text
trunk base: almost no movement
mid trunk: tiny movement
upper trunk: slightly more movement
thin branches: most movement
```

Use a height mask:

```text
heightMask = smoothstep(0.0, 1.0, normalizedTreeHeight)
```

Then make it stronger near the top:

```text
bendWeight = heightMask^1.5
```

or:

```text
bendWeight = heightMask^2.0
```

This keeps the base planted while allowing the upper tree to move slightly.

---

# Trunk Bend Deformation

The trunk/branch deformation can be thought of as a height-based lateral offset.

Conceptually:

```text
offset =
    bendDirection
  * bendAmplitude
  * bendWeight
```

The offset should be perpendicular to the height axis.

For a vertex:

```text
finalPosition =
    originalPosition
  + offset
```

But preferably, this should behave like a bend rather than a pure translation.

The base stays fixed, and the top gradually curves away from the base.

A simple approximation:

```text
bendOffset =
    bendDirection
  * bendAmplitude
  * normalizedHeight^2
```

This produces a gentle curved shape.

---

# Avoid Obvious Motion

The trunk motion should be slow, smooth, and almost invisible.

Avoid:

- Fast trunk movement
- Large side-to-side swaying
- Abrupt changes
- Start/stop cycles
- Identical world-space bend directions for all trees

Prefer:

- Constant slow circular bend
- Very small amplitude
- Smooth height falloff
- Per-instance phase variation when possible
- Random tree orientation

The motion should feel like life, not choreography. The forest should not look like it is rehearsing for Broadway.

---

# Per-Instance Variation

If possible, each tree instance should have small randomized parameters:

```text
instancePhase
instanceRotationOffset
instanceAmplitudeScale
instanceSpeedScale
```

These can be generated from:

```text
instanceID
worldPosition hash
tree transform
```

Even tiny variation helps a lot.

Example conceptual values:

```text
instancePhase = random 0 to 2π
instanceAmplitudeScale = random 0.75 to 1.25
instanceSpeedScale = random 0.85 to 1.15
```

Then:

```text
bendDirection =
    float2(
        cos(time * rotationSpeed * instanceSpeedScale + instancePhase),
        sin(time * rotationSpeed * instanceSpeedScale + instancePhase)
    )
```

If true per-instance shader data is difficult, random tree orientation alone still helps.

---

# Branch Weighting

Branches can move slightly more than the trunk.

If the asset has vertex attributes, use masks such as:

```text
trunkMask
branchMask
leafMask
heightMask
```

Suggested behavior:

```text
trunk = very subtle bend
large branches = slightly stronger bend
small branches = stronger still
leaves = handled separately
```

A branch mask could multiply the bend amplitude:

```text
branchBend =
    baseBend
  * branchMask
  * heightMask
```

This gives the upper branch structure gentle life without deforming the base too much.

---

# Part 2: Leaf Rustle / Shimmer

Leaves should have subtle constant motion.

The leaf motion should:

- Never fully stop
- Never pulse globally
- Avoid obvious synchronized movement
- Vary across different parts of the tree
- Be small enough not to look windy
- Break up static silhouettes and highlights

This is especially useful in a path-traced renderer, where static leaves can make the forest feel frozen.

---

# Leaf Motion Concept

Each leaf or leaf-card vertex receives a small procedural offset.

The offset should be driven by noise or layered sine waves.

Conceptually:

```text
leafMotion =
    small directional offset
  * proceduralNoise
  * leafMask
```

The motion should mostly affect:

- Leaf cards
- Small twigs
- Canopy geometry

It should not significantly move the trunk.

---

# Constant Rustle

Do not use gusts that start and stop.

The rustle should be continuous.

Instead of:

```text
rustleStrength = intermittentGust(time)
```

Use:

```text
rustleStrength = constantSmallValue
```

Then vary the direction and phase locally.

This prevents the entire forest from visibly starting or stopping at once.

---

# Leaf Noise Field

Use a procedural noise field based on a combination of:

```text
world position
object/local position
normalized height
time
instance seed
```

Conceptually:

```text
noiseValue =
    noise(
        vertexWorldPosition * spatialFrequency
      + time * temporalFrequency
      + instancePhase
    )
```

This causes different parts of the tree to animate differently.

Do not use a single uniform animation value for the entire tree.

---

# Local Direction Variation

Different parts of the tree should rustle in different directions.

One approach:

```text
localRustleDirection =
    normalized random/noise vector
```

derived from:

```text
vertex position
leaf cluster position
branch id
instance seed
```

Then:

```text
leafOffset =
    localRustleDirection
  * noiseValue
  * leafAmplitude
  * leafMask
```

This produces small asynchronous motion across the canopy.

---

# Leaf Card Animation

If leaves are represented as cards or small clusters, the best result usually comes from rotating or fluttering the cards slightly rather than translating them too much.

Conceptually:

```text
leafCardAngle =
    noiseValue
  * leafAngularAmplitude
```

Then rotate the leaf vertices around the card center or branch attachment point.

If actual card pivots are unavailable, approximate it by moving vertices along their normals or tangent directions.

Suggested components:

```text
normal flutter
tangent shimmer
tiny vertical jitter
```

Example conceptual composition:

```text
leafOffset =
    normalDirection  * normalNoise  * 0.5
  + tangentDirection * tangentNoise * 0.35
  + verticalDirection * verticalNoise * 0.15
```

Keep all of these extremely small.

---

# Avoid Uniform Leaf Shimmer

Bad:

```text
allLeavesMove = sin(time)
```

This makes the whole forest shimmer in sync.

Better:

```text
each leaf region has different phase, direction, and noise value
```

For example:

```text
phase =
    dot(localPosition, randomVector)
  + time * rustleSpeed
  + instancePhase
```

Then:

```text
rustle =
    sin(phase)
```

Layer multiple versions:

```text
rustle =
    sin(phaseA) * 0.6
  + sin(phaseB) * 0.3
  + noiseValue * 0.1
```

This keeps the motion alive but non-repetitive.

---

# Leaf Cluster Variation

If possible, drive leaf movement per cluster rather than per vertex.

A “cluster” could be:

- A leaf card
- A small twig group
- A branchlet
- A meshlet
- A material/geometry region

Each cluster gets:

```text
clusterPhase
clusterDirection
clusterAmplitude
clusterFrequency
```

This avoids noisy crawling deformation while still breaking up the canopy.

If explicit clusters are unavailable, approximate clusters from position:

```text
clusterCoord = floor(localPosition * clusterFrequency)
clusterSeed = hash(clusterCoord)
```

Then use the cluster seed to vary direction and phase.

---

# Suggested Leaf Motion Frequencies

Use low to medium frequency motion.

For subtle forest life:

```text
leafRustleSpeed = 1 to 4 Hz equivalent visual motion
```

But avoid visibly rhythmic motion.

Instead of one frequency, layer several:

```text
slow shimmer = 0.7 Hz
medium rustle = 1.6 Hz
fine shimmer = 3.2 Hz
```

The actual visible movement should remain tiny.

---

# Suggested Amplitudes

These values should be art-tuned, but conceptually:

```text
trunk bend: extremely small
branch bend: small
leaf offset: very small
leaf angular flutter: small but visible in highlights
```

In world units, the leaf displacement should often be only a tiny fraction of a leaf size.

The goal is not to make the tree look windy.

The goal is to prevent a dead-still render.

---

# Masking

Use masks to control where motion applies.

Recommended masks:

```text
heightMask
branchMask
leafMask
trunkMask
stiffnessMask
```

Behavior:

```text
trunk base = almost locked
upper trunk = tiny bend
branches = subtle bend
leaves = rustle/shimmer
```

If the tree asset supports vertex colors, use them for motion masks.

Example:

```text
red   = stiffness
green = leaf mask
blue  = branch mask
alpha = phase/randomness
```

If no custom attributes exist, derive masks from:

```text
height
material ID
normal direction
vertex color
UV region
mesh section
```

---

# Handling Shared Animation Across Many Instances

Because many trees may share the same mesh and animation, avoid animation that is too recognizable.

Use these techniques:

## 1. Random Instance Rotation

Each tree has a random yaw orientation.

This makes the same local bend appear different in world space.

## 2. Instance Phase Offset

If available, add a random phase per instance.

```text
timeForTree =
    globalTime * instanceSpeedScale
  + instancePhase
```

## 3. World-Space Noise

For leaf shimmer, include world position in the noise input.

This makes trees at different positions sample different parts of the noise field.

```text
noiseInput =
    worldPosition * frequency
  + time
```

## 4. Amplitude Variation

Small per-instance amplitude variation avoids identical silhouettes.

```text
amplitudeScale = random 0.8 to 1.2
```

## 5. Speed Variation

Small per-instance speed variation helps avoid synchronized cycles.

```text
speedScale = random 0.9 to 1.1
```

---

# Important Note About Global Time

Avoid using global time alone.

Bad:

```text
motion = sin(globalTime)
```

Better:

```text
motion =
    sin(globalTime * speed + instancePhase + localPositionPhase)
```

Even better:

```text
motion =
    noise(worldPosition * spatialFrequency + globalTime * temporalFrequency)
```

Global time should only advance the animation. It should not be the only thing determining it.

---

# Renderer / Path Tracing Considerations

In a path-traced renderer, animated geometry can affect:

- BVH updates
- Motion blur
- Temporal accumulation
- Denoising stability
- ReSTIR / reservoir reuse
- Instance transform reuse
- Skinned mesh reuse

Because of this, the motion should be very subtle.

The system should not cause large silhouette changes frame-to-frame.

Leaf shimmer should be strong enough to break static highlights, but weak enough to avoid excessive temporal noise.

The trunk bend should be slow enough that acceleration is minimal.

---

# Motion Blur Consideration

If motion blur is supported, leaf shimmer can become noisy or smeared.

For this system, it may be useful to treat the procedural vegetation motion as:

```text
small deformation motion
```

rather than strong physical motion.

Motion vectors should be generated if temporal denoising or TAA-style accumulation uses them.

If motion vectors are unavailable, keep the leaf displacement especially small.

---

# Final Recommended System

Use two procedural layers:

```text
1. Slow rotating tree bend
2. Constant local leaf shimmer
```

## Slow Rotating Tree Bend

```text
bendDirection =
    float2(
        cos(time * slowSpeed + instancePhase),
        sin(time * slowSpeed + instancePhase)
    )

bendAmount =
    treeMotionAmplitude
  * heightMask^2
  * branch/trunk mask
```

This creates a gentle circular sway.

The bend should complete one full rotation approximately every 10 to 20 seconds.

## Constant Leaf Shimmer

```text
leafNoise =
    noise(
        worldPosition * spatialFrequency
      + time * temporalFrequency
      + instancePhase
    )

leafOffset =
    localRustleDirection
  * leafNoise
  * leafAmplitude
  * leafMask
```

This creates continuous asynchronous canopy motion.

---

# Overall Design Philosophy

Do NOT simulate wind.

Do NOT create obvious swaying.

Do NOT start and stop gusts.

Instead:

- Add subtle continuous life
- Keep trunk motion almost imperceptible
- Keep leaf motion constant but locally varied
- Use world-space noise and instance variation to hide repetition
- Let random tree orientation make shared animation appear different across the forest

The viewer should not consciously notice the animation.

They should only notice that the forest does not feel frozen.