// #profile cs_6_6
// =============================================================================
// skinning.hlsl
// MARS Animation System — GPU skinning compute shader
//
// Transforms vertices by a skeletal bone palette to produce animated meshes.
// Input: source vertex buffer (with bone indices/weights) + bone palette
// Output: skinned vertex buffer (ready for BLAS refit)
// =============================================================================

#pragma pack_matrix(row_major)

// ---------------------------------------------------------------------------
// Skinning constant buffer
// ---------------------------------------------------------------------------
struct SkinningConstants
{
	uint vertex_count;              // Number of vertices to skin
	uint source_vertex_buffer_srv;  // Bindless SRV slot for source vertices
	uint bone_palette_srv;          // Bindless SRV slot for bone matrices
	uint output_vertex_buffer_uav;  // Bindless UAV slot for skinned output
};

ConstantBuffer<SkinningConstants> g_Skinning : register(b0, space0);

// Bindless resource arrays
ByteAddressBuffer           g_Buffers[]          : register(t0, space1);
StructuredBuffer<float4x4>  g_BonePalettes[]     : register(t0, space2);
RWByteAddressBuffer         g_OutputBuffers[]    : register(u0, space3);

// ---------------------------------------------------------------------------
// Vertex layout (must match C++ Vertex struct)
// ---------------------------------------------------------------------------
struct GpuVertex
{
	float3 position;        // offset  0 (12 bytes)
	float3 normal;          // offset 12 (12 bytes)
	float3 tangent;         // offset 24 (12 bytes)
	float2 uv;              // offset 36 ( 8 bytes)
	uint4  bone_indices;    // offset 44 (16 bytes)
	float4 bone_weights;    // offset 60 (16 bytes)
};

static const uint k_vertex_stride = 76u;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
GpuVertex LoadVertex(ByteAddressBuffer buffer, uint index)
{
	uint base = index * k_vertex_stride;

	GpuVertex v;
	v.position     = asfloat(buffer.Load3(base +  0u));
	v.normal       = asfloat(buffer.Load3(base + 12u));
	v.tangent      = asfloat(buffer.Load3(base + 24u));
	v.uv           = asfloat(buffer.Load2(base + 36u));
	v.bone_indices = buffer.Load4(base + 44u);
	v.bone_weights = asfloat(buffer.Load4(base + 60u));

	return v;
}

void StoreVertex(RWByteAddressBuffer buffer, uint index, GpuVertex v)
{
	uint base = index * k_vertex_stride;

	buffer.Store3(base +  0u, asuint(v.position));
	buffer.Store3(base + 12u, asuint(v.normal));
	buffer.Store3(base + 24u, asuint(v.tangent));
	buffer.Store2(base + 36u, asuint(v.uv));
	buffer.Store4(base + 44u, v.bone_indices);
	buffer.Store4(base + 60u, asuint(v.bone_weights));
}

float3 TransformPosition(float4x4 mat, float3 pos)
{
	float4 result = mul(mat, float4(pos, 1.0f));
	return result.xyz;
}

float3 TransformNormal(float4x4 mat, float3 n)
{
	// For normals, we only apply the rotation/scale part (ignore translation)
	// Extract 3x3 upper-left and transform
	float3 result = mul((float3x3)mat, n);
	return normalize(result);
}

// ---------------------------------------------------------------------------
// Main compute shader
// ---------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint vertexIndex = dispatchThreadID.x;

	if (vertexIndex >= g_Skinning.vertex_count)
		return;

	// Load source vertex
	ByteAddressBuffer sourceBuffer = g_Buffers[g_Skinning.source_vertex_buffer_srv];
	GpuVertex v = LoadVertex(sourceBuffer, vertexIndex);

	// Load bone palette
	StructuredBuffer<float4x4> bonePalette = g_BonePalettes[g_Skinning.bone_palette_srv];

	// Compute weighted blend of bone transforms
	float4x4 skinMatrix = (float4x4)0;

	[unroll]
	for (uint i = 0u; i < 4u; ++i)
	{
		uint  boneIndex = v.bone_indices[i];
		float weight    = v.bone_weights[i];

		if (weight > 0.0f)
		{
			float4x4 boneMatrix = bonePalette[boneIndex];

			// Accumulate weighted bone transform
			skinMatrix[0] += boneMatrix[0] * weight;
			skinMatrix[1] += boneMatrix[1] * weight;
			skinMatrix[2] += boneMatrix[2] * weight;
			skinMatrix[3] += boneMatrix[3] * weight;
		}
	}

	// Transform vertex attributes
	GpuVertex skinned;
	skinned.position     = TransformPosition(skinMatrix, v.position);
	skinned.normal       = TransformNormal(skinMatrix, v.normal);
	skinned.tangent      = TransformNormal(skinMatrix, v.tangent);
	skinned.uv           = v.uv;
	skinned.bone_indices = v.bone_indices;
	skinned.bone_weights = v.bone_weights;

	// Write skinned vertex to output buffer
	RWByteAddressBuffer outputBuffer = g_OutputBuffers[g_Skinning.output_vertex_buffer_uav];
	StoreVertex(outputBuffer, vertexIndex, skinned);
}
