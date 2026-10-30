#include "renderer_impl.hpp"
#include "renderer_cluster_catalog.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/render_data/render_asset_format.hpp>

#include <cstddef>
#include <bit>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace hs
{
Result D3D12Renderer::Impl::CreateEnvironmentMeshes()
{
    std::array<std::uint32_t,
        kEnvironmentMaterialNames.size() * render_data::kEnvironmentGeometryLodCount> page_indices{};
    for (std::size_t index = 0; index < page_indices.size(); ++index)
        page_indices[index] = static_cast<std::uint32_t>(index);
    if (auto loaded = CreateEnvironmentMeshes(page_indices,
        EnvironmentMaterialResidency::RuntimeFallback); !loaded) return loaded;
    return PrepareEnvironmentGeometryStreaming();
}

Result D3D12Renderer::Impl::CreateEnvironmentMeshes(
    std::span<const std::uint32_t> selected_page_indices, EnvironmentMaterialResidency material_residency)
{
    static_assert(sizeof(Vertex) == 76 &&
                  offsetof(Vertex, position) == 0 &&
                  offsetof(Vertex, normal) == 12 &&
                  offsetof(Vertex, bone_indices) == 24 &&
                  offsetof(Vertex, bone_weights) == 32 &&
                  offsetof(Vertex, uv) == 48 &&
                  offsetof(Vertex, tangent) == 56 &&
                  offsetof(Vertex, material_index) == 72 &&
                  offsetof(Vertex, reserved) == 74);
    auto result = frames[0].allocator->Reset();
    if (FAILED(result)) return HResultFailure("Reset mesh upload allocator", result);
    result = command_list->Reset(frames[0].allocator.Get(), nullptr);
    if (FAILED(result)) return HResultFailure("Reset mesh upload list", result);
    std::vector<AllocationResource> uploads;
    std::array<bool, kAuthoredMaterialNames.size()> material_bindings_initialized{};
    const auto validate_capture_binding = [&](const render_data::MaterialTextureDefinition &binding,
        std::uint32_t layer, render_data::TextureHandle compiled_texture) -> Result {
        if (!config.development_catalog || !RendererIsAdmittedRuntimeProduct(config, binding.product_id))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Capture material texture is absent from the admitted Generation: " + binding.product_id);
        if (compiled_texture && (compiled_texture.value >= config.packed_textures.size() ||
            config.packed_textures[compiled_texture.value].asset_id.value != MakeAssetId(binding.product_id).value))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Capture material texture handle differs from its admitted product: " + binding.product_id);
        try
        {
            const auto descriptor = config.development_catalog->ProveTexture(MakeAssetId(binding.product_id));
            if (descriptor.dimension != render_data::TextureDimension::Texture2DArray)
                throw std::runtime_error("capture material texture is not a 2D array");
            for (const auto dependency : descriptor.dependencies)
            {
                if (!config.development_catalog->Contains(dependency))
                    throw std::runtime_error("capture material texture dependency is absent");
                if (!RendererIsSourceRevision(config, dependency) &&
                    !config.development_catalog->HasRuntimeProduct(dependency))
                    throw std::runtime_error("capture material texture dependency is not a runtime product");
            }
            render_data::ValidateMaterialTexturePackage(binding, descriptor.description, layer);
        }
        catch (const std::exception &error)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Invalid capture material texture " + binding.product_id + ": " + error.what());
        }
        return Result::Success();
    };
    const auto load_compiled_material = [&](std::uint32_t ordinal, std::uint32_t layer,
        render_data::RockMaterial &material, EnvironmentMaterialBindingsGpu &bindings) -> Result {
        const std::string name(kAuthoredMaterialNames[ordinal]);
        const auto id = render_data::MaterialProduct(name);
        const auto handle = FindRendererMaterial(config, MakeAssetId(id));
        if (!handle) throw std::runtime_error("Compiled environment material is missing");
        const auto &descriptor = config.packed_materials[handle.value];
        if (descriptor.kind != render_data::MaterialKind::Environment ||
            descriptor.surface != material_surface_layouts[ordinal] || MaterialLayoutDigest(descriptor) != material_parameter_layouts[ordinal])
            throw std::runtime_error("Compiled environment material layout differs from its resident profile");
        material.metallic = descriptor.metallic; material.roughness_scale = descriptor.roughness_scale;
        material.two_sided = (descriptor.flags & render_data::kMaterialFlagTwoSided) != 0;
        material.alpha_mask = (descriptor.flags & render_data::kMaterialFlagAlphaMask) != 0;
        if (ordinal == kTerrainMaterialOrdinal && (material.two_sided || material.alpha_mask || descriptor.textures.size() != 6))
            throw std::runtime_error("Compiled terrain requires opaque single-sided material and six textures");
        for (const auto &texture : descriptor.textures)
        {
            const render_data::MaterialTextureDefinition definition{"slot_" + std::to_string(texture.slot),
                std::string(texture.product_id), texture.dimension, texture.semantic};
            if (material_residency == EnvironmentMaterialResidency::CaptureOnly)
            {
                if (auto checked = validate_capture_binding(definition, layer, texture.texture); !checked) return checked;
            }
            else if (auto loaded = LoadEnvironmentMaterialTexture(definition, layer, uploads,
                bindings.texture_indices[texture.slot], texture.texture); !loaded) return loaded;
            bindings.product_ids[texture.slot] = texture.product_id;
            material.textures[texture.slot] = texture.product_id;
        }
        return Result::Success();
    };
    for (const auto selected_index : selected_page_indices)
    {
        const auto index = static_cast<std::size_t>(selected_index);
        const std::string mesh_name = std::string(kEnvironmentMaterialNames[index/3]) +
            (index%3 == 0 ? std::string{} : "_lod" + std::to_string(index%3));
        const auto geometry_ordinal = static_cast<std::uint32_t>(index / 3);
        const auto material_ordinal = render_data::EnvironmentGeometryMaterialOrdinal(geometry_ordinal);
        const std::string material_name(kAuthoredMaterialNames[material_ordinal]);
        const std::string logical_id = "render/" + mesh_name + ".rpkg";
        const std::string mesh_dependency =
            "environment/environment_" + mesh_name + ".meshbin";
        RendererAssetSource path;
        if (auto resolved = ResolveRendererAsset(config, logical_id, path); !resolved)
            return resolved;
        std::vector<std::byte> bytes;
        if (auto read = ReadBinary(path, bytes); !read) return read;
        render_data::GeometryMetadata geometry_metadata;
        std::string page_id;
        RendererAssetSource page_path;
        std::uint32_t fallback_page_slot = UINT32_MAX;
        try
        {
            const auto package = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate,
                [&] { return render_data::ReadPackage(bytes, MakeAssetId(logical_id)); });
            const render_data::ChunkView *geometry_chunk = nullptr;
            const render_data::ChunkView *material_reference = nullptr;
            const render_data::ChunkView *hierarchy_chunk = nullptr;
            for (const auto &chunk : package.chunks)
            {
                if (chunk.type == render_data::ChunkType::GeometryMetadata)
                    geometry_chunk = &chunk;
                else if (chunk.type == render_data::ChunkType::GeometryMaterialReference)
                    material_reference = &chunk;
                else if (chunk.type == render_data::ChunkType::GeometryHierarchyReference)
                    hierarchy_chunk = &chunk;
            }
            const std::string name(kEnvironmentMaterialNames[index / 3]);
            const bool masked = name.starts_with("leaf_") || name.starts_with("grass_");
            if (package.chunks.size() != (index % 3 == 0 ? 3u : 2u) || !geometry_chunk ||
                !geometry_chunk->required || geometry_chunk->version != 1 ||
                geometry_chunk->stride != render_data::kGeometryMetadataStride || geometry_chunk->count != 1 ||
                !material_reference || !material_reference->required || material_reference->version != 1 ||
                material_reference->stride != render_data::kGeometryMaterialReferenceStride ||
                (index % 3 == 0 && (!hierarchy_chunk || !hierarchy_chunk->required ||
                    hierarchy_chunk->version != 1 || hierarchy_chunk->count != 1 ||
                    hierarchy_chunk->stride != render_data::kGeometryHierarchyReferenceStride)) ||
                (index % 3 != 0 && hierarchy_chunk))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Invalid geometry package chunks: " + path.path.string());
            const auto material_dependency = render_data::DecodeGeometryMaterialReference(
                material_reference->bytes, package.dependencies);
            const auto expected_material_product = render_data::MaterialProduct(material_name);
            if (package.dependencies[material_dependency] != expected_material_product)
                throw std::runtime_error("Environment geometry references a different canonical material product");
            for (const auto &dependency : package.dependencies)
            {
                if (RendererIsSourceRevision(config, dependency))
                {
                    if (RendererProductDigest(config, dependency).empty())
                        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                            "Geometry package references an unauthenticated source revision " + dependency);
                    continue;
                }
                if (!RendererIsAdmittedRuntimeProduct(config, dependency))
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                        "Geometry package dependency is absent from runtime and provenance catalogs: " + dependency);
                RendererAssetSource dependency_path;
                if (config.packed_content ? RendererProductDigest(config, dependency).empty() :
                    !ResolveRendererAsset(config, dependency, dependency_path))
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                           "Invalid render package dependency " + dependency +
                                               ": " + path.path.string());
            }
            if (std::ranges::find(package.dependencies, mesh_dependency) ==
                package.dependencies.end())
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Render package omits mesh dependency " + mesh_dependency +
                                           ": " + path.path.string());
            geometry_metadata = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::DecodeGeometryMetadata(
                geometry_chunk->bytes, package.dependencies, logical_id); });
            if (geometry_metadata.lod != index % 3 ||
                geometry_metadata.page_dependency_index == render_data::kNoGeometryDependency)
                throw std::runtime_error("environment geometry metadata has invalid LOD/page identity");
            if (index % 3 == 0)
            {
                if (auto loaded = environment_cluster_catalogs[index / 3].Load(config, package, logical_id,
                        [](const RendererAssetSource &input, std::vector<std::byte> &output) {
                            return ReadBinary(input, output);
                        }); !loaded)
                    return loaded;
            }
            page_id = package.dependencies[geometry_metadata.page_dependency_index];
            if (auto resolved = ResolveRendererAsset(config, page_id, page_path); !resolved)
                return resolved;
            if (geometry_metadata.fallback_dependency_index != render_data::kNoGeometryDependency)
            {
                const auto &fallback_id = package.dependencies[geometry_metadata.fallback_dependency_index];
                for (std::size_t candidate = 0; candidate < environment_meshes.size(); ++candidate)
                {
                    const std::string candidate_name = std::string(kEnvironmentMaterialNames[candidate / 3]) +
                        (candidate % 3 == 0 ? std::string{} : "_lod" + std::to_string(candidate % 3));
                    if (fallback_id == "render/" + candidate_name + ".rpkg")
                        fallback_page_slot = static_cast<std::uint32_t>(candidate);
                }
                if (fallback_page_slot == UINT32_MAX)
                    throw std::runtime_error("environment fallback metadata has no geometry page slot");
            }
            auto &material_bindings = environment_material_bindings[material_ordinal];
            const std::uint32_t required_layer = material_ordinal == kTerrainMaterialOrdinal
                ? std::max(3u, geometry_metadata.max_material_index)
                : masked ? (name.starts_with("grass_") ? 1u : 2u) : geometry_metadata.max_material_index;
            if (!material_bindings_initialized[material_ordinal])
            {
                render_data::RockMaterial packaged_material;
                EnvironmentMaterialBindingsGpu selected_bindings;
                if (config.packed_content || config.development_catalog)
                {
                    if (auto loaded = load_compiled_material(material_ordinal, required_layer,
                        packaged_material, selected_bindings); !loaded) return loaded;
                }
                else
                {
                    const auto &material_package = material_headers[material_ordinal].view;
                    const auto find_chunk = [&](render_data::ChunkType type) -> const render_data::ChunkView & {
                        const auto found = std::ranges::find(material_package.chunks, type, &render_data::ChunkView::type);
                        if (found == material_package.chunks.end() || !found->required)
                            throw std::runtime_error("Canonical environment material chunk is missing");
                        return *found;
                    };
                    packaged_material = render_data::DecodeRockMaterial(
                        find_chunk(render_data::ChunkType::MaterialMetadata).bytes, material_package.dependencies);
                    const auto programs = render_data::DecodeMaterialPrograms(
                        find_chunk(render_data::ChunkType::MaterialProgram).bytes, material_package.dependencies);
                    const auto scene_id = "render/shader/" + material_name + "_scene.dxil";
                    const auto shadow_id = "render/shader/" + material_name + "_shadow.dxil";
                    if (programs.size() != (masked ? 2u : 1u) ||
                        programs[0].kind != (masked ? render_data::MaterialProgramKind::MaskedScenePS :
                            render_data::MaterialProgramKind::OpaqueScenePS) ||
                        material_package.dependencies[programs[0].dependency_index] != scene_id ||
                        (masked && (programs[1].kind != render_data::MaterialProgramKind::MaskedShadowPS ||
                            material_package.dependencies[programs[1].dependency_index] != shadow_id)))
                        throw std::runtime_error("Canonical environment material program identity is invalid");
                    const auto bindings = render_data::DecodeMaterialBindings(
                        find_chunk(render_data::ChunkType::MaterialBindings).bytes, material_package.dependencies);
                    for (const auto &binding : bindings)
                    {
                        const auto slot = binding.slot;
                        if (binding.dependency_index == 0xffff)
                        {
                            if (!packaged_material.textures[slot].empty())
                                throw std::runtime_error("material metadata declares an absent texture binding");
                            continue;
                        }
                        const auto &texture_id = material_package.dependencies[binding.dependency_index];
                        if (packaged_material.textures[slot] != texture_id)
                            throw std::runtime_error("material metadata and typed texture binding disagree");
                        const render_data::MaterialTextureDefinition definition{
                            "slot_" + std::to_string(slot), texture_id, binding.dimension, binding.semantic};
                        if (material_residency == EnvironmentMaterialResidency::CaptureOnly)
                        {
                            if (auto checked = validate_capture_binding(definition, required_layer, {}); !checked) return checked;
                        }
                        else if (auto loaded = LoadEnvironmentMaterialTexture(definition, required_layer, uploads,
                            selected_bindings.texture_indices[slot]); !loaded) return loaded;
                        selected_bindings.product_ids[slot] = texture_id;
                    }
                }
                const auto variant = render_data::ClassifyEnvironmentMaterial(packaged_material);
                if (material_ordinal == kTerrainMaterialOrdinal &&
                    std::ranges::any_of(packaged_material.textures, [](const auto &id) { return id.empty(); }))
                    throw std::runtime_error("Terrain requires all six canonical texture bindings");
                if ((variant ==
                    render_data::EnvironmentMaterialVariant::MaskedTwoSided) != masked)
                    throw std::runtime_error("Canonical environment material program domain mismatch");
                environment_material_variants[material_ordinal] = variant;
                material_bindings = std::move(selected_bindings);
                material_bindings_initialized[material_ordinal] = true;
            }
            // Every geometry selector must fit, including later meshes sharing this material.
            for (std::size_t slot = 0; slot < material_bindings.texture_indices.size(); ++slot)
            {
                const auto texture_index = material_bindings.texture_indices[slot];
                if (texture_index == UINT32_MAX)
                {
                    if (material_residency != EnvironmentMaterialResidency::CaptureOnly ||
                        material_bindings.product_ids[slot].empty()) continue;
                    try
                    {
                        const auto description = config.development_catalog->ProveTexture(
                            MakeAssetId(material_bindings.product_ids[slot])).description;
                        if (required_layer >= description.array_size)
                            throw std::runtime_error("selector exceeds admitted texture array");
                    }
                    catch (const std::exception &error)
                    {
                        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                            "Invalid capture geometry selector for " + material_bindings.product_ids[slot] +
                                ": " + error.what());
                    }
                    continue;
                }
                const auto &description = texture_index < kEnvironmentTextureCount
                    ? environment_texture_descriptions[texture_index]
                    : environment_material_textures.at(texture_index - kEnvironmentTextureCount).description;
                if (required_layer >= description.array_size)
                    throw std::runtime_error("Environment geometry selector exceeds its canonical material texture");
            }
        }
        catch (const std::exception &error)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Invalid render package " + path.path.string() + ": " + error.what());
        }
        auto &mesh = environment_meshes[index];
        const auto &sphere = geometry_metadata.bounds.sphere;
        mesh.bounding_radius = std::sqrt(sphere[0] * sphere[0] + sphere[1] * sphere[1] +
                                         sphere[2] * sphere[2]) + sphere[3];
        environment_variant_radii[index / 3] = std::max(
            environment_variant_radii[index / 3], mesh.bounding_radius);
        mesh.metadata = geometry_metadata;
        mesh.page_slot = static_cast<std::uint32_t>(index);
        mesh.fallback_page_slot = fallback_page_slot;
        mesh.vertex_count = geometry_metadata.vertex_count;
        mesh.index_count = geometry_metadata.index_count;
        mesh.meshlet_count = geometry_metadata.meshlet_count;
        if (index % 3 == 0)
        {
            auto &stream = environment_geometry_streams[index / 3];
            stream.source = page_path;
            stream.development_catalog = resident_development_catalog;
            stream.logical_id = page_id;
            stream.source_id = mesh_dependency;
            stream.metadata = geometry_metadata;
            if (config.packed_content)
            {
                stream.compiled_geometry = FindRendererGeometry(config, MakeAssetId(page_id));
                if (!stream.compiled_geometry)
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Compiled streamed geometry is absent: " + page_id);
            }
            stream.target_index = static_cast<std::uint32_t>(index);
            stream.fallback_index = fallback_page_slot;
        }
        // Startup never opens a LOD0 payload; its selected resident fallback is complete.
        if (index % 3 == 0) continue;
        std::vector<std::byte> page_bytes;
        if (auto read = ReadBinary(page_path, page_bytes); !read) return read;
        PreparedGeometryUpload preparation;
        try
        {
            if (auto prepared = PrepareReadGeometry(config, page_id, page_bytes, geometry_metadata, preparation); !prepared)
                return prepared;
            const auto source_matches = preparation.descriptor ?
                preparation.descriptor->dependencies.size() == 1 &&
                    preparation.descriptor->dependencies[0].value == MakeAssetId(mesh_dependency).value :
                preparation.page.package.dependencies == std::vector<std::string>{mesh_dependency};
            if (!source_matches)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Environment geometry page has an unexpected source dependency: " + mesh_dependency);
        }
        catch (const std::exception &error)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Invalid environment geometry page " + page_path.path.string() + ": " +
                                       error.what());
        }
        if (auto created = MeasureStartupOperation(RendererStartupOperation::GeometryPrepare, [&] { return CreateGeometryPage(mesh, geometry_metadata, page_bytes,
                MakeAssetId(page_id), static_cast<std::uint32_t>(index), command_list.Get(), uploads, &preparation); });
            !created) return created;
    }
    result = command_list->Close();
    if (FAILED(result)) return HResultFailure("Close mesh upload", result);
    if (auto waited = SubmitResidentUtilityUpload(command_list.Get(), frames[0].allocator.Get(), uploads); !waited)
        return waited;
    if (material_residency == EnvironmentMaterialResidency::RuntimeFallback)
        for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
        {
            PublishEnvironmentMaterialTextures(frame);
            WriteGeometryPageDescriptors(frame);
        }
    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareEnvironmentGeometryStreaming()
{
    if (resource_generation_id.empty())
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Geometry streaming requires the selected content Generation ID.");
    for (std::size_t ordinal = 0; ordinal < environment_geometry_streams.size(); ++ordinal)
    {
        const auto &stream = environment_geometry_streams[ordinal];
        if (stream.source.empty() || stream.logical_id.empty() || stream.source_id.empty() ||
            stream.metadata.lod != 0 || stream.target_index != ordinal * 3 ||
            stream.fallback_index != ordinal * 3 + 1 ||
            !FindGeometryPage(stream.fallback_index) ||
            !FindGeometryPage(static_cast<std::uint32_t>(ordinal * 3 + 2)))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Environment geometry streaming catalog is incomplete.");
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::DemandEnvironmentGeometry(std::uint32_t ordinal,
                                                     std::uint32_t priority)
{
    if (ordinal >= environment_geometry_streams.size())
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Environment geometry demand ordinal is out of range.");
    auto &stream = environment_geometry_streams[ordinal];
    const auto error = std::bit_cast<float>(priority);
    if (!std::isfinite(error) || error <= 0.0f)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Geometry demand requires positive ordered error.");
    const auto demand_frame = rendered_submission_count + 1u;
    if (stream.last_demand_frame != demand_frame) stream.priority = 0u;
    stream.priority = std::max(stream.priority, priority);
    stream.last_demand_frame = demand_frame;
    if (stream.request && !stream.retiring) static_cast<void>(streaming.UpdatePriority(stream.request, stream.priority));
    if (resource_generation_id.empty() || stream.source.empty() || stream.logical_id.empty() ||
        stream.target_index >= environment_meshes.size())
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Geometry demand requires a selected content catalog.");
    if (stream.request || stream.retiring || environment_meshes[stream.target_index].storage.resource)
        return Result::Success();
    stream.content_generation = resource_generation_id;
    stream.request = streaming.Request(
        stream.logical_id,
        stream.logical_id, stream.content_generation, stream.priority);
    if (!stream.request)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Could not create environment geometry streaming request.");
    ++stream.requests;
    return Result::Success();
}

Result D3D12Renderer::Impl::PumpEnvironmentGeometryStreaming()
{
    if (auto budget = UpdateResidencyBudget(); !budget) return budget;
    const auto completed = graph_queue_executor.Completed();
    static_cast<void>(residency.Collect(completed));
    static_cast<void>(streaming.Collect(completed));

    std::array<std::size_t, kEnvironmentMaterialNames.size()> order{};
    for (std::size_t ordinal = 0; ordinal < order.size(); ++ordinal)
    {
        order[ordinal] = ordinal;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        const auto left_priority = environment_geometry_streams[left].priority;
        const auto right_priority = environment_geometry_streams[right].priority;
        return left_priority == right_priority ? left < right : left_priority > right_priority;
    });
    const auto pump_page = [&](EnvironmentGeometryStream &stream) -> Result {
        if (stream.retiring)
        {
            if (!stream.retirement.IsSatisfiedBy(completed)) return Result::Success();
            static_cast<void>(residency.ReleaseNow(stream.candidate.residency));
            stream.candidate.Reset(); stream.request = {}; stream.retiring = false;
            stream.retirement = {}; ++stream.reclaims;
        }
        if (!stream.request) return Result::Success();
        if (stream.upload_submitted && stream.upload_completion.IsSatisfiedBy(completed))
        {
            if (const auto *page = streaming.Find(stream.request);
                page && page->state == StreamingPageState::Uploading)
            {
                if (!streaming.TryPublish(stream.request, resource_generation_id, completed))
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                           "Completed geometry upload could not be published.");
            }
            if (stream.upload.resource)
            {
                stream.upload.Reset();
                static_cast<void>(residency.ReleaseNow(stream.upload_residency));
                stream.upload_residency = {};
                ++stream.reclaims;
            }
            stream.upload_submitted = false;
        }

        if (const auto *page = streaming.Find(stream.request);
            page && page->state == StreamingPageState::Resident &&
            stream.candidate.storage.resource)
        {
            if (stream.target_index >= environment_meshes.size())
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Streamed geometry target index is out of range.");
            auto &mesh = environment_meshes[stream.target_index];
            // Each fallback keeps its own allocation. Per-frame descriptor tables
            // publish this immutable finer page only after its upload completes.
            static_cast<GeometryPageGpu &>(mesh) = std::move(stream.candidate);
            stream.candidate.Reset();
            stream.generation = mesh.generation;
            ++stream.publications;
        }

        const auto *page = streaming.Find(stream.request);
        if (!page) return Result::Success();
        if (page->state == StreamingPageState::Requested)
        {
            if (stream.last_demand_frame < rendered_submission_count) return Result::Success();
            std::uint64_t file_bytes{};
            if (auto sized = stream.source.ByteCount(file_bytes); !sized) return sized;
            const auto peak_factor = stream.compiled_geometry ? 1u : 3u;
            if (!file_bytes || file_bytes > UINT64_MAX / peak_factor ||
                file_bytes > std::numeric_limits<std::size_t>::max() ||
                file_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Geometry stream read size overflow.");
            if (!residency.CanStage(file_bytes * peak_factor)) return Result::Success();
            stream.decoded_residency = residency.ReserveHost(file_bytes);
            if (!stream.compiled_geometry) stream.decode_reservation = residency.ReserveHost(file_bytes * 2u);
            if (!stream.decoded_residency || (!stream.compiled_geometry && !stream.decode_reservation))
            {
                static_cast<void>(residency.ReleaseNow(stream.decoded_residency));
                static_cast<void>(residency.ReleaseNow(stream.decode_reservation));
                stream.decoded_residency = {}; stream.decode_reservation = {};
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Geometry stream host reservation failed.");
            }
            stream.read_bytes = file_bytes;
            const auto logical_id = stream.logical_id;
            const auto source_id = stream.source_id;
            const auto metadata = stream.metadata;
            stream.preparation = std::make_shared<PreparedGeometryUpload>();
            const auto preparation = stream.preparation;
            const auto descriptor = stream.compiled_geometry ? &config.packed_geometry[stream.compiled_geometry.value] : nullptr;
            const auto development_catalog = stream.development_catalog;
            const auto verified_digest = stream.source.pack ?
                std::optional(stream.source.range.expected_digest) : std::nullopt;
            bool submitted{};
            const auto queued = asset_io.Enqueue({stream.request, stream.content_generation,
                    stream.logical_id, stream.source, file_bytes, resident_generation_lease,
                    [logical_id, source_id, metadata, preparation, verified_digest, descriptor, development_catalog](std::span<const std::byte> bytes) {
                        try
                        {
                            preparation->source_product = logical_id;
                            if (descriptor)
                            {
                                render_data::ValidateGeometryDescriptorMetadata(*descriptor, metadata);
                                if (descriptor->dependencies.size() != 1 ||
                                    descriptor->dependencies[0].value != MakeAssetId(source_id).value)
                                    throw std::runtime_error("Compiled streamed geometry source dependency differs.");
                                preparation->descriptor = descriptor;
                            }
                            else if (development_catalog &&
                                (preparation->descriptor = development_catalog->FindGeometry(MakeAssetId(logical_id))))
                            {
                                render_data::ValidateGeometryDescriptorMetadata(*preparation->descriptor, metadata);
                                if (preparation->descriptor->dependencies.size() != 1 ||
                                    preparation->descriptor->dependencies[0].value != MakeAssetId(source_id).value)
                                    throw std::runtime_error("Admitted streamed geometry source dependency differs.");
                            }
                            else
                            {
                                preparation->page = render_data::PrepareGeometryPage(bytes, MakeAssetId(logical_id), metadata);
                                if (preparation->page.package.dependencies != std::vector<std::string>{source_id})
                                    throw std::runtime_error("Streamed geometry source dependency differs.");
                                if (development_catalog)
                                {
                                    preparation->descriptor = &development_catalog->AdmitGeometry(
                                        MakeAssetId(logical_id), bytes, preparation->page);
                                    render_data::ValidateGeometryDescriptorMetadata(*preparation->descriptor, metadata);
                                }
                            }
                            preparation->digest = verified_digest ? *verified_digest : render_data::Sha256(bytes);
                        }
                        catch (const std::exception &error)
                        {
                            throw std::runtime_error("Environment geometry stream read/decode failed for " +
                                logical_id + ": " + error.what());
                        }
                    }},
                    streaming, residency, stream.decoded_residency, submitted);
            if (!submitted)
            {
                stream.preparation.reset();
                static_cast<void>(residency.ReleaseNow(stream.decoded_residency));
                static_cast<void>(residency.ReleaseNow(stream.decode_reservation));
                stream.decode_reservation = {};
            }
            stream.decoded_residency = {};
            if (!queued) return queued;
            return Result::Success();
        }
        if (page->state == StreamingPageState::Reading)
        {
            AssetReadCompletion read;
            if (auto result = asset_io.Poll(stream.request, streaming, residency, read); !result) return result;
            if (!read.ready || read.cancelled)
            {
                if (read.ready) stream.preparation.reset();
                return Result::Success();
            }
            stream.decoded_storage = std::move(read.bytes);
            stream.decoded_residency = read.storage;
            if (!stream.preparation)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Environment geometry decode result owner is missing.");
            if (!streaming.CompleteRead(stream.request, resource_generation_id))
            {
                std::vector<std::byte>{}.swap(stream.decoded_storage);
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Geometry read completed for a stale content Generation.");
            }
            page = streaming.Find(stream.request);
        }
        if (!page || page->state != StreamingPageState::Decoded)
            return Result::Success();
        if (stream.last_demand_frame < rendered_submission_count) return Result::Success();

        const auto &metadata = stream.metadata;
        const std::array<std::uint64_t, kGeometryBuffersPerPage> sizes{
            static_cast<std::uint64_t>(metadata.vertex_count) * sizeof(Vertex),
            static_cast<std::uint64_t>(metadata.index_count) * sizeof(std::uint32_t),
            static_cast<std::uint64_t>(metadata.meshlet_count) * render_data::kGeometryMeshletStride,
            static_cast<std::uint64_t>(metadata.vertex_ref_count) * sizeof(std::uint32_t) +
                static_cast<std::uint64_t>(metadata.index_count / 3) * 2 * sizeof(std::uint32_t)};
        std::uint64_t upload_size{}; std::array<std::uint64_t, 4> offsets{};
        if (!GeometryPageLayout(sizes, offsets, upload_size))
            return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Streamed geometry allocation size overflow.");
        const auto resident_description = BufferDescription(upload_size);
        const auto resident_size = device->GetResourceAllocationInfo(0, 1, &resident_description).SizeInBytes;
        const auto upload_description = BufferDescription(upload_size);
        const auto staging_size =
            device->GetResourceAllocationInfo(0, 1, &upload_description).SizeInBytes;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (staging_size == maximum || resident_size == maximum)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Streamed geometry staging allocation size overflow.");
        SampleAllocatorBudgetStats(false, false);
        ScopedGpuAdmission admission(residency, resident_size, staging_size);
        if (!admission) return TryRelieveStreamingPressure(resident_size, staging_size, stream.priority);

        GeometryPageGpu candidate;
        candidate.fallback_page_slot = stream.fallback_index;
        if (stream.generation == UINT32_MAX)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Geometry publication generation exhausted.");
        candidate.generation = static_cast<std::uint32_t>(stream.generation + 1u);
        AllocationResource upload;
        if (auto prepared = PrepareGeometryPageUpload(candidate, metadata, stream.decoded_storage,
                MakeAssetId(stream.logical_id), stream.target_index, upload,
                stream.preparation.get()); !prepared)
        {
            static_cast<void>(streaming.Cancel(stream.request));
            stream.preparation.reset();
            return prepared;
        }
        const auto upload_ticket = RegisterNativeAllocation(
            ResidencyCategory::Staging, upload.allocation->GetSize(), false,
            ResidencySegmentForHeap(D3D12_HEAP_TYPE_UPLOAD));
        if (!upload_ticket)
        {
            static_cast<void>(residency.ReleaseNow(candidate.residency));
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Could not account streamed geometry allocations.");
        }
        admission.Complete();
        if (!streaming.BeginUpload(stream.request, resource_generation_id))
        {
            static_cast<void>(residency.ReleaseNow(candidate.residency));
            static_cast<void>(residency.ReleaseNow(upload_ticket));
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Environment geometry stream upload transition failed.");
        }
        static_cast<void>(residency.ReleaseNow(stream.decoded_residency));
        static_cast<void>(residency.ReleaseNow(stream.decode_reservation));
        stream.decoded_residency = {};
        stream.decode_reservation = {}; stream.read_bytes = 0;
        std::vector<std::byte>{}.swap(stream.decoded_storage);
        stream.preparation.reset();
        stream.candidate = std::move(candidate);
        stream.upload = std::move(upload);
        stream.upload_residency = upload_ticket;
        stream.upload_pending = true;
        return Result::Success();
    };
    for (const auto ordinal : order)
        if (auto pumped = pump_page(environment_geometry_streams[ordinal]); !pumped) return pumped;
    return Result::Success();
}
} // namespace hs
