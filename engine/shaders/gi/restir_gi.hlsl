// #profile lib_6_3
// =============================================================================
// restir_gi.hlsl
// MARS GI — ReSTIR Global Illumination
//
// This file is a placeholder for future standalone compute-pass ReSTIR GI
// dispatches (separate temporal and spatial reuse passes).
//
// The initial secondary-ray tracing and per-pixel reservoir update for M8 are
// implemented inline inside path_trace.hlsl's ClosestHit shader so that
// TraceRay and PBR evaluation are in scope.
//
// Reference: "ReSTIR GI: Path Resampling for Real-Time Path Tracing"
// — Ouyang et al., HPG 2021.
// =============================================================================

#include "restir_gi.hlsli"

// Future compute passes:
// TODO: Temporal reuse compute pass — reads prev-frame GI reservoir layer,
//       merges with current-frame initial samples.
// TODO: Spatial reuse compute pass — gathers from neighbour reservoirs,
//       applies MIS weights to avoid bias.
