// =============================================================================
// random.hlsli
// MARS Common — random number generation (PCG hash, Halton sequence).
// =============================================================================

#ifndef MARS_RANDOM_HLSLI
#define MARS_RANDOM_HLSLI

// ---------------------------------------------------------------------------
// PCG hash — fast, high-quality 32-bit PRNG
// Reference: "PCG: A Family of Simple Fast Space-Efficient Statistically Good
// Algorithms for Random Number Generation" — O'Neill 2014
// ---------------------------------------------------------------------------
uint PcgHash(uint state)
{
    uint x = state * 747796405u + 2891336453u;
    uint word = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    return (word >> 22u) ^ word;
}

// Advance state and return a float in [0, 1)
float PcgRand(inout uint state)
{
    state = PcgHash(state);
    return asfloat(0x3F800000u | (state >> 9u)) - 1.0f;
}

// Seed from pixel + frame index
uint PcgSeed(uint2 pixel, uint frameIndex)
{
    return pixel.x * 1973u + pixel.y * 9277u + frameIndex * 26699u;
}

// ---------------------------------------------------------------------------
// Halton low-discrepancy sequence (base 2 and base 3 for camera jitter)
// ---------------------------------------------------------------------------
float Halton(uint index, uint base)
{
    float result = 0.0f;
    float f      = 1.0f;
    uint  i      = index;
    while (i > 0u)
    {
        f      = f / float(base);
        result = result + f * float(i % base);
        i      = i / base;
    }
    return result;
}

// Returns a 2D Halton sample in [0,1)^2 suitable for sub-pixel jitter
float2 HaltonJitter(uint frameIndex)
{
    return float2(Halton(frameIndex, 2), Halton(frameIndex, 3));
}

#endif // MARS_RANDOM_HLSLI
