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
    for (std::size_t index = 0; index < environment_meshes.size(); ++index)
    {
        const std::string mesh_name = std::string(kEnvironmentMaterialNames[index/3]) +
            (index%3 == 0 ? std::string{} : "_lod" + std::to_string(index%3));
        const auto geometry_ordinal = static_cast<std::uint32_t>(index / 3);
        const auto material_ordinal = render_data::EnvironmentGeometryMaterialOrdinal(geometry_ordinal);
        const std::string material_name(kAuthoredMaterialNames[material_ordinal]);
        const std::string logical_id = "render/" + mesh_name + ".rpkg";
        const std::string mesh_dependency =
            "environment/environment_" + mesh_name + ".meshbin";
        std::filesystem::path path;
        if (auto resolved = config.ResolveProduct(logical_id, path); !resolved)
            return resolved;
        std::vector<std::byte> bytes;
        if (index % 3 != 0 || geometry_ordinal >= render_data::kGroundDetailGeometryBegin)
            if (auto read = ReadBinary(path, bytes); !read) return read;
        render_data::GeometryMetadata geometry_metadata;
        std::string page_id;
        std::filesystem::path page_path;
        std::uint32_t fallback_page_slot = UINT32_MAX;
        render_data::RockMaterial packaged_material;
        std::string program_shader;
        std::string program_shadow_shader;
        try
        {
            const auto package = index % 3 == 0 && geometry_ordinal < render_data::kGroundDetailGeometryBegin ? material_headers[material_ordinal].view
                : MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::ReadPackage(bytes, MakeAssetId(logical_id)); });
            const render_data::ChunkView *geometry_chunk = nullptr;
            const render_data::ChunkView *material_chunk = nullptr;
            const render_data::ChunkView *program_chunk = nullptr;
            const render_data::ChunkView *bindings_chunk = nullptr;
            const render_data::ChunkView *hierarchy_chunk = nullptr;
            for (const auto &chunk : package.chunks)
            {
                if (chunk.type == render_data::ChunkType::GeometryMetadata)
                    geometry_chunk = &chunk;
                else if (chunk.type == render_data::ChunkType::MaterialMetadata)
                    material_chunk = &chunk;
                else if (chunk.type == render_data::ChunkType::MaterialProgram)
                    program_chunk = &chunk;
                else if (chunk.type == render_data::ChunkType::MaterialBindings)
                    bindings_chunk = &chunk;
                else if (chunk.type == render_data::ChunkType::GeometryHierarchyReference)
                    hierarchy_chunk = &chunk;
            }
            const std::string name(kEnvironmentMaterialNames[index / 3]);
            const bool masked = name.starts_with("leaf_") || name.starts_with("grass_");
            const auto layout_chunk = std::ranges::find(package.chunks,
                render_data::ChunkType::MaterialParameterLayout, &render_data::ChunkView::type);
            if (layout_chunk == package.chunks.end() || !layout_chunk->required ||
                layout_chunk->version != 1 || layout_chunk->count != 1 || layout_chunk->stride != 32 ||
                MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::DecodeMaterialParameterLayout(layout_chunk->bytes); }) !=
                    material_parameter_layouts[material_ordinal])
                throw std::runtime_error("environment material parameter layout mismatch");
            const auto surface_chunk = std::ranges::find(package.chunks,
                render_data::ChunkType::MaterialSurfaceLayout, &render_data::ChunkView::type);
            if (surface_chunk == package.chunks.end() || !surface_chunk->required ||
                surface_chunk->version != 1 || surface_chunk->count != 1 ||
                surface_chunk->stride != render_data::kMaterialSurfaceLayoutStride)
                throw std::runtime_error("environment material surface layout is missing or invalid");
            if (MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::DecodeMaterialSurfaceLayout(surface_chunk->bytes); }) !=
                material_surface_layouts[material_ordinal])
                throw std::runtime_error("environment surface profile differs across LOD headers");
            if (package.chunks.size() != (index % 3 == 0 ? 7u : 6u) ||
                !geometry_chunk || !material_chunk || !program_chunk || !bindings_chunk ||
                (index % 3 == 0 && (!hierarchy_chunk || !hierarchy_chunk->required ||
                    hierarchy_chunk->version != 1 || hierarchy_chunk->count != 1 ||
                    hierarchy_chunk->stride != render_data::kGeometryHierarchyReferenceStride)) ||
                (index % 3 != 0 && hierarchy_chunk != nullptr) ||
                geometry_chunk->version != 1 || material_chunk->version != 1 ||
                !geometry_chunk->required || !material_chunk->required ||
                geometry_chunk->stride != render_data::kGeometryMetadataStride ||
                geometry_chunk->count != 1 ||
                bindings_chunk->version != 1 || !bindings_chunk->required ||
                bindings_chunk->stride != render_data::kMaterialBindingStride ||
                bindings_chunk->count != render_data::kMaterialTextureSlots)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Invalid render package chunks: " + path.string());
            if (program_chunk)
            {
                if (program_chunk->version != 1 || !program_chunk->required ||
                    program_chunk->stride != render_data::kMaterialProgramStride ||
                    program_chunk->count != (masked ? 2u : 1u))
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                           "Invalid material program chunk: " + path.string());
                const auto programs = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::DecodeMaterialPrograms(
                    program_chunk->bytes, package.dependencies); });
                const auto scene_id = "render/shader/" + material_name + "_scene.dxil";
                const auto shadow_id = "render/shader/" + material_name + "_shadow.dxil";
                if (programs.size() != (masked ? 2u : 1u) ||
                    programs[0].kind != (masked
                        ? render_data::MaterialProgramKind::MaskedScenePS
                        : render_data::MaterialProgramKind::OpaqueScenePS) ||
                    package.dependencies[programs[0].dependency_index] != scene_id ||
                    (masked && (programs[1].kind !=
                        render_data::MaterialProgramKind::MaskedShadowPS ||
                        package.dependencies[programs[1].dependency_index] != shadow_id)))
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                           "Invalid material program identity: " + path.string());
                program_shader = package.dependencies[programs[0].dependency_index];
                if (masked)
                    program_shadow_shader = package.dependencies[programs[1].dependency_index];
            }
            packaged_material = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::DecodeRockMaterial(material_chunk->bytes,
                                                                 package.dependencies); });
            if ((render_data::ClassifyEnvironmentMaterial(packaged_material) ==
                 render_data::EnvironmentMaterialVariant::MaskedTwoSided) != masked)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Material program domain mismatch: " + path.string());
            for (const auto &dependency : package.dependencies)
            {
                std::filesystem::path dependency_path;
                if (auto resolved = config.ResolveProduct(dependency, dependency_path); !resolved)
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                           "Invalid render package dependency " + dependency +
                                               ": " + path.string());
            }
            if (std::ranges::find(package.dependencies, mesh_dependency) ==
                package.dependencies.end())
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Render package omits mesh dependency " + mesh_dependency +
                                           ": " + path.string());
            geometry_metadata = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::DecodeGeometryMetadata(
                geometry_chunk->bytes, package.dependencies, logical_id); });
            if (geometry_metadata.lod != index % 3 ||
                geometry_metadata.page_dependency_index == render_data::kNoGeometryDependency)
                throw std::runtime_error("environment geometry metadata has invalid LOD/page identity");
            if (index % 3 == 0)
            {
                if (auto loaded = environment_cluster_catalogs[index / 3].Load(config, package, logical_id,
                        [](const std::filesystem::path &input, std::vector<std::byte> &output) {
                            return ReadBinary(input, output);
                        }); !loaded)
                    return loaded;
            }
            page_id = package.dependencies[geometry_metadata.page_dependency_index];
            if (auto resolved = config.ResolveProduct(page_id, page_path); !resolved)
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
            const std::uint32_t required_layer = masked
                ? (name.starts_with("grass_") ? 1u : 2u) : geometry_metadata.max_material_index;
            const auto bindings = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::DecodeMaterialBindings(
                bindings_chunk->bytes, package.dependencies); });
            EnvironmentMaterialBindingsGpu selected_bindings;
            for (const auto &binding : bindings)
            {
                const auto slot = binding.slot;
                if (binding.dependency_index == 0xffff)
                {
                    if (!packaged_material.textures[slot].empty())
                        throw std::runtime_error("material metadata declares an absent texture binding");
                    continue;
                }
                const auto &texture_id = package.dependencies[binding.dependency_index];
                if (packaged_material.textures[slot] != texture_id)
                    throw std::runtime_error("material metadata and typed texture binding disagree");
                const render_data::MaterialTextureDefinition definition{
                    "slot_" + std::to_string(slot), texture_id, binding.dimension, binding.semantic};
                if (auto loaded = LoadEnvironmentMaterialTexture(definition, required_layer, uploads,
                        selected_bindings.texture_indices[slot]); !loaded)
                    return loaded;
                selected_bindings.product_ids[slot] = texture_id;
            }
            auto &material_bindings = environment_material_bindings[material_ordinal];
            if (!material_bindings_initialized[material_ordinal])
            {
                material_bindings = std::move(selected_bindings);
                material_bindings_initialized[material_ordinal] = true;
            }
            else if (material_bindings.product_ids != selected_bindings.product_ids)
                throw std::runtime_error("material texture bindings differ between mesh LOD packages");
        }
        catch (const std::exception &error)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Invalid render package " + path.string() + ": " + error.what());
        }
        auto &mesh = environment_meshes[index];
        const auto &sphere = geometry_metadata.bounds.sphere;
        mesh.bounding_radius = std::sqrt(sphere[0] * sphere[0] + sphere[1] * sphere[1] +
                                         sphere[2] * sphere[2]) + sphere[3];
        environment_variant_radii[index / 3] = std::max(
            environment_variant_radii[index / 3], mesh.bounding_radius);
        mesh.material = std::move(packaged_material);
        mesh.program_shader = std::move(program_shader);
        mesh.program_shadow_shader = std::move(program_shadow_shader);
        mesh.metadata = geometry_metadata;
        mesh.page_slot = static_cast<std::uint32_t>(index);
        mesh.fallback_page_slot = fallback_page_slot;
        mesh.vertex_count = geometry_metadata.vertex_count;
        mesh.index_count = geometry_metadata.index_count;
        mesh.meshlet_count = geometry_metadata.meshlet_count;
        if (index % 3 == 0)
        {
            auto &stream = environment_geometry_streams[index / 3];
            stream.path = page_path;
            stream.logical_id = page_id;
            stream.source_id = mesh_dependency;
            stream.metadata = geometry_metadata;
            stream.target_index = static_cast<std::uint32_t>(index);
            stream.fallback_index = fallback_page_slot;
        }
        // Startup never opens a LOD0 payload; its selected resident fallback is complete.
        if (index % 3 == 0) continue;
        std::vector<std::byte> page_bytes;
        if (auto read = ReadBinary(page_path, page_bytes); !read) return read;
        try
        {
            const auto page_package = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::ReadPackage(page_bytes, MakeAssetId(page_id)); });
            if (page_package.dependencies != std::vector<std::string>{mesh_dependency})
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Environment geometry page has an unexpected source dependency; expected only " +
                                           mesh_dependency + ": " + page_path.string());
        }
        catch (const std::exception &error)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Invalid environment geometry page " + page_path.string() + ": " +
                                       error.what());
        }
        if (auto created = MeasureStartupOperation(RendererStartupOperation::GeometryPrepare, [&] { return CreateGeometryPage(mesh, geometry_metadata, page_bytes,
                MakeAssetId(page_id), static_cast<std::uint32_t>(index), command_list.Get(), uploads); });
            !created) return created;
    }
    // Terrain is a material package; its geometry is the shared procedural page.
    {
        constexpr std::string_view logical_id = "render/material/terrain.rpkg";
        std::filesystem::path path;
        if (auto resolved = config.ResolveProduct(logical_id, path); !resolved) return resolved;
        std::vector<std::byte> bytes;
        try
        {
            const auto &package = material_headers[kTerrainMaterialOrdinal].view;
            const render_data::ChunkView *metadata = nullptr, *program = nullptr, *bindings = nullptr;
            for (const auto &chunk : package.chunks)
            {
                if (chunk.type == render_data::ChunkType::MaterialMetadata) metadata = &chunk;
                if (chunk.type == render_data::ChunkType::MaterialProgram) program = &chunk;
                if (chunk.type == render_data::ChunkType::MaterialBindings) bindings = &chunk;
            }
            const auto layout_chunk = std::ranges::find(package.chunks,
                render_data::ChunkType::MaterialParameterLayout, &render_data::ChunkView::type);
            if (layout_chunk == package.chunks.end() || !layout_chunk->required ||
                layout_chunk->version != 1 || layout_chunk->count != 1 || layout_chunk->stride != 32 ||
                MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::DecodeMaterialParameterLayout(layout_chunk->bytes); }) !=
                    material_parameter_layouts[kTerrainMaterialOrdinal])
                throw std::runtime_error("terrain material parameter layout mismatch");
            const auto surface_chunk = std::ranges::find(package.chunks,
                render_data::ChunkType::MaterialSurfaceLayout, &render_data::ChunkView::type);
            if (surface_chunk == package.chunks.end() || !surface_chunk->required ||
                surface_chunk->version != 1 || surface_chunk->count != 1 ||
                surface_chunk->stride != render_data::kMaterialSurfaceLayoutStride)
                throw std::runtime_error("terrain material surface layout is missing or invalid");
            if (MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::DecodeMaterialSurfaceLayout(surface_chunk->bytes); }) !=
                material_surface_layouts[kTerrainMaterialOrdinal])
                throw std::runtime_error("terrain surface profile changed during initialization");
            if (package.chunks.size() != 5 || !metadata || !program || !bindings ||
                !metadata->required || metadata->version != 1 || metadata->count != 1 ||
                !program->required || program->version != 1 || program->count != 1 ||
                program->stride != render_data::kMaterialProgramStride ||
                !bindings->required || bindings->version != 1 ||
                bindings->count != render_data::kMaterialTextureSlots ||
                bindings->stride != render_data::kMaterialBindingStride)
                throw std::runtime_error("terrain material package has invalid required chunks");
            for (const auto &dependency : package.dependencies)
            {
                std::filesystem::path dependency_path;
                if (auto resolved = config.ResolveProduct(dependency, dependency_path); !resolved) return resolved;
            }
            const auto programs = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::DecodeMaterialPrograms(program->bytes, package.dependencies); });
            if (programs.size() != 1 || programs[0].kind != render_data::MaterialProgramKind::OpaqueScenePS ||
                package.dependencies[programs[0].dependency_index] != "render/shader/terrain_scene.dxil")
                throw std::runtime_error("terrain material program identity is invalid");
            auto material = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::DecodeRockMaterial(metadata->bytes, package.dependencies); });
            if (render_data::ClassifyEnvironmentMaterial(material) !=
                    render_data::EnvironmentMaterialVariant::OpaqueSingleSided)
                throw std::runtime_error("terrain material must be opaque and single sided");
            const auto decoded = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::DecodeMaterialBindings(bindings->bytes, package.dependencies); });
            EnvironmentMaterialBindingsGpu selected;
            for (const auto &binding : decoded)
            {
                if (binding.dependency_index == 0xffff || binding.slot >= render_data::kMaterialTextureSlots)
                    throw std::runtime_error("terrain requires all six authored texture bindings");
                const auto &texture_id = package.dependencies[binding.dependency_index];
                if (material.textures[binding.slot] != texture_id)
                    throw std::runtime_error("terrain metadata and texture binding disagree");
                const render_data::MaterialTextureDefinition definition{
                    "slot_" + std::to_string(binding.slot), texture_id, binding.dimension, binding.semantic};
                if (auto loaded = LoadEnvironmentMaterialTexture(definition, 3, uploads,
                        selected.texture_indices[binding.slot]); !loaded) return loaded;
                selected.product_ids[binding.slot] = texture_id;
            }
            if (material_bindings_initialized[kTerrainMaterialOrdinal] &&
                environment_material_bindings[kTerrainMaterialOrdinal].product_ids != selected.product_ids)
                throw std::runtime_error("terrain texture bindings differ from its mesh packages");
            terrain_material = std::move(material);
            environment_material_bindings[kTerrainMaterialOrdinal] = std::move(selected);
        }
        catch (const std::exception &error)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Invalid terrain material package " + path.string() + ": " + error.what());
        }
    }
    result = command_list->Close();
    if (FAILED(result)) return HResultFailure("Close mesh upload", result);
    if (auto waited = SubmitResidentUtilityUpload(command_list.Get(), frames[0].allocator.Get(), uploads); !waited)
        return waited;
    for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
    {
        PublishEnvironmentMaterialTextures(frame);
        WriteGeometryPageDescriptors(frame);
    }
    return PrepareEnvironmentGeometryStreaming();
}

Result D3D12Renderer::Impl::PrepareEnvironmentGeometryStreaming()
{
    if (resource_generation_id.empty())
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Geometry streaming requires the selected content Generation ID.");
    for (std::size_t ordinal = 0; ordinal < environment_geometry_streams.size(); ++ordinal)
    {
        const auto &stream = environment_geometry_streams[ordinal];
        if (stream.path.empty() || stream.logical_id.empty() || stream.source_id.empty() ||
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
    if (stream.last_demand_frame != frame_number + 1u) stream.priority = 0u;
    stream.priority = std::max(stream.priority, priority);
    stream.last_demand_frame = frame_number + 1u;
    if (stream.request && !stream.retiring) static_cast<void>(streaming.UpdatePriority(stream.request, stream.priority));
    if (resource_generation_id.empty() || stream.path.empty() || stream.logical_id.empty() ||
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
            if (stream.last_demand_frame < frame_number) return Result::Success();
            std::error_code error;
            const auto file_bytes = std::filesystem::file_size(stream.path, error);
            if (error || !file_bytes || file_bytes > UINT64_MAX / 3u ||
                file_bytes > std::numeric_limits<std::size_t>::max() ||
                file_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Geometry stream read size overflow.");
            if (!residency.CanStage(file_bytes * 3u)) return Result::Success();
            stream.decoded_residency = residency.ReserveHost(file_bytes);
            stream.decode_reservation = residency.ReserveHost(file_bytes * 2u);
            if (!stream.decoded_residency || !stream.decode_reservation)
            {
                static_cast<void>(residency.ReleaseNow(stream.decoded_residency));
                static_cast<void>(residency.ReleaseNow(stream.decode_reservation));
                stream.decoded_residency = {}; stream.decode_reservation = {};
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Geometry stream host reservation failed.");
            }
            stream.read_bytes = file_bytes;
            bool submitted{};
            const auto queued = asset_io.Enqueue({stream.request, stream.content_generation,
                    stream.logical_id, stream.path, file_bytes, resident_generation_lease},
                    streaming, residency, stream.decoded_residency, submitted);
            if (!submitted)
            {
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
            if (!read.ready || read.cancelled) return Result::Success();
            stream.decoded_storage = std::move(read.bytes);
            stream.decoded_residency = read.storage;
            try
            {
                const auto page_package = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::ReadPackage(
                    stream.decoded_storage, MakeAssetId(stream.logical_id)); });
                if (page_package.dependencies != std::vector<std::string>{stream.source_id})
                    throw std::runtime_error(
                        "Streamed environment geometry page has an unexpected source dependency.");
                static_cast<void>(MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::DecodeGeometryPage(
                    stream.decoded_storage, MakeAssetId(stream.logical_id), stream.metadata); }));
            }
            catch (const std::exception &error)
            {
                static_cast<void>(streaming.Cancel(stream.request));
                std::vector<std::byte>{}.swap(stream.decoded_storage);
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Environment geometry stream read/decode failed for " + stream.logical_id +
                    ": " + error.what());
            }
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
        if (stream.last_demand_frame < frame_number) return Result::Success();

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
        if (staging_size == maximum || resident_size > maximum - staging_size)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Streamed geometry staging allocation size overflow.");
        const auto gpu_peak = resident_size + staging_size;
        ScopedGpuAdmission admission(residency, gpu_peak);
        if (!admission) return TryRelieveStreamingPressure(gpu_peak, stream.priority);

        GeometryPageGpu candidate;
        candidate.fallback_page_slot = stream.fallback_index;
        if (stream.generation == UINT32_MAX)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Geometry publication generation exhausted.");
        candidate.generation = static_cast<std::uint32_t>(stream.generation + 1u);
        AllocationResource upload;
        if (auto prepared = PrepareGeometryPageUpload(candidate, metadata, stream.decoded_storage,
                MakeAssetId(stream.logical_id), stream.target_index, upload); !prepared)
        {
            static_cast<void>(streaming.Cancel(stream.request));
            return prepared;
        }
        const auto upload_ticket = residency.Register(
            ResidencyCategory::Staging, upload.allocation->GetSize(), false);
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
