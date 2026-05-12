// #profile lib_6_8
// =============================================================================
// restir_di.hlsl
// MARS GI — ReSTIR Direct Illumination
//
// Spatiotemporal Reservoir Resampling for direct lighting.
// Reference: "Spatiotemporal reservoir resampling for real-time ray tracing
// with dynamic direct lighting" — Bitterli et al., SIGGRAPH 2020.
//
// The core data structures and RIS functions are in restir_di.hlsli which is
// included by path_trace.hlsl for inline per-pixel evaluation.
// This file is a placeholder for a future multi-pass compute implementation
// (temporal/spatial reuse as separate compute dispatches, M8+).
// =============================================================================

#include "restir_di.hlsli"

// TODO (M8+): Separate temporal reuse compute pass
// TODO (M8+): Separate spatial reuse compute pass
