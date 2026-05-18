// #profile ps_6_6
// impostor_bake_ps.hlsl — Pixel shader for octahedral impostor atlas baking.
//
// RT0: base-colour (RGB) + binarized coverage (A).
// RT1: oct-encoded object-space normal (RG), normalized depth (B), roughness (A).
//      RG = oct-normal in [0,1]  (packed from [-1,1] via * 0.5 + 0.5)
//      B  = NDC depth [0,1]  (0 = near / front of bounding sphere)
//      A  = roughness (constant 0.5 — per-material roughness not yet authored)

#define RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), RootConstants(num32BitConstants=16, b0), DescriptorTable(SRV(t0, numDescriptors=1), visibility=SHADER_VISIBILITY_PIXEL), StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP, visibility=SHADER_VISIBILITY_PIXEL)"

Texture2D<float4> g_base_color : register(t0);
SamplerState      g_sampler    : register(s0);

struct PsInput
{
	float4 sv_pos    : SV_Position;
	float2 uv        : TEXCOORD0;
	float3 normal_os : TEXCOORD1;
	float  ndc_depth : TEXCOORD2;
};

struct PsOutput
{
	float4 rt0 : SV_Target0;  // base color + coverage
	float4 rt1 : SV_Target1;  // oct-normal + depth + roughness
};

// Octahedral encode: maps a unit sphere normal to a 2D vector in [-1,1].
float2 OctEncode(float3 n)
{
	float3 a = abs(n);
	float2 p = n.xy / (a.x + a.y + a.z);
	if (n.z < 0.0f)
		p = (1.0f - abs(p.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f,
										 n.y >= 0.0f ? 1.0f : -1.0f);
	return p;
}

[RootSignature(RS)]
PsOutput main(PsInput input)
{
	float4 color = g_base_color.Sample(g_sampler, input.uv);
	clip(color.a - 0.1f);
	// Binarize alpha: write 1.0 for all covered pixels so that bilinear
	// filtering at leaf-card edges does not pull the atlas alpha below the
	// runtime intersection shader's 0.5 threshold.
	color.a = 1.0f;

	// Encode normal and depth for RT1.
	float2 oct = OctEncode(normalize(input.normal_os)) * 0.5f + 0.5f;

	PsOutput o;
	o.rt0 = color;
	o.rt1 = float4(oct.x, oct.y, input.ndc_depth, 0.5f);  // roughness = 0.5
	return o;
}
