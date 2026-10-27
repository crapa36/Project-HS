#include "../AssetPipeline/action.hpp"
#include "../RenderMaterialShader/material_graph_parser.hpp"
#include "../Content/geometry_cooker.hpp"
#include "../Content/geometry_hierarchy_cooker.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/material_ir.hpp>
#include <hs/render_data/render_asset_format.hpp>
#include <hs/render_data/texture_package.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
namespace ap = hs::asset_pipeline;
namespace rd = hs::render_data;
namespace fs = std::filesystem;

struct PackageInput
{
    std::string logical_id;
    std::vector<std::string> dependencies;
    std::string page_id, mesh_id, hierarchy_id;
    std::vector<rd::ExpandedVertex> vertices;
    std::uint16_t lod{};
};

// The legacy HSEM source is still the current MSVC SkinnedVertex image. Keep
// that ABI assumption local to this importer; HSRPKG writes named fields.
static_assert(std::endian::native == std::endian::little &&
              std::is_trivially_copyable_v<hs::SkinnedVertex> &&
              sizeof(hs::SkinnedVertex) == rd::kExpandedVertexStride &&
              offsetof(hs::SkinnedVertex, position) == 0 &&
              offsetof(hs::SkinnedVertex, normal) == 12 &&
              offsetof(hs::SkinnedVertex, bone_indices) == 24 &&
              offsetof(hs::SkinnedVertex, bone_weights) == 32 &&
              offsetof(hs::SkinnedVertex, uv) == 48 &&
              offsetof(hs::SkinnedVertex, tangent) == 56 &&
              offsetof(hs::SkinnedVertex, material_index) == 72 &&
              offsetof(hs::SkinnedVertex, reserved) == 74);

void Require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error("hs_render_package: " + std::string(message));
}

ap::Input SemanticInput(std::string logical_id, const fs::path &dependency_path,
                        std::span<const std::byte> bytes)
{
    return ap::Input{std::move(logical_id), dependency_path, ap::Sha256Bytes(bytes)};
}

std::string StructuralMaterialJson(std::string_view source)
{
    auto root = ap::Json::parse(source);
    auto &parameters = root.at("parameters");
    if (parameters.is_object())
    {
        for (auto iterator = parameters.begin(); iterator != parameters.end(); ++iterator)
        {
            if (iterator.value().is_object())
            {
                iterator.value().erase("default");
                iterator.value().erase("default_value");
                iterator.value().erase("default_words");
            }
            else
            {
                // Legacy material JSON stores each declaration's default directly.
                iterator.value() = nullptr;
            }
        }
    }
    else if (parameters.is_array())
    {
        for (auto &parameter : parameters)
        {
            Require(parameter.is_object(), "material parameter declaration must be an object");
            parameter.erase("default");
            parameter.erase("default_value");
            parameter.erase("default_words");
        }
    }
    return root.dump();
}

ap::Input StructuralMaterialInput(const std::string &logical_id,
                                  const fs::path &material_path,
                                  std::string_view material_source)
{
    const auto structural = StructuralMaterialJson(material_source);
    return SemanticInput(logical_id + ".structural", material_path,
                         std::as_bytes(std::span<const char>(structural.data(),
                                                             structural.size())));
}

ap::Input ParameterIdentityInput(const std::string &product_id,
                                 const fs::path &result_path)
{
    return SemanticInput(product_id + ".identity", result_path,
                         std::as_bytes(std::span<const char>(product_id.data(),
                                                             product_id.size())));
}

rd::RockMaterial NeutralMaterial(const rd::RockMaterial &material)
{
    auto neutral = material;
    neutral.metallic = 0.0f;
    neutral.roughness_scale = 1.0f;
    return neutral;
}

void ValidateParameterBytes(const std::string &product_id, std::string_view bytes,
                              std::span<const rd::MaterialParameterDefinition> parameters)
{
    const auto decoded = rd::ReadMaterialParameterPackage(
        std::as_bytes(std::span(bytes.data(), bytes.size())), hs::MakeAssetId(product_id));
    rd::ValidateMaterialParameters(decoded);
    Require(rd::MaterialParameterLayoutHash(decoded) ==
                rd::MaterialParameterLayoutHash(parameters) &&
                std::ranges::equal(decoded, parameters),
            "material parameter package layout disagrees with material source");
}

struct UpstreamSource
{
    std::string name_space, product_id;
};

class UpstreamProducts
{
  public:
    explicit UpstreamProducts(const fs::path &build_root) : build_root_(build_root) {}

    ap::ActionResultSnapshot &Snapshot(std::string_view name_space)
    {
        const auto key = std::string(name_space);
        auto found = snapshots_.find(key);
        if (found == snapshots_.end())
            found = snapshots_.try_emplace(key, build_root_, key).first;
        return found->second;
    }

    ap::UpstreamProduct Identity(std::string_view name_space, std::string_view product_id)
    {
        const auto &metadata = Snapshot(name_space).ProductMetadata(product_id);
        return {std::string(product_id), metadata.at("digest").get<std::string>(),
            metadata.at("size").get<std::uint64_t>(),
            build_root_ / "Actions" / fs::path(std::string(name_space)) / "result.json"};
    }

    std::string ReadBytes(std::string_view name_space, std::string_view product_id,
                          ap::UpstreamProduct &dependency)
    {
        return Snapshot(name_space).ReadProductBytes(product_id, dependency);
    }

    void Verify(std::string_view name_space, std::string_view product_id)
    {
        ap::UpstreamProduct actual;
        fs::path path;
        Snapshot(name_space).ReadProduct(product_id, actual, path);
    }

  private:
    fs::path build_root_;
    std::map<std::string, ap::ActionResultSnapshot> snapshots_;
};

constexpr std::string_view kTerrainMaterialName = "terrain";


rd::MaterialSurfaceLayout SurfaceLayout(const rd::PreparedMaterial &prepared)
{
    const auto reflection = rd::AnalyzeClosure(prepared);
    const rd::MaterialSurfaceLayout layout{reflection.capability_mask, reflection.varying_mask};
    (void)rd::EncodeMaterialSurfaceLayout(layout);
    return layout;
}

bool MatchesSurfaceLayout(const rd::PackageView &package, rd::MaterialSurfaceLayout expected)
{
    const auto chunk = std::ranges::find(package.chunks,
        rd::ChunkType::MaterialSurfaceLayout, &rd::ChunkView::type);
    return chunk != package.chunks.end() && chunk->required && chunk->version == 1 &&
        chunk->count == 1 && chunk->stride == rd::kMaterialSurfaceLayoutStride &&
        rd::DecodeMaterialSurfaceLayout(chunk->bytes) == expected;
}

bool CurrentStandaloneMaterial(const fs::path &build_root, const ap::ActionSpec &spec,
                    std::string_view product_id,
                    std::span<const std::string> dependencies,
                    std::span<const rd::MaterialParameterDefinition> parameters,
                    rd::MaterialSurfaceLayout surface_layout)
{
    const std::vector<std::string> expected_dependencies(dependencies.begin(), dependencies.end());
    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (!fs::exists(result_path) || !fs::exists(result_path.parent_path() / "result.d"))
        return false;
    try
    {
        const ap::ActionResultSnapshot result(build_root, spec.namespace_path);
        if (result.ActionId() != spec.action_id ||
            result.ActionKeyValue() != ap::ActionKey(spec) || result.ProductCount() != 1 ||
            result.ProductMetadata(product_id).at("dependencies") != expected_dependencies)
            return false;
        ap::UpstreamProduct ignored;
        const auto contents = result.ReadProductBytes(product_id, ignored);
        const auto view = rd::ReadPackage(
            std::as_bytes(std::span(contents.data(), contents.size())),
            hs::MakeAssetId(product_id));
        if (view.dependencies != expected_dependencies || view.chunks.size() != 5 ||
            !MatchesSurfaceLayout(view, surface_layout))
            return false;
        const auto metadata = std::ranges::find(view.chunks, rd::ChunkType::MaterialMetadata,
                                                &rd::ChunkView::type);
        const auto programs = std::ranges::find(view.chunks, rd::ChunkType::MaterialProgram,
                                                &rd::ChunkView::type);
        const auto bindings = std::ranges::find(view.chunks, rd::ChunkType::MaterialBindings,
                                                &rd::ChunkView::type);
        const auto layout = std::ranges::find(view.chunks,
            rd::ChunkType::MaterialParameterLayout, &rd::ChunkView::type);
        if (metadata == view.chunks.end() || !metadata->required)
            return false;
        const auto decoded_metadata = rd::DecodeRockMaterial(metadata->bytes, view.dependencies);
        if (decoded_metadata.metallic != 0.0f || decoded_metadata.roughness_scale != 1.0f ||
            metadata->version != 1 || metadata->count != 1 ||
            metadata->stride != rd::kMaterialMetadataStride ||
            programs == view.chunks.end() || !programs->required ||
            programs->version != 1 || programs->count != 1 ||
            programs->stride != rd::kMaterialProgramStride ||
            bindings == view.chunks.end() || !bindings->required ||
            bindings->version != 1 || bindings->count != rd::kMaterialTextureSlots ||
            bindings->stride != rd::kMaterialBindingStride ||
            layout == view.chunks.end() || !layout->required ||
            layout->version != 1 || layout->count != 1 || layout->stride != 32 ||
            decoded_metadata.textures.size() != rd::kMaterialTextureSlots)
            return false;
        rd::ValidateMaterialParameterLayout(layout->bytes, parameters);
        return !rd::DecodeMaterialPrograms(programs->bytes, view.dependencies).empty() &&
               rd::DecodeMaterialBindings(bindings->bytes, view.dependencies).size() ==
                    rd::kMaterialTextureSlots;
    }
    catch (const std::exception &) { return false; }
}

void ProduceStandaloneMaterial(std::string_view name, const fs::path &source_root, const fs::path &build_root)
{
    Require(source_root.is_absolute() && build_root.is_absolute(),
            "source and build roots must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);

    const std::string product_id = "render/material/" + std::string(name) + ".rpkg";
    const auto material_relative = "ContentSource/Materials/" + std::string(name) + ".json";
    const auto material_path = source_root / material_relative;
    const auto schema_relative = "Schemas/material_graph.schema.json";
    const auto schema_path = source_root / schema_relative;
    ap::RequireRegularFile(material_path);
    ap::RequireRegularFile(schema_path);
    const auto material_text = ap::ReadBytes(material_path);
    const auto definition = hs::material_content::ParseMaterialDefinition(
        name, material_text);
    Require(definition.prepared.Graph().textures.size() == (name == kTerrainMaterialName ? rd::kMaterialTextureSlots : 3u),
            "standalone material texture count mismatch");

    ap::ActionSpec spec;
    spec.action_id = "render." + std::string(name);
    spec.recipe_id = "project-hs.render.material";
    spec.recipe_version = 4;
    spec.namespace_path = "render/" + std::string(name);
    UpstreamProducts upstream_products(build_root);
    spec.inputs.push_back(StructuralMaterialInput("material/" + std::string(name) + "/package-structure",
                                                  material_path, material_text));
    spec.inputs.push_back(ap::HashInput(schema_relative, schema_path));

    const std::string parameter_namespace = "render/material/" + std::string(name) + "/parameters";
    const std::string parameter_id = parameter_namespace + ".rpkg";
    auto parameter_dependency = upstream_products.Identity(parameter_namespace, parameter_id);
    spec.inputs.push_back(ParameterIdentityInput(parameter_id, parameter_dependency.result_path));

    std::vector<std::string> dependencies;
    dependencies.reserve(definition.prepared.Graph().textures.size() + 2);
    dependencies.push_back(parameter_id);
    for (const auto &texture : definition.prepared.Graph().textures)
    {
        const auto texture_namespace = texture.product_id.substr(
            0, texture.product_id.size() - std::string_view(".rpkg").size());
        spec.upstream.push_back(upstream_products.Identity(texture_namespace, texture.product_id));
        dependencies.push_back(texture.product_id);
    }

    const auto shader_namespace = "render/shader/" + std::string(name);
    const std::string shader_id = std::string(shader_namespace) + "_scene.dxil";
    spec.upstream.push_back(upstream_products.Identity(shader_namespace, shader_id));
    dependencies.push_back(shader_id);
    std::ranges::sort(dependencies);
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
    std::ranges::sort(spec.upstream, {}, &ap::UpstreamProduct::logical_id);
    spec.upstream.erase(std::unique(spec.upstream.begin(), spec.upstream.end(),
        [](const ap::UpstreamProduct &a, const ap::UpstreamProduct &b) {
            return a.logical_id == b.logical_id;
        }), spec.upstream.end());

    const auto surface_layout = SurfaceLayout(definition.prepared);
    if (CurrentStandaloneMaterial(build_root, spec, product_id, dependencies, definition.prepared.Graph().parameters, surface_layout))
    {
        upstream_products.Verify(parameter_namespace, parameter_id);
        for (const auto &texture : definition.prepared.Graph().textures)
            upstream_products.Verify(texture.product_id.substr(0, texture.product_id.size() - 5),
                texture.product_id);
        upstream_products.Verify(shader_namespace, shader_id);
        ap::TouchActionResult(build_root / "Actions" / spec.namespace_path / "result.json");
        std::cout << spec.action_id << " current\n";
        return;
    }

    const auto parameter_bytes = upstream_products.ReadBytes(parameter_namespace, parameter_id, parameter_dependency);
    ValidateParameterBytes(parameter_id, parameter_bytes, definition.prepared.Graph().parameters);
    for (const auto &texture : definition.prepared.Graph().textures)
    {
        ap::UpstreamProduct dependency;
        const auto contents = upstream_products.ReadBytes(
            texture.product_id.substr(0, texture.product_id.size() - 5), texture.product_id, dependency);
        const auto view = rd::ReadTexturePackage(
            std::as_bytes(std::span(contents.data(), contents.size())), hs::MakeAssetId(texture.product_id));
        rd::ValidateMaterialTexturePackage(texture, view.description, name == kTerrainMaterialName ? 3u : 0u);
    }
    upstream_products.Verify(shader_namespace, shader_id);
    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        const auto metadata = rd::EncodeRockMaterial(NeutralMaterial(definition.material), dependencies);
        // Standalone materials use one opaque scene program in the frozen material ABI;
        // visibility and resolve products remain shader action outputs.
        const std::array programs{
            rd::MaterialProgramSource{shader_id, rd::MaterialProgramKind::OpaqueScenePS}};
        const auto program_bytes = rd::EncodeMaterialPrograms(dependencies, programs);
        const auto binding_bytes = rd::EncodeMaterialBindings(
            dependencies, definition.prepared.Graph().textures);
        const auto parameter_layout = rd::EncodeMaterialParameterLayout(
            definition.prepared.Graph().parameters);
        const auto surface_bytes = rd::EncodeMaterialSurfaceLayout(surface_layout);
        const std::array chunks{
            rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialMetadata),
                            1, true, metadata, 1, rd::kMaterialMetadataStride},
            rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialProgram),
                            1, true, program_bytes,
                            program_bytes.size() / rd::kMaterialProgramStride,
                            rd::kMaterialProgramStride},
            rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialBindings),
                            1, true, binding_bytes, rd::kMaterialTextureSlots,
                            rd::kMaterialBindingStride},
            rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialParameterLayout),
                            1, true, parameter_layout, 1, 32},
            rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialSurfaceLayout),
                            1, true, surface_bytes, 1, rd::kMaterialSurfaceLayoutStride}};
        const auto package = rd::BuildPackage(hs::MakeAssetId(product_id),
                                               dependencies, chunks);
        const auto verified = rd::ReadPackage(package, hs::MakeAssetId(product_id));
        Require(verified.dependencies == dependencies && verified.chunks.size() == 5 &&
                MatchesSurfaceLayout(verified, surface_layout),
                "standalone material package self-validation failed");
        const auto verified_layout = std::ranges::find(verified.chunks,
            rd::ChunkType::MaterialParameterLayout, &rd::ChunkView::type);
        Require(verified_layout != verified.chunks.end() && verified_layout->required &&
                verified_layout->version == 1 && verified_layout->count == 1 &&
                verified_layout->stride == 32,
                "standalone parameter layout chunk missing");
        rd::ValidateMaterialParameterLayout(verified_layout->bytes,
                                            definition.prepared.Graph().parameters);
        const auto filename = fs::path(product_id).filename().string();
        const auto output = stage / filename;
        ap::WriteBytes(output, package);
        auto product = ap::HashProduct(std::string(product_id), filename, output);
        product.dependencies = dependencies;
        ap::Publish(build_root, spec, stage, {std::move(product)});
    }
    catch (...)
    {
        ap::RemoveStage(stage);
        throw;
    }
    ap::RemoveStage(stage);
    std::cout << spec.action_id << " published\n";
}

std::uint32_t U32(std::string_view bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i)
        value |= std::uint32_t(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    return value;
}

std::vector<std::uint16_t> MaterialPartitionMapping(
    std::span<const rd::ExpandedVertex> fine,
    std::span<const rd::ExpandedVertex> coarse)
{
    std::uint16_t max_entry = 0;
    for (const auto &vertex : fine) max_entry = std::max(max_entry, vertex.material_index);
    for (const auto &vertex : coarse) max_entry = std::max(max_entry, vertex.material_index);
    // Each RenderPackage action owns one MaterialDefinition. Mesh material
    // entries are shader/atlas selectors, so every entry maps to that one
    // actual material partition for geometry error measurement.
    return std::vector<std::uint16_t>(std::size_t(max_entry) + 1u, 0u);
}

ap::Input HierarchyContractInput(const fs::path &source_root, std::string_view name,
    hs::geometry_content::GeometryHierarchyCookOptions options)
{
    const std::array words{rd::kGeometryHierarchyAbi, rd::kGeometryErrorMethodAbi,
        options.target_group_size, std::bit_cast<std::uint32_t>(options.reduction_ratio),
        options.max_page_triangles, options.max_page_meshlets};
    std::array<std::byte, 24> bytes{};
    for (std::size_t word = 0; word < words.size(); ++word)
        for (std::size_t lane = 0; lane < 4; ++lane)
            bytes[word * 4 + lane] = std::byte((words[word] >> (lane * 8)) & 255u);
    return SemanticInput("render/hierarchy/" + std::string(name) + "/contract",
        source_root / "Source/Apps/Content/geometry_hierarchy_cooker.hpp", bytes);
}

bool CurrentHierarchy(const ap::ActionResultSnapshot &result,
    const PackageInput &input, const rd::PackageView &main_package,
    std::uint32_t flags, std::set<std::string> &expected_products)
{
    const auto reference = std::ranges::find(main_package.chunks,
        rd::ChunkType::GeometryHierarchyReference, &rd::ChunkView::type);
    if (reference == main_package.chunks.end() || !reference->required || reference->version != 1 ||
        reference->count != 1 || reference->stride != rd::kGeometryHierarchyReferenceStride)
        return false;
    const auto dependency_index = rd::DecodeGeometryHierarchyReference(reference->bytes,
        main_package.dependencies, input.logical_id);
    if (main_package.dependencies[dependency_index] != input.hierarchy_id) return false;
    ap::UpstreamProduct ignored;
    const auto contents = result.ReadProductBytes(input.hierarchy_id, ignored);
    const auto hierarchy = rd::DecodeGeometryHierarchy(std::as_bytes(std::span(contents.data(), contents.size())));
    if (hierarchy.geometry_flags != flags ||
        !std::ranges::all_of(hierarchy.groups, [](const auto &group) { return group.material_partition == 0; }))
        return false;
    std::vector<std::string> page_ids;
    for (const auto &page : hierarchy.pages) page_ids.push_back(page.logical_id);
    std::ranges::sort(page_ids);
    if (result.ProductMetadata(input.hierarchy_id).at("dependencies") != page_ids) return false;
    expected_products.insert(input.hierarchy_id);
    for (const auto &page : hierarchy.pages)
    {
        const auto &page_id = page.logical_id;
        const std::vector<std::string> dependencies{input.mesh_id};
        if (result.ProductMetadata(page_id).at("dependencies") != dependencies) return false;
        // The matching producer ActionKey certifies the page recipe; stream
        // its declared extent and digest without decoding the large geometry
        // payload on a cache hit.
        fs::path page_path;
        result.ReadProduct(page_id, ignored, page_path);
        expected_products.insert(page_id);
    }
    return true;
}

bool Current(const fs::path &build_root, const ap::ActionSpec &spec,
             const std::vector<PackageInput> &packages,
             std::span<const rd::MaterialParameterDefinition> parameters,
             rd::MaterialSurfaceLayout surface_layout)
{
    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (!fs::exists(result_path) || !fs::exists(result_path.parent_path() / "result.d"))
        return false;
    try
    {
        const ap::ActionResultSnapshot result(build_root, spec.namespace_path);
        if (result.ActionId() != spec.action_id ||
            result.ActionKeyValue() != ap::ActionKey(spec)) return false;
        std::set<std::string> expected_products;
        const auto action_name = std::string_view(spec.action_id).substr(7);
        const auto geometry_name = std::ranges::find(rd::kEnvironmentMaterialNames, action_name);
        if (geometry_name == rd::kEnvironmentMaterialNames.end()) return false;
        const auto geometry_ordinal = static_cast<std::size_t>(geometry_name - rd::kEnvironmentMaterialNames.begin());
        const auto material_name = rd::kAuthoredMaterialNames[rd::EnvironmentGeometryMaterialOrdinal(geometry_ordinal)];
        const auto material_product = rd::MaterialProduct(material_name);
        if (material_product.ends_with("/material.rpkg"))
        {
            ap::UpstreamProduct ignored;
            const auto material_contents = result.ReadProductBytes(material_product, ignored);
            const auto material_bytes = std::as_bytes(std::span(material_contents.data(), material_contents.size()));
            const auto material_view = rd::ReadPackage(material_bytes, hs::MakeAssetId(material_product));
            if (material_view.chunks.size() != 5 || !MatchesSurfaceLayout(material_view, surface_layout) ||
                result.ProductMetadata(material_product).at("dependencies") != material_view.dependencies) return false;
            const auto metadata = std::ranges::find(material_view.chunks,
                rd::ChunkType::MaterialMetadata, &rd::ChunkView::type);
            const auto layout = std::ranges::find(material_view.chunks,
                rd::ChunkType::MaterialParameterLayout, &rd::ChunkView::type);
            if (metadata == material_view.chunks.end() || layout == material_view.chunks.end()) return false;
            const auto material = rd::DecodeRockMaterial(metadata->bytes, material_view.dependencies);
            if (material.metallic != 0.0f || material.roughness_scale != 1.0f) return false;
            rd::ValidateMaterialParameterLayout(layout->bytes, parameters);
            expected_products.insert(material_product);
        }
        for (const auto &package : packages)
        {
            if (result.ProductMetadata(package.logical_id).at("dependencies") != package.dependencies)
                return false;
            ap::UpstreamProduct ignored;
            const auto contents = result.ReadProductBytes(package.logical_id, ignored);
            const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
            const auto view = rd::ReadPackage(bytes, hs::MakeAssetId(package.logical_id));
            if (view.chunks.size() != (package.lod == 0 ? 3u : 2u) ||
                view.dependencies != package.dependencies)
                return false;
            const auto material_reference = std::ranges::find(view.chunks,
                rd::ChunkType::GeometryMaterialReference, &rd::ChunkView::type);
            if (material_reference == view.chunks.end() || !material_reference->required ||
                view.dependencies[rd::DecodeGeometryMaterialReference(material_reference->bytes,
                    view.dependencies)] != material_product) return false;
            const auto geometry_chunk=std::ranges::find(view.chunks,rd::ChunkType::GeometryMetadata,&rd::ChunkView::type);
            if(geometry_chunk==view.chunks.end() || !geometry_chunk->required)return false;
            const auto geometry=rd::DecodeGeometryMetadata(geometry_chunk->bytes,view.dependencies,package.logical_id);
            const std::vector<std::string> page_dependencies{package.mesh_id};
            if(result.ProductMetadata(package.page_id).at("dependencies")!=page_dependencies)return false;
            fs::path page_path;
            result.ReadProduct(package.page_id,ignored,page_path);
            expected_products.insert(package.logical_id);
            expected_products.insert(package.page_id);
            if (package.lod == 0 && !CurrentHierarchy(result, package, view,
                    geometry.flags, expected_products))
                return false;
        }
        // Every expected ID was found in the snapshot's unique-ID index above.
        return result.ProductCount() == expected_products.size();
    }
    catch (const std::exception &) { return false; }
}

void Produce(std::string_view name, const fs::path &source_root,
             const fs::path &build_root, bool exhaustive_geometry_error)
{
    if (name == kTerrainMaterialName || name == "ground_detail")
    {
        Require(!exhaustive_geometry_error, "exhaustive geometry error requires an environment geometry product");
        ProduceStandaloneMaterial(name, source_root, build_root);
        return;
    }
    Require(hs::material_content::IsEnvironmentMaterialName(name),
            "unknown environment archetype");
    Require(source_root.is_absolute() && build_root.is_absolute(),
            "source and build roots must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);
    const auto geometry_ordinal = static_cast<std::uint32_t>(
        std::ranges::find(rd::kEnvironmentMaterialNames, name) - rd::kEnvironmentMaterialNames.begin());
    const auto material_name = rd::kAuthoredMaterialNames[rd::EnvironmentGeometryMaterialOrdinal(geometry_ordinal)];
    const bool masked = hs::material_content::IsMaskedEnvironmentMaterial(name);
    const auto schema_relative = "Schemas/material_graph.schema.json";
    const auto schema_path = source_root / schema_relative;
    ap::RequireRegularFile(schema_path);
    ap::ActionSpec spec;
    spec.action_id = "render." + std::string(name);
    spec.recipe_id = "project-hs.render.environment";
    spec.recipe_version = 15;
    spec.namespace_path = "render/" + std::string(name);
    spec.inputs.push_back(ap::HashInput(schema_relative, schema_path));
    UpstreamProducts upstream_products(build_root);
    std::vector<UpstreamSource> upstream_sources;
    hs::geometry_content::GeometryHierarchyCookOptions hierarchy_options;
    if (exhaustive_geometry_error)
        hierarchy_options.error_checks.mode = hs::geometry_content::GeometryErrorCheckMode::Exhaustive;
    spec.inputs.push_back(HierarchyContractInput(source_root, name, hierarchy_options));
    std::vector<PackageInput> packages;
    packages.reserve(3);
    const auto material_relative = "ContentSource/Materials/" + std::string(material_name) + ".json";
    const auto material_path = source_root / material_relative;
    const auto material_text = ap::ReadBytes(material_path);
    const auto definition = hs::material_content::ParseMaterialDefinition(material_name, material_text);
    const auto surface_layout = SurfaceLayout(definition.prepared);
    const auto material = NeutralMaterial(definition.material);
    spec.inputs.push_back(StructuralMaterialInput("material/" + std::string(material_name) +
                                                  "/package-structure",
                                                  material_path, material_text));
    const std::string parameter_id = "render/material/" + std::string(material_name) +
                                     "/parameters.rpkg";
    const auto material_product = rd::MaterialProduct(material_name);
    if (!material_product.ends_with("/material.rpkg"))
    {
        const auto material_namespace = "render/" + std::string(material_name);
        auto material_dependency = upstream_products.Identity(material_namespace, material_product);
        spec.upstream.push_back(std::move(material_dependency));
        upstream_sources.push_back({material_namespace, material_product});
    }
    const auto parameter_namespace = "render/material/" + std::string(material_name) + "/parameters";
    auto parameter_dependency = upstream_products.Identity(parameter_namespace, parameter_id);
    spec.inputs.push_back(ParameterIdentityInput(parameter_id, parameter_dependency.result_path));
    upstream_sources.push_back({parameter_namespace, parameter_id});
    std::vector<rd::TextureDescription> texture_descriptions;
    for (const auto &texture : definition.prepared.Graph().textures)
    {
        const auto texture_namespace = texture.product_id.substr(0, texture.product_id.size() - 5);
        spec.upstream.push_back(upstream_products.Identity(texture_namespace, texture.product_id));
        upstream_sources.push_back({texture_namespace, texture.product_id});
    }
    const auto shader_namespace = "render/shader/" + std::string(material_name);
    const auto scene_id = shader_namespace + "_scene.dxil";
    const auto shadow_id = shader_namespace + "_shadow.dxil";
    for (const auto &shader_id : masked ? std::vector<std::string>{scene_id, shadow_id}
                                        : std::vector<std::string>{scene_id})
    {
        spec.upstream.push_back(upstream_products.Identity(shader_namespace, shader_id));
        upstream_sources.push_back({shader_namespace, shader_id});
    }
    std::vector<std::string> material_dependencies{parameter_id, scene_id};
    if (masked) material_dependencies.push_back(shadow_id);
    for (const auto &texture : material.textures)
        if (!texture.empty()) material_dependencies.push_back(texture);
    std::ranges::sort(material_dependencies);
    material_dependencies.erase(std::unique(material_dependencies.begin(), material_dependencies.end()),
        material_dependencies.end());
    for (const auto *suffix : {"", "_lod1", "_lod2"})
    {
        const auto stem = std::string(name) + suffix;
        const auto mesh_id = "environment/environment_" + stem + ".meshbin";
        spec.upstream.push_back(upstream_products.Identity("environment/meshes", mesh_id));
        upstream_sources.push_back({"environment/meshes", mesh_id});
        PackageInput package{"render/" + stem + ".rpkg", {mesh_id}};
        package.page_id="render/geometry/"+stem+".rpage";package.mesh_id=mesh_id;
        package.lod=static_cast<std::uint16_t>(packages.size());
        if (package.lod == 0)
        {
            package.hierarchy_id = "render/hierarchy/" + std::string(name) + ".ghbin";
            package.dependencies.push_back(package.hierarchy_id);
        }
        package.dependencies.push_back(material_product);
        package.dependencies.push_back(package.page_id);
        if(package.lod<2)
        {
            package.dependencies.push_back("render/"+std::string(name)+"_lod"+std::to_string(package.lod+1)+".rpkg");
            if(package.lod==0)package.dependencies.push_back("render/"+std::string(name)+"_lod2.rpkg");
        }
        std::ranges::sort(package.dependencies);
        package.dependencies.erase(std::unique(package.dependencies.begin(),
                                               package.dependencies.end()),
                                   package.dependencies.end());
        packages.push_back(std::move(package));
    }
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
    std::ranges::sort(spec.upstream, {}, &ap::UpstreamProduct::logical_id);
    spec.upstream.erase(std::unique(spec.upstream.begin(), spec.upstream.end(),
        [](const ap::UpstreamProduct &a, const ap::UpstreamProduct &b) {
            return a.logical_id == b.logical_id;
        }), spec.upstream.end());
    std::ranges::sort(upstream_sources, [](const auto &a, const auto &b) {
        return a.product_id < b.product_id;
    });
    upstream_sources.erase(std::unique(upstream_sources.begin(), upstream_sources.end(),
        [](const auto &a, const auto &b) { return a.product_id == b.product_id; }), upstream_sources.end());
    if (!exhaustive_geometry_error && Current(build_root, spec, packages, definition.prepared.Graph().parameters, surface_layout))
    {
        for (const auto &source : upstream_sources)
            upstream_products.Verify(source.name_space, source.product_id);
        ap::TouchActionResult(build_root / "Actions" / spec.namespace_path / "result.json");
        std::cout << spec.action_id << " current\n";
        return;
    }

    // Only a cache miss needs semantic source decoding. Each verified byte
    // buffer is consumed directly so the same upstream file is not reopened.
    const auto parameter_bytes = upstream_products.ReadBytes(
        parameter_namespace, parameter_id, parameter_dependency);
    std::set<std::string> decoded_upstream_ids{parameter_id};
    ValidateParameterBytes(parameter_id, parameter_bytes,
        definition.prepared.Graph().parameters);
    texture_descriptions.clear();
    for (const auto &texture : definition.prepared.Graph().textures)
    {
        const auto texture_namespace = texture.product_id.substr(0, texture.product_id.size() - 5);
        ap::UpstreamProduct dependency;
        const auto contents = upstream_products.ReadBytes(texture_namespace, texture.product_id, dependency);
        decoded_upstream_ids.insert(texture.product_id);
        const auto view = rd::ReadTexturePackage(
            std::as_bytes(std::span(contents.data(), contents.size())), hs::MakeAssetId(texture.product_id));
        texture_descriptions.push_back(view.description);
    }
    for (auto &package : packages)
    {
        ap::UpstreamProduct dependency;
        const auto mesh = upstream_products.ReadBytes("environment/meshes", package.mesh_id, dependency);
        decoded_upstream_ids.insert(package.mesh_id);
        Require(mesh.size() >= 16 && U32(mesh, 0) == 0x4d455348 && U32(mesh, 4) == 1 &&
                U32(mesh, 8) > 0 && U32(mesh, 8) <= 3'000'000 &&
                U32(mesh, 12) == rd::kExpandedVertexStride &&
                std::uint64_t(U32(mesh, 8)) * rd::kExpandedVertexStride == mesh.size() - 16,
                "upstream mesh has an invalid vertex payload");
        package.vertices = rd::DecodeExpandedVertices(
            std::as_bytes(std::span(mesh.data() + 16, mesh.size() - 16)));
        std::uint32_t required_layer = name.starts_with("leaf_") ? 2u :
                                       name.starts_with("grass_") ? 1u : 0u;
        for (std::size_t offset = 16; offset < mesh.size(); offset += rd::kExpandedVertexStride)
        {
            const auto material_index = static_cast<unsigned char>(mesh[offset + 72]) |
                (std::uint32_t(static_cast<unsigned char>(mesh[offset + 73])) << 8);
            if (masked)
                Require(material_index < (name.starts_with("leaf_") ? 18u : 30u),
                        "foliage atlas entry exceeds the shader page mapping ABI");
            else required_layer = std::max(required_layer, material_index);
        }
        for (std::size_t slot = 0; slot < definition.prepared.Graph().textures.size(); ++slot)
            rd::ValidateMaterialTexturePackage(definition.prepared.Graph().textures[slot],
                                               texture_descriptions[slot], required_layer);
        std::vector<std::uint32_t> layer_counts;
        for (const auto &description : texture_descriptions) layer_counts.push_back(description.array_size);
        rd::ValidateMaterialSampleLayers(definition.prepared, layer_counts, required_layer);
    }
    for (const auto &source : upstream_sources)
        if (!decoded_upstream_ids.contains(source.product_id))
            upstream_products.Verify(source.name_space, source.product_id);

    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        std::vector<ap::Product> products;
        products.reserve(packages.size()*2 + 1);
        const auto geometry_flags = (masked ? rd::kGeometryFlagTwoSided : 0u) |
            (hs::HasEnvironmentWind(rd::EnvironmentMeshForOrdinal(geometry_ordinal),
                geometry_ordinal >= rd::kGroundDetailGeometryBegin ? geometry_ordinal - rd::kGroundDetailGeometryBegin : 0u)
                ? rd::kGeometryFlagWind : 0u);
        const auto hierarchy_mapping = MaterialPartitionMapping(packages.front().vertices, packages.front().vertices);
        const auto cooked_hierarchy = hs::geometry_content::CookGeometryHierarchy(
            packages.front().vertices, geometry_flags, "render/hierarchy/" + std::string(name),
            hierarchy_mapping, hierarchy_options);
        std::vector<std::string> hierarchy_dependencies;
        for (std::size_t index = 0; index < cooked_hierarchy.pages.size(); ++index)
        {
            const auto &page_id = cooked_hierarchy.hierarchy.pages[index].logical_id;
            hierarchy_dependencies.push_back(page_id);
            const std::vector<std::string> dependencies{packages.front().mesh_id};
            const auto bytes = rd::BuildGeometryPage(hs::MakeAssetId(page_id), dependencies,
                cooked_hierarchy.pages[index]);
            const auto payload = rd::DecodeGeometryPagePayload(bytes, hs::MakeAssetId(page_id), geometry_flags);
            rd::ValidateGeometryHierarchyPage(cooked_hierarchy.hierarchy, static_cast<std::uint32_t>(index), payload);
            const auto filename = fs::path(page_id).filename().string();
            const auto output = stage / filename;
            ap::WriteBytes(output, bytes);
            auto product = ap::HashProduct(page_id, filename, output);
            product.dependencies = dependencies;
            products.push_back(std::move(product));
        }
        const auto hierarchy_bytes = rd::EncodeGeometryHierarchy(cooked_hierarchy.hierarchy);
        const auto hierarchy_filename = fs::path(packages.front().hierarchy_id).filename().string();
        const auto hierarchy_output = stage / hierarchy_filename;
        ap::WriteBytes(hierarchy_output, hierarchy_bytes);
        auto hierarchy_product = ap::HashProduct(packages.front().hierarchy_id, hierarchy_filename, hierarchy_output);
        hierarchy_product.dependencies = std::move(hierarchy_dependencies);
        products.push_back(std::move(hierarchy_product));
        const auto parameter_layout = rd::EncodeMaterialParameterLayout(
            definition.prepared.Graph().parameters);
        const auto surface_bytes = rd::EncodeMaterialSurfaceLayout(surface_layout);
        if (material_product.ends_with("/material.rpkg"))
        {
            const auto material_metadata = rd::EncodeRockMaterial(material, material_dependencies);
            const auto material_program = masked
                ? rd::EncodeMaterialPrograms(material_dependencies, std::array{
                    rd::MaterialProgramSource{scene_id, rd::MaterialProgramKind::MaskedScenePS},
                    rd::MaterialProgramSource{shadow_id, rd::MaterialProgramKind::MaskedShadowPS}})
                : rd::EncodeMaterialProgram(material_dependencies, scene_id);
            const auto material_bindings = rd::EncodeMaterialBindings(material_dependencies,
                definition.prepared.Graph().textures);
            const std::array material_chunks{
                rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialMetadata),1,true,
                    material_metadata,1,rd::kMaterialMetadataStride},
                rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialProgram),1,true,
                    material_program,material_program.size()/rd::kMaterialProgramStride,rd::kMaterialProgramStride},
                rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialBindings),1,true,
                    material_bindings,rd::kMaterialTextureSlots,rd::kMaterialBindingStride},
                rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialParameterLayout),1,true,
                    parameter_layout,1,32},
                rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialSurfaceLayout),1,true,
                    surface_bytes,1,rd::kMaterialSurfaceLayoutStride}};
            const auto material_bytes = rd::BuildPackage(hs::MakeAssetId(material_product),
                material_dependencies, material_chunks);
            const auto verified_material = rd::ReadPackage(material_bytes, hs::MakeAssetId(material_product));
            Require(verified_material.dependencies == material_dependencies && verified_material.chunks.size() == 5,
                "canonical environment material package self-validation failed");
            const auto material_filename = fs::path(material_product).filename().string();
            const auto material_output = stage / material_filename;
            ap::WriteBytes(material_output, material_bytes);
            auto material_product_entry = ap::HashProduct(material_product, material_filename, material_output);
            material_product_entry.dependencies = material_dependencies;
            products.push_back(std::move(material_product_entry));
        }
        float previous_error=0;
        for (const auto &input : packages)
        {
            const auto geometry=hs::geometry_content::CookGeometry(input.vertices,
                geometry_flags);
            rd::GeometryMetadata gm;gm.flags=geometry.flags;gm.vertex_count=static_cast<std::uint32_t>(geometry.vertices.size());gm.index_count=static_cast<std::uint32_t>(geometry.indices.size());gm.submesh_count=static_cast<std::uint32_t>(geometry.submeshes.size());gm.meshlet_count=static_cast<std::uint32_t>(geometry.meshlets.size());gm.vertex_ref_count=static_cast<std::uint32_t>(geometry.meshlet_vertex_refs.size());gm.bounds=geometry.bounds;gm.lod=input.lod;
            for(const auto&v:geometry.vertices)gm.max_material_index=std::max(gm.max_material_index,std::uint32_t(v.material_index));
            const auto dep=[&](const std::string&id){const auto found=std::ranges::find(input.dependencies,id);Require(found!=input.dependencies.end(),"missing geometry dependency");return static_cast<std::uint16_t>(found-input.dependencies.begin());};
            gm.page_dependency_index=dep(input.page_id);
            if(input.lod<2){gm.fallback_dependency_index=dep("render/"+std::string(name)+"_lod"+std::to_string(input.lod+1)+".rpkg");gm.terminal_fallback_dependency_index=dep("render/"+std::string(name)+"_lod2.rpkg");}
            if(input.lod){const auto material_mapping=MaterialPartitionMapping(packages.front().vertices,input.vertices);const auto error=hs::geometry_content::MeasureGeometryError(packages.front().vertices,input.vertices,material_mapping,4096,hierarchy_options.error_checks);gm.geometric_error=std::max(previous_error,std::nextafter(static_cast<float>(error.upper),std::numeric_limits<float>::infinity()));previous_error=gm.geometric_error;}
            const auto geometry_metadata=rd::EncodeGeometryMetadata(gm,input.dependencies,input.logical_id);
            const std::vector<std::string>page_dependencies{input.mesh_id};
            const auto page=rd::BuildGeometryPage(hs::MakeAssetId(input.page_id),page_dependencies,geometry);
            (void)rd::DecodeGeometryPage(page,hs::MakeAssetId(input.page_id),gm);
            const auto page_filename=fs::path(input.page_id).filename().string();const auto page_output=stage/page_filename;ap::WriteBytes(page_output,page);auto page_product=ap::HashProduct(input.page_id,page_filename,page_output);page_product.dependencies=page_dependencies;products.push_back(std::move(page_product));
            const auto material_reference = rd::EncodeGeometryMaterialReference(dep(material_product),
                input.dependencies);
            std::vector<rd::ChunkSource> chunks{
                rd::ChunkSource{13,1,true,geometry_metadata,1,rd::kGeometryMetadataStride},
                rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::GeometryMaterialReference),1,true,
                    material_reference,1,rd::kGeometryMaterialReferenceStride}};
            std::vector<std::byte> hierarchy_reference;
            if (input.lod == 0)
            {
                hierarchy_reference = rd::EncodeGeometryHierarchyReference(dep(input.hierarchy_id),
                    input.dependencies, input.logical_id);
                chunks.push_back({static_cast<std::uint32_t>(rd::ChunkType::GeometryHierarchyReference),
                    1, true, hierarchy_reference, 1, rd::kGeometryHierarchyReferenceStride});
            }
            const auto package = rd::BuildPackage(hs::MakeAssetId(input.logical_id),
                                                  input.dependencies, chunks);
            const auto verified = rd::ReadPackage(package, hs::MakeAssetId(input.logical_id));
            Require(verified.chunks.size() == chunks.size() &&
                    verified.dependencies == input.dependencies,
                    "render package self-validation failed");
            const auto reference = std::ranges::find(verified.chunks,
                rd::ChunkType::GeometryMaterialReference, &rd::ChunkView::type);
            Require(reference != verified.chunks.end() && rd::DecodeGeometryMaterialReference(
                reference->bytes, verified.dependencies) == dep(material_product),
                "render package material reference self-validation failed");
            const auto filename = fs::path(input.logical_id).filename().string();
            const auto output = stage / filename;
            ap::WriteBytes(output, package);
            auto product = ap::HashProduct(input.logical_id, filename, output);
            product.dependencies = input.dependencies;
            products.push_back(std::move(product));
        }
        ap::Publish(build_root, spec, stage, std::move(products));
    }
    catch (...)
    {
        ap::RemoveStage(stage);
        throw;
    }
    ap::RemoveStage(stage);
    std::cout << spec.action_id << " published\n";
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    fs::path source_root, build_root;
    std::string name;
    bool exhaustive_geometry_error{};
    ap::Json action_id = nullptr;
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg = argv[i];
            if (arg == L"--source-root" && source_root.empty() && ++i < argc)
                source_root = fs::path(argv[i]);
            else if (arg == L"--build-root" && build_root.empty() && ++i < argc)
                build_root = fs::path(argv[i]);
            else if (arg == L"--name" && name.empty() && ++i < argc)
            {
                const std::wstring_view value = argv[i];
                Require(std::ranges::all_of(value, [](wchar_t character) {
                    return (character >= L'a' && character <= L'z') ||
                           (character >= L'0' && character <= L'9') ||
                           character == L'_';
                }), "invalid environment archetype token");
                for (const wchar_t character : value)
                    name.push_back(static_cast<char>(character));
            }
            else if (arg == L"--exhaustive-geometry-error" && !exhaustive_geometry_error)
                exhaustive_geometry_error = true;
            else
                throw std::runtime_error("hs_render_package: invalid command line");
        }
        Require(!name.empty() && !source_root.empty() && !build_root.empty(),
                "usage: hs_render_package --name <archetype> --source-root <absolute> --build-root <absolute> [--exhaustive-geometry-error]");
        if (name == kTerrainMaterialName || name == "ground_detail")
            action_id = "render." + name;
        else if (hs::material_content::IsEnvironmentMaterialName(name))
            action_id = "render." + name;
        Produce(name, source_root.lexically_normal(), build_root.lexically_normal(), exhaustive_geometry_error);
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_render_package", error.what(),
                          {{"action_id", action_id}, {"name", name},
                           {"source_root", ap::DiagnosticPath(source_root)},
                           {"build_root", ap::DiagnosticPath(build_root)}});
        return 1;
    }
}
