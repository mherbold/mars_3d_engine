// =============================================================================
// asset_importer.cpp
// MARS 3D Engine — Assimp-based model importer implementation
// =============================================================================

#include "mars_engine/asset/asset_importer.h"

#pragma warning(push, 0)
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#pragma warning(pop)

#include <stdexcept>
#include <filesystem>
#include <format>
#include <print>
#include <cassert>
#include <algorithm>

namespace mars
{

// ---------------------------------------------------------------------------
// Internal import context — avoids passing many args through recursive calls
// ---------------------------------------------------------------------------
struct AssetImporter::ImportContext
{
    const aiScene* ai_scene   = nullptr;
    std::string    base_dir;    // directory of the source file
    ModelAsset*    asset       = nullptr;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static Vec3 to_vec3(const aiVector3D& v) { return {v.x, v.y, v.z}; }
static Vec2 to_vec2(const aiVector3D& v) { return {v.x, v.y}; }
static Vec4 to_vec4(const aiColor4D&  c) { return {c.r, c.g, c.b, c.a}; }
static Vec3 to_vec3(const aiColor3D&  c) { return {c.r, c.g, c.b}; }

static std::string resolve_texture(const std::string& base_dir, const aiString& ai_path)
{
    if (ai_path.length == 0) return {};
    namespace fs = std::filesystem;
    fs::path p(ai_path.C_Str());
    if (p.is_absolute()) return p.string();
    return (fs::path(base_dir) / p).string();
}

// ---------------------------------------------------------------------------
// process_material
// ---------------------------------------------------------------------------
MaterialData AssetImporter::process_material(ImportContext& ctx, uint32_t mat_index)
{
    const aiMaterial* ai_mat = ctx.ai_scene->mMaterials[mat_index];
    MaterialData mat;

    // Name
    aiString name;
    if (ai_mat->Get(AI_MATKEY_NAME, name) == AI_SUCCESS)
        mat.name = name.C_Str();

    // Base color / diffuse factor
    aiColor4D diffuse;
    if (ai_mat->Get(AI_MATKEY_BASE_COLOR, diffuse) == AI_SUCCESS)
        mat.base_color_factor = to_vec4(diffuse);
    else if (ai_mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS)
        mat.base_color_factor = to_vec4(diffuse);

    // Metallic / roughness
    float metallic = 0.0f, roughness = 1.0f;
    ai_mat->Get(AI_MATKEY_METALLIC_FACTOR,   metallic);
    ai_mat->Get(AI_MATKEY_ROUGHNESS_FACTOR,  roughness);
    mat.metallic_factor  = metallic;
    mat.roughness_factor = roughness;

    // Emissive
    aiColor3D emissive;
    if (ai_mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS)
        mat.emissive_factor = to_vec3(emissive);

    // Two-sided
    int two_sided = 0;
    ai_mat->Get(AI_MATKEY_TWOSIDED, two_sided);
    mat.double_sided = two_sided != 0;

    // Textures
    aiString tex_path;
    auto resolve = [&](const aiString& p, bool srgb) -> TextureRef
    {
        TextureRef ref;
        ref.path   = resolve_texture(ctx.base_dir, p);
        ref.is_srgb = srgb;
        return ref;
    };

    if (ai_mat->GetTexture(aiTextureType_BASE_COLOR, 0, &tex_path) == AI_SUCCESS ||
        ai_mat->GetTexture(aiTextureType_DIFFUSE,    0, &tex_path) == AI_SUCCESS)
        mat.base_color_texture = resolve(tex_path, true);

    if (ai_mat->GetTexture(aiTextureType_NORMALS, 0, &tex_path) == AI_SUCCESS ||
        ai_mat->GetTexture(aiTextureType_HEIGHT,  0, &tex_path) == AI_SUCCESS)
        mat.normal_texture = resolve(tex_path, false);

    if (ai_mat->GetTexture(aiTextureType_METALNESS, 0, &tex_path) == AI_SUCCESS ||
        ai_mat->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &tex_path) == AI_SUCCESS)
        mat.metallic_roughness_texture = resolve(tex_path, false);

    if (ai_mat->GetTexture(aiTextureType_EMISSIVE, 0, &tex_path) == AI_SUCCESS)
        mat.emissive_texture = resolve(tex_path, true);

    if (ai_mat->GetTexture(aiTextureType_AMBIENT_OCCLUSION, 0, &tex_path) == AI_SUCCESS ||
        ai_mat->GetTexture(aiTextureType_LIGHTMAP, 0, &tex_path) == AI_SUCCESS)
        mat.occlusion_texture = resolve(tex_path, false);

    return mat;
}

// ---------------------------------------------------------------------------
// process_mesh
// ---------------------------------------------------------------------------
MeshData AssetImporter::process_mesh(ImportContext& /*ctx*/, const void* ai_mesh_ptr)
{
    const aiMesh* ai_mesh = static_cast<const aiMesh*>(ai_mesh_ptr);
    MeshData mesh;
    mesh.name           = ai_mesh->mName.C_Str();
    mesh.material_index = ai_mesh->mMaterialIndex;

    // Vertices
    mesh.vertices.reserve(ai_mesh->mNumVertices);
    AABB bounds;
    bool first = true;

    for (uint32_t i = 0; i < ai_mesh->mNumVertices; ++i)
    {
        Vertex v;
        v.position = to_vec3(ai_mesh->mVertices[i]);

        if (ai_mesh->HasNormals())
            v.normal = to_vec3(ai_mesh->mNormals[i]);

        if (ai_mesh->HasTangentsAndBitangents())
            v.tangent = to_vec3(ai_mesh->mTangents[i]);

        if (ai_mesh->HasTextureCoords(0))
            v.uv = to_vec2(ai_mesh->mTextureCoords[0][i]);

        if (ai_mesh->HasBones())
        {
            // Bone weights are filled per-bone below; weights array already zero-init.
        }

        if (first) { bounds.min_pt = bounds.max_pt = v.position; first = false; }
        else        bounds.expand(v.position);

        mesh.vertices.push_back(v);
    }

    // Bone weights (up to 4 influences per vertex)
    if (ai_mesh->HasBones())
    {
        // Temporary arrays for accumulating influences
        std::vector<uint32_t> influence_count(ai_mesh->mNumVertices, 0);

        for (uint32_t b = 0; b < ai_mesh->mNumBones; ++b)
        {
            const aiBone* bone = ai_mesh->mBones[b];
            for (uint32_t w = 0; w < bone->mNumWeights; ++w)
            {
                uint32_t vi     = bone->mWeights[w].mVertexId;
                uint32_t slot   = influence_count[vi];
                if (slot < 4)
                {
                    mesh.vertices[vi].bone_indices[slot] = b;
                    mesh.vertices[vi].bone_weights[slot] = bone->mWeights[w].mWeight;
                    ++influence_count[vi];
                }
            }
        }
    }

    // Indices
    mesh.indices.reserve(static_cast<size_t>(ai_mesh->mNumFaces) * 3);
    for (uint32_t f = 0; f < ai_mesh->mNumFaces; ++f)
    {
        const aiFace& face = ai_mesh->mFaces[f];
        for (uint32_t idx = 0; idx < face.mNumIndices; ++idx)
            mesh.indices.push_back(face.mIndices[idx]);
    }

    mesh.bounds = bounds;
    return mesh;
}

// ---------------------------------------------------------------------------
// process_node — recursive scene-graph traversal
// ---------------------------------------------------------------------------
void AssetImporter::process_node(ImportContext& ctx, const void* ai_node_ptr)
{
    const aiNode* node = static_cast<const aiNode*>(ai_node_ptr);

    for (uint32_t i = 0; i < node->mNumMeshes; ++i)
    {
        uint32_t mesh_index = node->mMeshes[i];
        ctx.asset->meshes.push_back(process_mesh(ctx, ctx.ai_scene->mMeshes[mesh_index]));
    }

    for (uint32_t c = 0; c < node->mNumChildren; ++c)
        process_node(ctx, node->mChildren[c]);
}

// ---------------------------------------------------------------------------
// import
// ---------------------------------------------------------------------------
std::optional<ModelAsset> AssetImporter::import(const std::string& file_path) const
{
    namespace fs = std::filesystem;

    if (!fs::exists(file_path))
    {
        std::println(stderr, "[AssetImporter] File not found: {}", file_path);
        return std::nullopt;
    }

    Assimp::Importer importer;

    const unsigned int flags =
        aiProcess_Triangulate            |   // all faces become triangles
        aiProcess_JoinIdenticalVertices  |   // merge duplicate verts
        aiProcess_GenSmoothNormals       |   // generate normals if absent
        aiProcess_CalcTangentSpace       |   // compute tangents / bitangents
        aiProcess_FlipUVs                |   // D3D UV convention (V flipped)
        aiProcess_LimitBoneWeights       |   // max 4 bone influences per vertex
        aiProcess_ImproveCacheLocality   |   // Forsyth vertex-cache optimisation
        aiProcess_RemoveRedundantMaterials;

    const aiScene* scene = importer.ReadFile(file_path, flags);
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
    {
        std::println(stderr, "[AssetImporter] Failed to load '{}': {}", file_path, importer.GetErrorString());
        return std::nullopt;
    }

    ModelAsset asset;
    asset.name        = fs::path(file_path).stem().string();
    asset.source_path = file_path;

    // Import materials first (meshes reference by index).
    asset.materials.reserve(scene->mNumMaterials);
    for (uint32_t i = 0; i < scene->mNumMaterials; ++i)
    {
        ImportContext ctx_mat;
        ctx_mat.ai_scene = scene;
        ctx_mat.base_dir = fs::path(file_path).parent_path().string();
        ctx_mat.asset    = &asset;
        asset.materials.push_back(process_material(ctx_mat, i));
    }

    // Import meshes via node traversal.
    ImportContext ctx;
    ctx.ai_scene = scene;
    ctx.base_dir = fs::path(file_path).parent_path().string();
    ctx.asset    = &asset;

    process_node(ctx, scene->mRootNode);

    // Compute overall model AABB.
    bool first = true;
    for (const auto& mesh : asset.meshes)
    {
        if (first)
        {
            asset.bounds = mesh.bounds;
            first = false;
        }
        else
        {
            asset.bounds.expand(mesh.bounds.min_pt);
            asset.bounds.expand(mesh.bounds.max_pt);
        }
    }

    std::println("[AssetImporter] Loaded '{}': {} mesh(es), {} material(s), {} vertices total",
        file_path,
        asset.meshes.size(),
        asset.materials.size(),
        [&]() { size_t n = 0; for (auto& m : asset.meshes) n += m.vertices.size(); return n; }());

    return asset;
}

} // namespace mars
