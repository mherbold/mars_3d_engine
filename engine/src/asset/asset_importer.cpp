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
#include <assimp/GltfMaterial.h>
#pragma warning(pop)

#include <stdexcept>
#include <filesystem>
#include <format>
#include <cassert>
#include <algorithm>
#include <cctype>
#include <functional>
#include <string_view>

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

    // Alpha mode — set alpha_masked + alpha_cutoff for cutout materials.
    //
    // Sources, in order of preference:
    //   1. glTF: AI_MATKEY_GLTF_ALPHAMODE == "MASK" (+ AI_MATKEY_GLTF_ALPHACUTOFF)
    //   2. Generic Assimp opacity < 1.0 with a binary alpha texture
    //   3. Name heuristic — SpeedTree FBX exports do not carry an alpha-mode
    //      property, so we fall back to matching the material or texture name
    //      against the conventional leaf/frond/branch-cap atlas tokens. These
    //      cards are pre-multiplied-alpha cutouts in every SpeedTree export.
    aiString alpha_mode;
    if (ai_mat->Get(AI_MATKEY_GLTF_ALPHAMODE, alpha_mode) == AI_SUCCESS)
    {
        std::string m = alpha_mode.C_Str();
        if (m == "MASK" || m == "BLEND") // both end up using cutout in this path tracer
            mat.alpha_masked = true;
    }
    float gltf_cutoff = 0.5f;
    if (ai_mat->Get(AI_MATKEY_GLTF_ALPHACUTOFF, gltf_cutoff) == AI_SUCCESS)
        mat.alpha_cutoff = gltf_cutoff;

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

    // SpeedTree / generic name heuristic for alpha-tested foliage cards.
    if (!mat.alpha_masked)
    {
        auto contains_ci = [](const std::string& s, std::string_view needle) {
            if (s.size() < needle.size()) return false;
            for (size_t i = 0; i + needle.size() <= s.size(); ++i)
            {
                bool ok = true;
                for (size_t j = 0; j < needle.size(); ++j)
                {
                    char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i + j])));
                    char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
                    if (a != b) { ok = false; break; }
                }
                if (ok) return true;
            }
            return false;
        };
        const std::string& bc = mat.base_color_texture.path;
        static constexpr std::string_view k_leaf_tokens[] = {
            "leaves", "leaf", "frond", "fronds",
            "atlas", "billboard", "facing", "card"
        };
        for (auto tok : k_leaf_tokens)
        {
            if (contains_ci(mat.name, tok) || contains_ci(bc, tok))
            {
                mat.alpha_masked = true;
                // Leaf cards are visible from both faces.
                mat.double_sided = true;
                break;
            }
        }
    }

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
std::optional<ModelAsset> AssetImporter::import(const std::string& file_path,
                                                bool pre_transform_vertices) const
{
    namespace fs = std::filesystem;

    if (!fs::exists(file_path))
    {
        MARS_LOG("[AssetImporter] File not found: {}", file_path);
        return std::nullopt;
    }

    Assimp::Importer importer;

    // Ensure UnitScaleFactor (FBX cm -> m, etc.) is honoured when we ask
    // Assimp to bake transforms into vertex positions.
    if (pre_transform_vertices)
    {
        importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
    }

    unsigned int flags =
        aiProcess_Triangulate            |   // all faces become triangles
        aiProcess_JoinIdenticalVertices  |   // merge duplicate verts
        aiProcess_GenSmoothNormals       |   // generate normals if absent
        aiProcess_CalcTangentSpace       |   // compute tangents / bitangents
        aiProcess_FlipUVs                |   // D3D UV convention (V flipped)
        aiProcess_LimitBoneWeights       |   // max 4 bone influences per vertex
        aiProcess_ImproveCacheLocality   |   // Forsyth vertex-cache optimisation
        aiProcess_RemoveRedundantMaterials;

    if (pre_transform_vertices)
    {
        // Bake every node's accumulated transform into the mesh vertices so the
        // model is in a single, flat coordinate space at world-scale. Also apply
        // the source file's global UnitScaleFactor (FBX cm -> m).
        // NOTE: aiProcess_PreTransformVertices is incompatible with bone /
        // skeletal data, which is why this path is opt-in.
        flags |= aiProcess_PreTransformVertices | aiProcess_GlobalScale;
    }

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

    // NOTE: we deliberately keep submeshes separate even when
    // pre_transform_vertices is set. Vegetation FBXs (SpeedTree) split a tree
    // into bark / branches / leaves / fronds submeshes, each with its own
    // material. The renderer registers one TLAS instance per submesh so each
    // gets the correct texture; merging here would force every submesh to
    // share the first submesh's material and the tree would render
    // single-coloured.

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

// ---------------------------------------------------------------------------
// import_vegetation_species
// ---------------------------------------------------------------------------
std::vector<ModelAsset> AssetImporter::import_vegetation_species(const std::string& species_path) const
{
    namespace fs = std::filesystem;

    MARS_LOG("[AssetImporter] Loading vegetation species from '{}'", species_path);

    std::vector<ModelAsset> lods;
    lods.reserve(3); // HighPoly, LowPoly, FarCluster (Impostor is separate)

    // Build expected paths
    const fs::path base(species_path);
    const std::string species_name = base.filename().string();

    // Try to find the .fbx files in HighPoly/ and LowPoly/ subdirectories
    // SpeedTree ORCA assets use the species name as the FBX filename
    const fs::path high_poly_dir = base / "HighPoly";
    const fs::path low_poly_dir  = base / "LowPoly";
    const fs::path textures_dir  = base / "Textures";

    // Find the first .fbx file in each directory (some species have multiple LODs)
    auto find_first_fbx = [](const fs::path& dir) -> std::string
    {
        if (!fs::exists(dir) || !fs::is_directory(dir))
            return {};

        for (const auto& entry : fs::directory_iterator(dir))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".fbx")
                return entry.path().string();
        }
        return {};
    };

    const std::string high_poly_fbx = find_first_fbx(high_poly_dir);
    const std::string low_poly_fbx  = find_first_fbx(low_poly_dir);

    if (high_poly_fbx.empty())
    {
        MARS_LOG("[AssetImporter] ERROR: No .fbx found in '{}/HighPoly/'", species_path);
        return {};
    }

    if (low_poly_fbx.empty())
    {
        MARS_LOG("[AssetImporter] ERROR: No .fbx found in '{}/LowPoly/'", species_path);
        return {};
    }

    // Texture paths (SpeedTree ORCA v2 convention)
    const std::string base_color_tex = (textures_dir / "BaseColor.dds").string();
    const std::string specular_tex   = (textures_dir / "Specular.dds").string();
    const std::string normal_tex     = (textures_dir / "Normal.dds").string();

    // Verify textures exist
    bool has_textures = fs::exists(base_color_tex) && 
                        fs::exists(specular_tex) && 
                        fs::exists(normal_tex);

    if (!has_textures)
    {
        MARS_LOG("[AssetImporter] WARNING: Not all expected textures found in '{}/Textures/'", species_path);
        MARS_LOG("[AssetImporter]   Expected: BaseColor.dds, Specular.dds, Normal.dds");
    }

    // Helper to override SpeedTree textures on imported materials
    auto apply_speedtree_textures = [&](ModelAsset& asset)
    {
        if (!has_textures) return;

        for (auto& mat : asset.materials)
        {
            // SpeedTree ORCA uses a specific texture layout:
            //   BaseColor.dds: RGB = base color, A = opacity mask
            //   Specular.dds:  R = occlusion, G = roughness, B = metalness (packed ORM)
            //   Normal.dds:    RGB = tangent-space normal (DirectX convention, Y-up)

            mat.base_color_texture.path   = base_color_tex;
            mat.base_color_texture.is_srgb = true;

            mat.metallic_roughness_texture.path   = specular_tex; // ORM packed
            mat.metallic_roughness_texture.is_srgb = false;

            mat.occlusion_texture.path   = specular_tex; // R channel = occlusion
            mat.occlusion_texture.is_srgb = false;

            mat.normal_texture.path   = normal_tex;
            mat.normal_texture.is_srgb = false;
        }
    };

    // LOD 0: HighPoly
    {
        // Vegetation FBXs (SpeedTree ORCA) contain many independently-placed
        // submeshes (trunk / branches / leaves / fronds) whose final positions
        // are encoded in per-node transforms. They are also exported in
        // centimetres. Bake both into the vertex positions during import.
        auto high_asset = import(high_poly_fbx, /*pre_transform_vertices=*/true);
        if (!high_asset.has_value())
        {
            MARS_LOG("[AssetImporter] ERROR: Failed to import HighPoly mesh from '{}'", high_poly_fbx);
            return {};
        }

        apply_speedtree_textures(high_asset.value());
        lods.push_back(std::move(high_asset.value()));
        MARS_LOG("[AssetImporter]   LOD 0 (Near): {} meshes, {} vertices", 
            lods[0].meshes.size(), 
            lods[0].meshes.empty() ? 0 : lods[0].meshes[0].vertices.size());
    }

    // LOD 1: LowPoly
    {
        auto low_asset = import(low_poly_fbx, /*pre_transform_vertices=*/true);
        if (!low_asset.has_value())
        {
            MARS_LOG("[AssetImporter] ERROR: Failed to import LowPoly mesh from '{}'", low_poly_fbx);
            return {};
        }

        apply_speedtree_textures(low_asset.value());
        lods.push_back(std::move(low_asset.value()));
        MARS_LOG("[AssetImporter]   LOD 1 (Mid): {} meshes, {} vertices", 
            lods[1].meshes.size(), 
            lods[1].meshes.empty() ? 0 : lods[1].meshes[0].vertices.size());
    }

    // LOD 2: FarCluster — for now, reuse LowPoly (in the future, we'd load a separate simplified mesh)
    {
        lods.push_back(lods[1]); // Copy LowPoly asset
        MARS_LOG("[AssetImporter]   LOD 2 (FarCluster): reusing LowPoly mesh");
    }

    // LOD 3: Impostor — handled separately (octahedral atlas baked offline)

    MARS_LOG("[AssetImporter] Vegetation species '{}' loaded: {} LOD tiers", species_name, lods.size());
    return lods;
}

} // namespace mars
