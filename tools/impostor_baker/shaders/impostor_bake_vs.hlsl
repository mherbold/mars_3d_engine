// #profile vs_6_6
// impostor_bake_vs.hlsl — Vertex shader for octahedral impostor atlas baking.
#pragma pack_matrix(row_major)
//
// Root signature:
//   b0 : 16 root constants = one column-major float4x4 view_proj matrix
//   t0 : base-colour SRV (Texture2D<float4>)
//   s0 : linear-clamp static sampler
//
// Vertex layout (matches engine Vertex struct, stride 76 bytes):
//   float3 position  @ offset  0
//   float3 normal    @ offset 12
//   float3 tangent   @ offset 24  (unused here)
//   float2 uv        @ offset 36
//   4x uint + 4x float bone data @ offset 44  (unused here)

#define RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), RootConstants(num32BitConstants=16, b0), DescriptorTable(SRV(t0, numDescriptors=1), visibility=SHADER_VISIBILITY_PIXEL), StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP, visibility=SHADER_VISIBILITY_PIXEL)"

cbuffer BakeVsCB : register(b0)
{
	float4x4 g_view_proj;
};

struct VsInput
{
	float3 position : POSITION;
	float3 normal   : NORMAL;
	float3 tangent  : TANGENT;
	float2 uv       : TEXCOORD0;
};

struct VsOutput
{
	float4 sv_pos    : SV_Position;
	float2 uv        : TEXCOORD0;
	float3 normal_os : TEXCOORD1;   // object-space normal for depth/normal atlas
	float  ndc_depth : TEXCOORD2;   // clip-space depth (sv_pos.z / sv_pos.w) for depth atlas
};

[RootSignature(RS)]
VsOutput main(VsInput input)
{
	VsOutput o;
	float4 clip_pos = mul(float4(input.position, 1.0f), g_view_proj);
	o.sv_pos    = clip_pos;
	o.uv        = input.uv;
	o.normal_os = normalize(input.normal);
	o.ndc_depth = clip_pos.z / clip_pos.w;  // [0,1] in D3D left-handed projection
	return o;
}
