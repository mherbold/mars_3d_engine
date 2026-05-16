# Procedural GPU Flag Animation System (XPBD Cloth)

The cloth simulation system already exists and uses XPBD. The goal is NOT to physically simulate wind. Instead, the goal is to procedurally inject believable flag motion directly into the cloth as external forces/accelerations.

The result should feel like a real flag fluttering and waving in the wind while still preserving true cloth behavior from the XPBD solver.

The procedural system should operate entirely as a force-generation layer on top of the cloth simulation.

---

# High-Level Concept

The flag should behave like:

- A pinned cloth surface
- Receiving coherent traveling wave energy
- With turbulence near the trailing edge
- While being generally pushed in a user-defined direction

The cloth solver handles:

- Stretch constraints
- Bend constraints
- Inertia
- Damping
- Collisions
- General cloth behavior

The procedural layer only injects motion energy.

The procedural system should NEVER directly override cloth positions.

Instead, it should inject:

- External accelerations
- Velocity impulses
- Pressure-like directional forces

---

# User Controls

Each flag exposes only two artist-facing controls:

```text
FlagWindDirection
FlagWaveAmplitude
```

These internally drive all other procedural behavior.

---

# Flag Coordinate System

For each cloth:

```text
u = horizontal coordinate from pole/root edge to trailing edge
v = vertical coordinate
```

Normalize:

```text
u = 0.0 at pinned edge
u = 1.0 at free edge
```

The pinned edge should remain relatively stable while the free edge becomes increasingly dynamic.

Define:

```text
edgeWeight = u^2
```

(or similar smooth weighting)

This ensures motion increases toward the trailing edge.

---

# Main Procedural Wave System

The primary waving motion should come from traveling sinusoidal waves that move from the pinned edge toward the free edge.

Conceptually:

```text
phase = u * spatialFrequency - time * waveSpeed
wave = sin(phase)
```

The wave force should primarily push along the cloth normal direction.

Conceptually:

```text
force = clothNormal * wave * amplitude * edgeWeight
```

This produces the large rolling flag motion.

---

# Multiple Wave Layers

Do NOT use a single sine wave.

Instead, layer several waves:

- Large slow wave
- Medium secondary folds
- Small high-frequency flutter

Example conceptual layering:

```text
wave =
    sin(u * 2.0  - time * 1.5) * 1.0
  + sin(u * 5.0  - time * 3.2 + v * 1.5) * 0.35
  + sin(u * 11.0 - time * 7.0 + v * 4.0) * 0.12
```

Each layer has different:

- Frequency
- Speed
- Phase
- Vertical offset
- Strength

This prevents the flag from looking mechanically uniform.

---

# Vertical Phase Variation

A real flag does not move identically across its entire height.

Introduce vertical phase variation:

```text
phase =
    u * frequency
  - time * speed
  + v * verticalPhaseOffset
```

This causes different vertical regions of the flag to move slightly out of sync.

Without this, the flag looks rigid and artificial.

---

# Downwind Push

A real flag also stretches and leans in the wind direction.

Add a weaker directional push:

```text
force += windDirection * amplitude * edgeWeight * directionalStrength
```

This should be much weaker than the normal-direction waving force.

The purpose is only to bias the cloth downstream.

---

# Trailing Edge Flutter

The trailing edge should receive additional chaotic motion.

Define:

```text
trailingEdgeWeight =
    smoothstep(0.65, 1.0, u)
```

Apply high-frequency procedural turbulence:

```text
flutter =
    noise(u * 8, v * 6, time * 4)
  + sin(u * 20 - time * 12 + v * 7)
```

Then apply:

```text
force +=
    clothNormal
    * flutter
    * amplitude
    * trailingEdgeWeight
```

This creates the rapid fluttering behavior seen on real flags.

---

# Gust Modulation

The amplitude should not remain perfectly constant.

Internally modulate it over time:

```text
gust =
    0.75 + 0.25 * lowFrequencyNoise(time)

effectiveAmplitude =
    userAmplitude * gust
```

This introduces subtle natural variation.

The modulation should remain smooth and low frequency.

---

# Velocity-Based Driving (Preferred)

The procedural system should preferably behave like a velocity target rather than a positional target.

Bad approach:

```text
particle.position = desiredWavePosition
```

Good approach:

```text
velocity += force * dt
```

Better approach:

Use the procedural wave field to define a desired normal velocity.

Conceptually:

```text
desiredVelocity =
    clothNormal
    * waveValue
    * amplitude
```

Then:

```text
velocityError =
    desiredVelocity - currentVelocity
```

Apply a corrective force:

```text
force += velocityError * responseStrength
```

This preserves inertia and allows the XPBD solver to generate natural folds and secondary motion.

---

# XPBD Integration

The procedural forces should be integrated as external accelerations BEFORE XPBD constraint solving.

Typical order:

```text
1. Apply gravity
2. Apply procedural flag forces
3. Predict particle positions
4. Solve XPBD constraints
5. Update velocities from solved positions
6. Apply damping
```

The procedural system should never bypass the cloth solver.

---

# Amplitude Mapping

The single user amplitude control should internally drive many parameters.

Example conceptual mapping:

```text
waveHeight      = amplitude
waveSpeed       = lerp(0.8, 4.0, amplitude)
flutterStrength = amplitude^1.5
flutterSpeed    = lerp(2.0, 12.0, amplitude)
directionalPush = amplitude * 0.25
```

Low amplitude:

- Gentle folds
- Slow movement
- Minimal flutter

High amplitude:

- Faster traveling waves
- Stronger trailing edge turbulence
- More aggressive downstream pull
- More chaotic motion

---

# Recommended Force Composition

The final procedural force per particle should approximately be:

```text
finalForce =
    directionalPush
  + mainWaveForce
  + secondaryWaveForce
  + trailingEdgeFlutter
  + subtleVerticalTwist
```

Where:

## Directional Push

```text
windDirection * amplitude
```

Provides downstream bias.

## Main Wave Force

Large rolling motion.

## Secondary Waves

Break up uniformity.

## Trailing Edge Flutter

Adds realism and rapid edge motion.

## Vertical Twist

Adds subtle asymmetry between upper/lower portions.

---

# Stability Recommendations

Clamp maximum procedural acceleration.

Example conceptually:

```text
forceMagnitude <= maxAcceleration
```

Strong procedural forces can destabilize cloth.

Also increase damping slightly at high amplitudes.

Example:

```text
damping =
    baseDamping
  + amplitude * dampingBoost
```

---

# Overall Design Philosophy

Do NOT simulate air.

Instead:

Treat the procedural system as a moving pressure/wave field that injects energy into the cloth.

The XPBD solver then transforms that injected energy into believable cloth motion.

Conceptually:

```text
Pinned edge anchors the cloth.
Traveling waves inject coherent motion.
Trailing edge receives turbulence.
XPBD produces the final realistic folds and secondary motion.
```

The procedural system should create:

- Large rolling waves
- Secondary folds
- Free-edge flutter
- Natural asymmetry
- Directional bias

while still allowing the cloth simulation to behave naturally.