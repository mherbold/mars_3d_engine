// =============================================================================
// mars_omm_baker — Offline Opacity Micro-Map (OMM) baking tool
//
// Evaluates each triangle's per-micro-triangle opacity state using the mesh's
// UV coordinates and base-color alpha channel, then serialises the result as a
// D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE blob.
//
// The runtime BLAS builder in PathTracer::build_vegetation_lod_blas() checks
// for a matching .ommblob beside the model file and attaches it so DXR can
// skip AnyHit invocations on fully-opaque micro-triangles.
//
// Usage:
//   mars_omm_baker --model   <path/to/model.fbx>
//                  --texture <path/to/alpha.dds>
//                  --output  <path/to/output.ommblob>
//                 [--subdivision-level <N>]   default: 4 (256 micro-tris/tri)
// =============================================================================

#include <mars_engine/mars_engine.h>
#include <DirectXTex.h>

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <array>
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// OC1 4-state codes (D3D12_RAYTRACING_OPACITY_MICROMAP_STATE_OC1_*)
// ---------------------------------------------------------------------------
enum class OC1State : uint8_t
{
	TransparentBlack = 0, // fully transparent
	OpaqueBlack      = 1, // fully opaque, black side
	TransparentWhite = 2, // fully transparent, white side
	OpaqueWhite      = 3, // fully opaque, white side
};

// ---------------------------------------------------------------------------
// .ommblob file header
// Identifies the format so the runtime can sanity-check the blob.
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct OmmBlobHeader
{
	uint32_t magic             = 0x4F4D4D42u; // 'OMMB'
	uint32_t version           = 1;
	uint32_t triangle_count    = 0;
	uint32_t subdivision_level = 0;
	// Followed by triangle_count × (4^subdivision_level / 4) bytes of OMM data.
	// Each byte stores 4 two-bit OC1 states packed LSB-first.
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
static void print_usage()
{
	std::printf(
		"Usage:\n"
		"  mars_omm_baker --model   <mesh.fbx>\n"
		"                 --texture <alpha.dds>\n"
		"                 --output  <output.ommblob>\n"
		"                [--subdivision-level <N>]  (default: 4)\n"
		"\n"
		"Example:\n"
		"  mars_omm_baker --model   models/trees/linden_lod0.fbx\n"
		"                 --texture models/trees/linden_leaf_diffuse.dds\n"
		"                 --output  models/trees/linden_lod0.ommblob\n"
	);
}

static void fatal(const char* msg)
{
	std::fprintf(stderr, "[omm_baker] FATAL: %s\n", msg);
	std::exit(1);
}

// ---------------------------------------------------------------------------
// Sample the alpha channel of a loaded DirectXTex image at (u,v) in [0,1].
// Returns a value in [0, 1].
// ---------------------------------------------------------------------------
static float sample_alpha(const DirectX::ScratchImage& img, float u, float v)
{
	const DirectX::Image* im = img.GetImage(0, 0, 0);
	if (!im || !im->pixels) return 1.0f;

	// Wrap UVs
	u = u - std::floor(u);
	v = v - std::floor(v);

	uint32_t px = static_cast<uint32_t>(u * static_cast<float>(im->width - 1) + 0.5f);
	uint32_t py = static_cast<uint32_t>(v * static_cast<float>(im->height - 1) + 0.5f);
	px = std::min(px, static_cast<uint32_t>(im->width  - 1));
	py = std::min(py, static_cast<uint32_t>(im->height - 1));

	// Only RGBA8 / BGRA8 formats are handled here; the full implementation
	// would use DXGI format introspection.
	if (im->format == DXGI_FORMAT_R8G8B8A8_UNORM ||
		im->format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
	{
		const uint8_t* pixel = im->pixels + py * im->rowPitch + px * 4;
		return static_cast<float>(pixel[3]) / 255.0f;
	}
	if (im->format == DXGI_FORMAT_B8G8R8A8_UNORM ||
		im->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
	{
		const uint8_t* pixel = im->pixels + py * im->rowPitch + px * 4;
		return static_cast<float>(pixel[3]) / 255.0f;
	}

	// Unknown format — treat as fully opaque
	return 1.0f;
}

// ---------------------------------------------------------------------------
// Evaluate OC1 4-state for one micro-triangle by sampling its three UV corners.
// The alpha threshold for "opaque" is 0.5.
// ---------------------------------------------------------------------------
static OC1State evaluate_micro_triangle(
	const DirectX::ScratchImage& alpha_tex,
	float u0, float v0,
	float u1, float v1,
	float u2, float v2,
	float alpha_cutoff = 0.5f)
{
	float a0 = sample_alpha(alpha_tex, u0, v0);
	float a1 = sample_alpha(alpha_tex, u1, v1);
	float a2 = sample_alpha(alpha_tex, u2, v2);
	float avg = (a0 + a1 + a2) / 3.0f;

	return (avg >= alpha_cutoff) ? OC1State::OpaqueBlack : OC1State::TransparentBlack;
}

// ---------------------------------------------------------------------------
// Build OMM data for all triangles in a mesh, given UVs per vertex.
// Returns a packed byte vector (4 states per byte, LSB-first).
// ---------------------------------------------------------------------------
static std::vector<uint8_t> bake_omm_data(
	const std::vector<uint32_t>&              indices,
	const std::vector<std::array<float, 2>>&  uvs,
	const DirectX::ScratchImage&              alpha_tex,
	uint32_t                                  subdivision_level,
	float                                     alpha_cutoff = 0.5f)
{
	const uint32_t tri_count      = static_cast<uint32_t>(indices.size() / 3);
	const uint32_t micro_per_tri  = 1u << (2u * subdivision_level); // 4^N
	const uint32_t bytes_per_tri  = (micro_per_tri + 3u) / 4u;      // 2 bits each

	std::vector<uint8_t> data(static_cast<size_t>(tri_count) * bytes_per_tri, 0u);

	for (uint32_t ti = 0; ti < tri_count; ++ti)
	{
		const uint32_t i0 = indices[ti * 3 + 0];
		const uint32_t i1 = indices[ti * 3 + 1];
		const uint32_t i2 = indices[ti * 3 + 2];

		const float bu0 = uvs[i0][0], bv0 = uvs[i0][1];
		const float bu1 = uvs[i1][0], bv1 = uvs[i1][1];
		const float bu2 = uvs[i2][0], bv2 = uvs[i2][1];

		// Subdivide the triangle uniformly using barycentric micro-triangle enumeration.
		// For subdivision level L, there are 4^L micro-triangles in a regular pattern.
		const uint32_t grid = 1u << subdivision_level; // micro-triangles per edge

		uint32_t micro_idx = 0;
		for (uint32_t row = 0; row < grid; ++row)
		{
			for (uint32_t col = 0; col <= row; ++col)
			{
				// Upward-pointing micro-triangle barycentric coordinates
				float w0 = 1.0f - static_cast<float>(row) / static_cast<float>(grid);
				float w1 = static_cast<float>(col)       / static_cast<float>(grid);
				float w2 = 1.0f - w0 - w1;

				float mu0 = bu0*w0 + bu1*w1 + bu2*w2;
				float mv0 = bv0*w0 + bv1*w1 + bv2*w2;

				float w0b = 1.0f - static_cast<float>(row + 1) / static_cast<float>(grid);
				float w1b = w1;
				float w2b = 1.0f - w0b - w1b;

				float mu1 = bu0*w0b + bu1*w1b + bu2*w2b;
				float mv1 = bv0*w0b + bv1*w1b + bv2*w2b;

				float w0c = w0;
				float w1c = static_cast<float>(col + 1) / static_cast<float>(grid);
				float w2c = 1.0f - w0c - w1c;

				float mu2 = bu0*w0c + bu1*w1c + bu2*w2c;
				float mv2 = bv0*w0c + bv1*w1c + bv2*w2c;

				OC1State state = evaluate_micro_triangle(
					alpha_tex, mu0, mv0, mu1, mv1, mu2, mv2, alpha_cutoff);

				// Pack two bits into the output byte array
				const uint32_t bit_offset = micro_idx * 2u;
				const uint32_t byte_idx   = static_cast<size_t>(ti) * bytes_per_tri + (bit_offset / 8u);
				const uint32_t bit_in_byte= bit_offset % 8u;
				data[byte_idx] |= (static_cast<uint8_t>(state) << bit_in_byte);

				++micro_idx;

				// Downward-pointing micro-triangle (if not at row boundary)
				if (col < row)
				{
					float dw0 = 1.0f - static_cast<float>(row)     / static_cast<float>(grid);
					float dw1 = static_cast<float>(col + 1)         / static_cast<float>(grid);
					float dw2 = 1.0f - dw0 - dw1;

					float du0 = bu0*dw0 + bu1*dw1 + bu2*dw2;
					float dv0 = bv0*dw0 + bv1*dw1 + bv2*dw2;

					float dw0b = 1.0f - static_cast<float>(row + 1) / static_cast<float>(grid);
					float dw1b = dw1;
					float dw2b = 1.0f - dw0b - dw1b;

					float du1 = bu0*dw0b + bu1*dw1b + bu2*dw2b;
					float dv1 = bv0*dw0b + bv1*dw1b + bv2*dw2b;

					float dw0c = dw0;
					float dw1c = static_cast<float>(col)            / static_cast<float>(grid);
					float dw2c = 1.0f - dw0c - dw1c;

					float du2 = bu0*dw0c + bu1*dw1c + bu2*dw2c;
					float dv2 = bv0*dw0c + bv1*dw1c + bv2*dw2c;

					OC1State ds = evaluate_micro_triangle(
						alpha_tex, du0, dv0, du1, dv1, du2, dv2, alpha_cutoff);

					const uint32_t dbit_offset = micro_idx * 2u;
					const uint32_t dbyte_idx   = static_cast<size_t>(ti) * bytes_per_tri + (dbit_offset / 8u);
					const uint32_t dbit_in_byte= dbit_offset % 8u;
					data[dbyte_idx] |= (static_cast<uint8_t>(ds) << dbit_in_byte);

					++micro_idx;
				}
			}
		}
	}

	return data;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
	std::string model_path;
	std::string texture_path;
	std::string output_path;
	uint32_t    subdivision_level = 4;

	for (int i = 1; i < argc; ++i)
	{
		if      (std::strcmp(argv[i], "--model")             == 0 && i+1 < argc) model_path         = argv[++i];
		else if (std::strcmp(argv[i], "--texture")           == 0 && i+1 < argc) texture_path       = argv[++i];
		else if (std::strcmp(argv[i], "--output")            == 0 && i+1 < argc) output_path        = argv[++i];
		else if (std::strcmp(argv[i], "--subdivision-level") == 0 && i+1 < argc) subdivision_level  = static_cast<uint32_t>(std::atoi(argv[++i]));
		else if (std::strcmp(argv[i], "--help")              == 0)               { print_usage(); return 0; }
	}

	if (model_path.empty() || texture_path.empty() || output_path.empty())
	{
		print_usage();
		return 1;
	}

	if (!fs::exists(model_path))   { std::fprintf(stderr, "[omm_baker] Model not found: %s\n",   model_path.c_str());   return 1; }
	if (!fs::exists(texture_path)) { std::fprintf(stderr, "[omm_baker] Texture not found: %s\n", texture_path.c_str()); return 1; }

	fs::path out_dir = fs::path(output_path).parent_path();
	if (!out_dir.empty() && !fs::exists(out_dir))
		fs::create_directories(out_dir);

	std::printf("[omm_baker] mars_omm_baker\n");
	std::printf("[omm_baker]   model             : %s\n", model_path.c_str());
	std::printf("[omm_baker]   texture           : %s\n", texture_path.c_str());
	std::printf("[omm_baker]   output            : %s\n", output_path.c_str());
	std::printf("[omm_baker]   subdivision level : %u (%u micro-triangles/triangle)\n",
		subdivision_level, 1u << (2u * subdivision_level));

	try
	{
		// ---- Load alpha texture -----------------------------------------------
		DirectX::ScratchImage alpha_tex;
		std::wstring wtex(texture_path.begin(), texture_path.end());
		HRESULT hr;

		if (texture_path.ends_with(".dds") || texture_path.ends_with(".DDS"))
			hr = DirectX::LoadFromDDSFile(wtex.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, alpha_tex);
		else
			hr = DirectX::LoadFromWICFile(wtex.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, alpha_tex);

		if (FAILED(hr))
			fatal("Failed to load alpha texture.");

		// Convert to RGBA8 so sample_alpha can access pixel bytes uniformly
		if (alpha_tex.GetMetadata().format != DXGI_FORMAT_R8G8B8A8_UNORM)
		{
			DirectX::ScratchImage converted;
			if (SUCCEEDED(DirectX::Convert(*alpha_tex.GetImage(0,0,0),
										   DXGI_FORMAT_R8G8B8A8_UNORM,
										   DirectX::TEX_FILTER_DEFAULT,
										   DirectX::TEX_THRESHOLD_DEFAULT,
										   converted)))
				alpha_tex = std::move(converted);
		}

		std::printf("[omm_baker] Texture loaded: %zux%zu %s\n",
			alpha_tex.GetMetadata().width,
			alpha_tex.GetMetadata().height,
			texture_path.c_str());

		// ---- Load mesh UVs via AssetImporter ----------------------------------
		mars::AssetImporter importer;
		auto model_asset = importer.import(model_path, /*pre_transform_vertices=*/true);
		if (!model_asset)
			fatal("Failed to import model.");

		uint32_t total_tris  = 0;
		uint32_t total_bytes = 0;

		OmmBlobHeader hdr{};
		hdr.subdivision_level = subdivision_level;

		std::vector<uint8_t> all_omm_data;

		for (const auto& mesh : model_asset->meshes)
		{
			const uint32_t tri_count      = static_cast<uint32_t>(mesh.indices.size() / 3);
			const uint32_t micro_per_tri  = 1u << (2u * subdivision_level);
			const uint32_t bytes_per_tri  = (micro_per_tri + 3u) / 4u;

			if (mesh.vertices.empty() || mesh.indices.empty())
			{
				std::printf("[omm_baker]   Mesh '%s' has no vertices/UVs — filling with OpaqueBlack.\n",
					mesh.name.c_str());
				// Fill all micro-triangles as opaque
				std::vector<uint8_t> opaque_data(static_cast<size_t>(tri_count) * bytes_per_tri, 0xFFu);
				all_omm_data.insert(all_omm_data.end(), opaque_data.begin(), opaque_data.end());
				total_tris  += tri_count;
				total_bytes += static_cast<uint32_t>(opaque_data.size());
				continue;
			}

			// Flatten UVs into [float,float] pairs from interleaved Vertex layout
			std::vector<std::array<float, 2>> flat_uvs;
			flat_uvs.reserve(mesh.vertices.size());
			for (const auto& vtx : mesh.vertices)
				flat_uvs.push_back({ vtx.uv.x, vtx.uv.y });

			auto mesh_omm = bake_omm_data(mesh.indices, flat_uvs, alpha_tex,
										  subdivision_level);

			std::printf("[omm_baker]   Mesh '%s': %u triangles → %zu OMM bytes\n",
				mesh.name.c_str(), tri_count, mesh_omm.size());

			all_omm_data.insert(all_omm_data.end(), mesh_omm.begin(), mesh_omm.end());
			total_tris  += tri_count;
			total_bytes += static_cast<uint32_t>(mesh_omm.size());
		}

		hdr.triangle_count = total_tris;

		// ---- Write .ommblob ---------------------------------------------------
		std::ofstream ofs(output_path, std::ios::binary);
		if (!ofs) fatal("Cannot open output file for writing.");

		ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
		ofs.write(reinterpret_cast<const char*>(all_omm_data.data()),
				  static_cast<std::streamsize>(all_omm_data.size()));

		std::printf("[omm_baker] Written: %s  (%u triangles, %u bytes of OMM data)\n",
			output_path.c_str(), total_tris, total_bytes);
	}
	catch (const std::exception& ex)
	{
		std::fprintf(stderr, "[omm_baker] ERROR: %s\n", ex.what());
		return 1;
	}

	std::printf("[omm_baker] Done.\n");
	return 0;
}
