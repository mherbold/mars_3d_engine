// =============================================================================
// asset_importer.cpp
// MARS 3D Engine — Assimp-based model importer implementation
// =============================================================================

#include "mars_engine/engine_api.h"
#include "mars_engine/asset/asset_importer.h"
#include "mars_engine/animation/skeleton.h"
#include "mars_engine/animation/animation_clip.h"

#pragma warning(push, 0)
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#pragma warning(pop)

#include <stdexcept>
#include <filesystem>
#include <format>
#include <cassert>
#include <algorithm>
#include <functional>

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
        MARS_LOG("[AssetImporter] File not found: {}", file_path);
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
        MARS_LOG("[AssetImporter] Failed to load '{}': {}", file_path, importer.GetErrorString());
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

    MARS_LOG("[AssetImporter] Loaded '{}': {} mesh(es), {} material(s), {} vertices total",
        file_path,
        asset.meshes.size(),
        asset.materials.size(),
        [&]() { size_t n = 0; for (auto& m : asset.meshes) n += m.vertices.size(); return n; }());

    return asset;
}

// ---------------------------------------------------------------------------
// import_skeleton
// ---------------------------------------------------------------------------
std::optional<Skeleton> AssetImporter::import_skeleton(const std::string& file_path) const
{
    namespace fs = std::filesystem;

    if (!fs::exists(file_path))
    {
        MARS_LOG("[AssetImporter] File not found: {}", file_path);
        return std::nullopt;
    }

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(file_path,
        aiProcess_Triangulate | aiProcess_LimitBoneWeights);
    if (!scene || !scene->mRootNode)
    {
        MARS_LOG("[AssetImporter] Failed to load '{}': {}", file_path, importer.GetErrorString());
        return std::nullopt;
    }

    // Collect all unique bone names across all meshes.
    std::vector<const aiBone*> all_bones;
    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi)
    {
        const aiMesh* mesh = scene->mMeshes[mi];
        for (uint32_t bi = 0; bi < mesh->mNumBones; ++bi)
            all_bones.push_back(mesh->mBones[bi]);
    }

    if (all_bones.empty())
    {
        MARS_LOG("[AssetImporter] No skeletal data found in '{}'", file_path);
        return std::nullopt;
    }

    // Build a name -> index map from the node hierarchy.
    // We use the first mesh's bone list to seed the skeleton.
    Skeleton skeleton;
    skeleton.name = fs::path(file_path).stem().string();

    // Helper: find a node by name in the scene hierarchy.
    std::function<const aiNode*(const aiNode*, const std::string&)> find_node;
    find_node = [&](const aiNode* node, const std::string& name) -> const aiNode*
    {
        if (std::string(node->mName.C_Str()) == name) return node;
        for (uint32_t c = 0; c < node->mNumChildren; ++c)
        {
            if (auto* r = find_node(node->mChildren[c], name)) return r;
        }
        return nullptr;
    };

    // Deduplicate bone names preserving first-seen order.
    std::vector<std::string> bone_names;
    for (const aiBone* b : all_bones)
    {
        std::string n = b->mName.C_Str();
        if (skeleton.find_bone_index(n) < 0)
        {
            Bone bone;
            bone.name = n;

            // Inverse bind pose from Assimp.
            // aiMatrix4x4 is row-major: a=row0, b=row1, c=row2, d=row3
            //                           1=col0, 2=col1, 3=col2, 4=col3
            const aiMatrix4x4& m = b->mOffsetMatrix;
            bone.inverse_bind_pose = {
                m.a1, m.a2, m.a3, m.a4,
                m.b1, m.b2, m.b3, m.b4,
                m.c1, m.c2, m.c3, m.c4,
                m.d1, m.d2, m.d3, m.d4
            };

            // Local transform from the scene node (if found).
            if (const aiNode* node = find_node(scene->mRootNode, n))
            {
                const aiMatrix4x4& lm = node->mTransformation;
                bone.local_transform = {
                    lm.a1, lm.a2, lm.a3, lm.a4,
                    lm.b1, lm.b2, lm.b3, lm.b4,
                    lm.c1, lm.c2, lm.c3, lm.c4,
                    lm.d1, lm.d2, lm.d3, lm.d4
                };
            }

            bone_names.push_back(n);
            skeleton.bones.push_back(std::move(bone));
        }
    }

    // Resolve parent indices using the scene node hierarchy.
    for (auto& bone : skeleton.bones)
    {
        const aiNode* node = find_node(scene->mRootNode, bone.name);
        if (node && node->mParent)
        {
            bone.parent_index = skeleton.find_bone_index(node->mParent->mName.C_Str());
        }
    }

    MARS_LOG("[AssetImporter] Loaded skeleton from '{}': {} bones", file_path, skeleton.bones.size());
    return skeleton;
}

// ---------------------------------------------------------------------------
// import_animations
// ---------------------------------------------------------------------------
std::vector<AnimationClip> AssetImporter::import_animations(const std::string& file_path) const
{
    namespace fs = std::filesystem;

    if (!fs::exists(file_path))
    {
        MARS_LOG("[AssetImporter] File not found: {}", file_path);
        return {};
    }

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(file_path, aiProcess_Triangulate);
    if (!scene || !scene->mRootNode)
    {
        MARS_LOG("[AssetImporter] Failed to load '{}': {}", file_path, importer.GetErrorString());
        return {};
    }

    if (scene->mNumAnimations == 0)
    {
        MARS_LOG("[AssetImporter] No animations found in '{}'", file_path);
        return {};
    }

    std::vector<AnimationClip> clips;
    clips.reserve(scene->mNumAnimations);

    for (uint32_t ai_idx = 0; ai_idx < scene->mNumAnimations; ++ai_idx)
    {
        const aiAnimation* anim = scene->mAnimations[ai_idx];
        AnimationClip clip;
        clip.name             = anim->mName.C_Str();
        clip.ticks_per_second = static_cast<float>(anim->mTicksPerSecond > 0.0
                                    ? anim->mTicksPerSecond : 30.0);
        clip.duration         = static_cast<float>(anim->mDuration) / clip.ticks_per_second;

        clip.channels.reserve(anim->mNumChannels);
        for (uint32_t ci = 0; ci < anim->mNumChannels; ++ci)
        {
            const aiNodeAnim* ch = anim->mChannels[ci];
            BoneChannel channel;
            channel.bone_name = ch->mNodeName.C_Str();

            channel.position_keys.reserve(ch->mNumPositionKeys);
            for (uint32_t k = 0; k < ch->mNumPositionKeys; ++k)
            {
                PositionKey pk;
                pk.time  = static_cast<float>(ch->mPositionKeys[k].mTime) / clip.ticks_per_second;
                pk.value = { ch->mPositionKeys[k].mValue.x,
                             ch->mPositionKeys[k].mValue.y,
                             ch->mPositionKeys[k].mValue.z };
                channel.position_keys.push_back(pk);
            }

            channel.rotation_keys.reserve(ch->mNumRotationKeys);
            for (uint32_t k = 0; k < ch->mNumRotationKeys; ++k)
            {
                RotationKey rk;
                rk.time  = static_cast<float>(ch->mRotationKeys[k].mTime) / clip.ticks_per_second;
                rk.value = { ch->mRotationKeys[k].mValue.x,
                             ch->mRotationKeys[k].mValue.y,
                             ch->mRotationKeys[k].mValue.z,
                             ch->mRotationKeys[k].mValue.w };
                channel.rotation_keys.push_back(rk);
            }

            channel.scale_keys.reserve(ch->mNumScalingKeys);
            for (uint32_t k = 0; k < ch->mNumScalingKeys; ++k)
            {
                ScaleKey sk;
                sk.time  = static_cast<float>(ch->mScalingKeys[k].mTime) / clip.ticks_per_second;
                // Use X component as uniform scale.
                sk.value = ch->mScalingKeys[k].mValue.x;
                channel.scale_keys.push_back(sk);
            }

            clip.channels.push_back(std::move(channel));
        }

        clips.push_back(std::move(clip));
    }

    MARS_LOG("[AssetImporter] Loaded {} animation(s) from '{}'", clips.size(), file_path);
    return clips;
}

} // namespace mars
