// #profile vs_6_6
// =============================================================================
// tone_map_blit_vs.hlsl
// MARS 3D Engine — full-screen triangle vertex shader for the tone-map blit.
//
// Generates a full-screen triangle from vertex ID (no vertex buffer needed).
// =============================================================================

struct VsOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VsOut main(uint vid : SV_VertexID)
{
    // Three vertices that cover the full clip-space screen:
    //   vid=0 → ( -1,  1)  top-left
    //   vid=1 → (  3,  1)  far right
    //   vid=2 → ( -1, -3)  far below
    float2 pos = float2((vid == 1) ? 3.0f : -1.0f,
                        (vid == 2) ? -3.0f :  1.0f);

    VsOut o;
    o.position = float4(pos, 0.0f, 1.0f);
    // UV: (0,0) top-left .. (1,1) bottom-right
    o.uv = pos * float2(0.5f, -0.5f) + 0.5f;
    return o;
}
