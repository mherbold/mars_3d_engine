// =============================================================================
// material_data.hlsli
// MARS Material — bindless material fetch helpers.
//
// Each draw/instance carries a root constant "material index" that is used
// to look up the per-material structured buffer entry and sample its textures
// from the global bindless descriptor heap.
// =============================================================================

#pragma once

// TODO: Define MaterialData struct (albedo tex index, normal tex index,
//       roughness, metallic, emissive, etc.)
// TODO: Implement FetchMaterial(uint materialIndex) -> MaterialData
