#include "renderer_impl.hpp"
#include "../../../Content/Shaders/terrain_histogram_abi.hlsli"

#include <hs/core/cooked_format.hpp>
#include <hs/core/sha256.hpp>
#include <hs/render_data/texture_package.hpp>

#include <algorithm>
#include <stdexcept>
#include <bit>
#include <cmath>
#include <cstring>
#include <span>

namespace hs
{
namespace
{
constexpr D3D12_RESOURCE_STATES kAllShaderRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

render_data::TextureDescriptorStorage TextureStreamingDescriptor(
    const RendererConfig &config, AssetId asset, std::span<const std::byte> bytes,
    const render_data::TexturePackageView &package)
{
    render_data::TextureDescriptor descriptor;
    if (config.development_catalog)
    {
        const auto found = config.development_catalog->FindTexture(asset);
        if (!found) throw std::runtime_error("Missing admitted development texture descriptor.");
        descriptor = *found;
    }
    else if (config.packed_content)
    {
        const auto handle = FindRendererTexture(config, asset);
        if (!handle) throw std::runtime_error("Missing packed texture descriptor.");
        descriptor = config.packed_textures[handle.value];
    }
    else
    {
        render_data::TextureDescriptorStorage result;
        result.asset_id = asset; result.description = package.description;
        result.dependencies.reserve(package.dependencies.size());
        for (const auto &dependency : package.dependencies) result.dependencies.push_back(MakeAssetId(dependency));
        result.subresources.reserve(package.subresources.size());
        for (const auto &subresource : package.subresources)
            result.subresources.push_back({static_cast<std::uint64_t>(subresource.bytes.data() - bytes.data()),
                subresource.row_pitch, subresource.slice_pitch, static_cast<std::uint32_t>(subresource.bytes.size()),
                Sha256(subresource.bytes), true});
        return result;
    }
    return {descriptor.asset_id, descriptor.bulk_index, descriptor.description,
        std::vector<AssetId>(descriptor.dependencies.begin(), descriptor.dependencies.end()),
        std::vector<render_data::TextureSubresourceLayout>(descriptor.subresources.begin(), descriptor.subresources.end()),
        descriptor.dimension, descriptor.depth};
}
} // namespace

Result D3D12Renderer::Impl::PrepareMaterialParameters(
    const RendererConfig &candidate, bool require_compatible_layout, MaterialPreparationScope scope)
{
    if (material_parameter_upload_pending)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Material parameter upload is still in flight.");
    material_parameter_candidate.Reset();
    material_parameter_candidate_counts.fill(0);
    material_parameter_candidate_bases.fill(0);
    for (auto &layout : material_parameter_candidate_layouts) layout.clear();
    material_parameter_upload.Reset();
    std::vector<render_data::MaterialParameterSlot> slots;
    try
    {
        const auto material_count = scope == MaterialPreparationScope::StaticEnvironment
            ? kAuthoredMaterialNames.size() : kResidentMaterialCount;
        for (std::size_t ordinal = 0; ordinal < material_count; ++ordinal)
        {
            const bool hero = ordinal >= kAuthoredMaterialNames.size();
            const auto name = hero ? kCharacterMaterialNames[ordinal - kAuthoredMaterialNames.size()]
                                   : kAuthoredMaterialNames[ordinal];
            const std::string id = "render/material/" +
                std::string(name) + "/parameters.rpkg";
            const auto material_id = render_data::MaterialProduct(name);
            if (candidate.packed_content || candidate.development_catalog)
            {
                const auto handle = FindRendererMaterial(candidate, MakeAssetId(material_id));
                if (!handle) throw std::runtime_error("Missing compiled material parameters: " + material_id);
                const auto &material = candidate.packed_materials[handle.value];
                const auto layout = MaterialLayoutDigest(material);
                const auto base = static_cast<std::uint32_t>(slots.size());
                if (require_compatible_layout && (layout != material_parameter_layouts[ordinal] ||
                    base != material_parameter_bases[ordinal]))
                    throw std::runtime_error("Compiled material parameter reload changes the resident layout: " + material_id);
                material_parameter_candidate_layouts[ordinal] = layout;
                material_parameter_candidate_bases[ordinal] = base;
                material_parameter_candidate_counts[ordinal] =
                    static_cast<std::uint32_t>(material.defaults.size());
                slots.insert(slots.end(), material.defaults.begin(), material.defaults.end());
                continue;
            }
            RendererAssetSource path;
            if (auto resolved = ResolveRendererAsset(candidate, id, path); !resolved) return resolved;
            std::vector<std::byte> bytes;
            if (auto read = ReadBinary(path, bytes); !read) return read;
            const auto definitions = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::ReadMaterialParameterPackage(bytes, MakeAssetId(id)); });
            for (const std::string_view required : {"metallic", "roughness_scale"})
                if (!hero && std::ranges::find(definitions, required,
                        &render_data::MaterialParameterDefinition::id) == definitions.end())
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                        "Material parameter declaration is missing: " + std::string(required));
            material_parameter_candidate_layouts[ordinal] =
                render_data::MaterialParameterLayoutHash(definitions);
            const auto &material = material_headers[ordinal].view;
            const auto layout = std::ranges::find(material.chunks,
                render_data::ChunkType::MaterialParameterLayout, &render_data::ChunkView::type);
            if (layout == material.chunks.end() || !layout->required || layout->version != 1 ||
                layout->count != 1 || layout->stride != 32 ||
                MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return render_data::DecodeMaterialParameterLayout(layout->bytes); }) !=
                    material_parameter_candidate_layouts[ordinal])
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Material parameter layout disagrees with its shader/material package: " + id);
            material_parameter_candidate_bases[ordinal] = static_cast<std::uint32_t>(slots.size());
            material_parameter_candidate_counts[ordinal] =
                static_cast<std::uint32_t>(definitions.size());
            if (require_compatible_layout &&
                (material_parameter_candidate_layouts[ordinal] != material_parameter_layouts[ordinal] ||
                 material_parameter_candidate_bases[ordinal] != material_parameter_bases[ordinal]))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Material parameter reload changes the resident layout: " + id);
            for (const auto &definition : definitions) slots.push_back({definition.default_words});
        }
        if (scope == MaterialPreparationScope::Resident)
            if (auto prepared = PrepareDecalMaterialParameters(candidate, require_compatible_layout, slots); !prepared)
                return prepared;
    }
    catch (const std::exception &error)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Invalid material parameter package: " + std::string(error.what()));
    }
    const auto max_slot_count = scope == MaterialPreparationScope::StaticEnvironment
        ? 64 * kAuthoredMaterialNames.size()
        : 64 * (kResidentMaterialCount + render_data::kDecalMaterialNames.size());
    if (slots.empty() || slots.size() > max_slot_count)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Invalid material parameter slot count.");
    material_parameter_candidate_slot_count = static_cast<std::uint32_t>(slots.size());
    material_parameter_candidate_slots = slots;
    // The render thread owns publication. Drain readers before reusing the
    // initialization command allocator; old table/descriptors stay live.
    if (auto waited = WaitForGpu(); !waited) return waited;
    const auto size = slots.size() * sizeof(render_data::MaterialParameterSlot);
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (auto created = CreateAllocation(material_parameter_candidate, allocation,
            BufferDescription(size), D3D12_RESOURCE_STATE_COPY_DEST); !created) return created;
    allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    if (auto created = CreateAllocation(material_parameter_upload, allocation,
            BufferDescription(size), D3D12_RESOURCE_STATE_GENERIC_READ); !created) return created;
    void *mapped{};
    D3D12_RANGE no_read{};
    auto result = material_parameter_upload.resource->Map(0, &no_read, &mapped);
    if (FAILED(result)) return HResultFailure("Map material parameter upload", result);
    std::memcpy(mapped, slots.data(), size);
    material_parameter_upload.resource->Unmap(0, nullptr);
    result = frames[0].allocator->Reset();
    if (FAILED(result)) return HResultFailure("Reset material parameter allocator", result);
    result = command_list->Reset(frames[0].allocator.Get(), nullptr);
    if (FAILED(result)) return HResultFailure("Reset material parameter list", result);
    command_list->CopyBufferRegion(material_parameter_candidate.resource.Get(), 0,
                                   material_parameter_upload.resource.Get(), 0, size);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = material_parameter_candidate.resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = kAllShaderRead;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1, &barrier);
    result = command_list->Close();
    if (FAILED(result)) return HResultFailure("Close material parameter list", result);
    result = device.As(&material_parameter_upload_device);
    if (FAILED(result)) return HResultFailure("Query material parameter upload device", result);
    std::vector<AllocationResource> uploads;
    uploads.push_back(std::move(material_parameter_upload));
    material_parameter_upload_pending = true;
    if (auto waited = SubmitResidentUtilityUpload(command_list.Get(), frames[0].allocator.Get(), uploads); !waited)
    {
        // Keep both submitted buffers alive through Shutdown/device removal.
        frame_submission_faulted = true;
        return waited;
    }
    material_parameter_upload_pending = false;
    material_parameter_upload.Reset();
    material_parameter_upload_device.Reset();
    return Result::Success();
}

void D3D12Renderer::Impl::CommitMaterialParameters() noexcept
{
    for (std::size_t ordinal = 0; ordinal < kResidentMaterialCount; ++ordinal)
    {
        bool changed = true;
        if (material_parameters.resource &&
            material_parameter_counts[ordinal] == material_parameter_candidate_counts[ordinal] &&
            material_parameter_bases[ordinal] == material_parameter_candidate_bases[ordinal] &&
            material_parameter_layouts[ordinal] == material_parameter_candidate_layouts[ordinal])
        {
            const auto old_slots = std::span<const render_data::MaterialParameterSlot>(
                material_parameter_slots).subspan(material_parameter_bases[ordinal],
                    material_parameter_counts[ordinal]);
            const auto next_slots = std::span<const render_data::MaterialParameterSlot>(
                material_parameter_candidate_slots).subspan(material_parameter_candidate_bases[ordinal],
                    material_parameter_candidate_counts[ordinal]);
            changed = !std::ranges::equal(old_slots, next_slots);
        }
        if (changed)
            material_parameter_revisions[ordinal] = material_parameter_revision + 1u;
    }
    Sha256Stream generation_hash;
    generation_hash.Add(std::as_bytes(std::span(material_parameter_candidate_bases).first(
        kAuthoredMaterialNames.size())));
    generation_hash.Add(std::as_bytes(std::span(material_parameter_candidate_counts).first(
        kAuthoredMaterialNames.size())));
    for (std::size_t ordinal = 0; ordinal < kAuthoredMaterialNames.size(); ++ordinal)
    {
        const auto &layout = material_parameter_candidate_layouts[ordinal];
        const auto layout_size = static_cast<std::uint64_t>(layout.size());
        generation_hash.Add(std::as_bytes(std::span(&layout_size, 1)));
        generation_hash.Add(std::as_bytes(std::span(layout.data(), layout.size())));
    }
    const auto authored_end = material_parameter_candidate_bases[kAuthoredMaterialNames.size() - 1] +
        material_parameter_candidate_counts[kAuthoredMaterialNames.size() - 1];
    generation_hash.Add(std::as_bytes(std::span(material_parameter_candidate_slots).first(authored_end)));
    const auto generation_parameters = generation_hash.Finish();
    if (!generation_material_parameters_valid)
    {
        generation_material_parameters = generation_parameters;
        generation_material_parameters_valid = true;
    }
    material_parameters_match_generation = generation_parameters == generation_material_parameters;

    const auto begin = material_parameter_candidate_bases[kTerrainMaterialOrdinal];
    const auto end = material_parameter_candidate_bases[kTerrainMaterialOrdinal + 1];
    Sha256Stream hash;
    hash.Add(std::as_bytes(std::span(material_parameter_candidate_slots).subspan(begin, end - begin)));
    const auto parameters = hash.Finish();
    if (!terrain_generation_parameters_valid)
    {
        terrain_generation_parameters = parameters;
        terrain_generation_parameters_valid = true;
    }
    const bool matches = parameters == terrain_generation_parameters;
    terrain_parameters_changed |= matches != terrain_parameters_match;
    terrain_parameters_match = matches;
    // Preparation completed and drained all readers. Publishing every frame
    // slot together prevents raster, shadow and compute observing mixed values.
    material_parameters = std::move(material_parameter_candidate);
    material_parameter_slot_count = material_parameter_candidate_slot_count;
    ++material_parameter_revision;
    material_parameter_bases = material_parameter_candidate_bases;
    material_parameter_counts = material_parameter_candidate_counts;
    material_parameter_layouts = std::move(material_parameter_candidate_layouts);
    material_parameter_slots = std::move(material_parameter_candidate_slots);
    CommitDecalMaterialParameters();
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_UNKNOWN;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.NumElements = material_parameter_slot_count;
    view.Buffer.StructureByteStride = sizeof(render_data::MaterialParameterSlot);
    for (UINT frame = 0; frame < kFrameCount; ++frame)
    {
        auto handle = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (static_cast<SIZE_T>(frame) * kTextureDescriptorCount +
                       kMaterialParameterDescriptorOffset) * srv_stride;
        device->CreateShaderResourceView(material_parameters.resource.Get(), &view, handle);
    }
}

Result D3D12Renderer::Impl::CreateEnvironmentTextures()
{
    struct TextureSpec { const char *name; DXGI_FORMAT format; UINT slices; UINT mips; UINT width{2048}; UINT height{2048}; };
    constexpr TextureSpec specs[] = {
        {"terrain_gaussian", DXGI_FORMAT_BC7_UNORM,4,12},
        {"terrain_surface", DXGI_FORMAT_BC5_UNORM,4,12},
        {"terrain_normal", DXGI_FORMAT_BC5_UNORM,4,12},
        {"terrain_detail", DXGI_FORMAT_BC5_UNORM,4,12},
        {"terrain_height", DXGI_FORMAT_BC4_UNORM,4,12},
        {"rock_basecolor", DXGI_FORMAT_BC7_UNORM_SRGB,4,12},
        {"rock_surface", DXGI_FORMAT_BC5_UNORM,4,12},
        {"rock_normal", DXGI_FORMAT_BC5_UNORM,4,12},
        {"rock_detail", DXGI_FORMAT_BC5_UNORM,4,12},
        {"bark_basecolor", DXGI_FORMAT_BC7_UNORM_SRGB,3,12},
        {"bark_surface", DXGI_FORMAT_BC5_UNORM,3,12},
        {"bark_normal", DXGI_FORMAT_BC5_UNORM,3,12},
        {"bark_detail", DXGI_FORMAT_BC5_UNORM,3,12},
        {"grass_basecolor", DXGI_FORMAT_BC7_UNORM_SRGB,2,6},
        {"grass_normal", DXGI_FORMAT_BC5_UNORM,2,6},
        {"grass_roughness", DXGI_FORMAT_BC4_UNORM,2,6},
        {"leaf_basecolor", DXGI_FORMAT_BC7_UNORM_SRGB,3,6},
        {"leaf_normal", DXGI_FORMAT_BC5_UNORM,3,6},
        {"leaf_roughness", DXGI_FORMAT_BC4_UNORM,3,6},
        {"terrain_histogram", DXGI_FORMAT_R32G32B32A32_FLOAT,HS_TERRAIN_HISTOGRAM_LAYERS,1,
            HS_TERRAIN_HISTOGRAM_WIDTH,HS_TERRAIN_HISTOGRAM_HEIGHT},
    };
    static_assert(std::size(specs) == kEnvironmentTextureCount);
    constexpr UINT kStreamTextureIndex = 0;
    constexpr UINT kFallbackFirstMip = 4;
    environment_fallback_residency = {};
    environment_material_textures.clear();
    environment_material_bindings = {};
    retired_streaming_textures.clear();
    streaming.Reset();
    auto result = frames[0].allocator->Reset();
    if (FAILED(result)) return HResultFailure("Reset environment upload allocator", result);
    result = command_list->Reset(frames[0].allocator.Get(), nullptr);
    if (FAILED(result)) return HResultFailure("Reset environment upload list", result);
    std::vector<AllocationResource> uploads;
    for (UINT texture_index = 0; texture_index < std::size(specs); ++texture_index)
    {
        const auto &spec = specs[texture_index];
        const std::string source_id = std::string("environment/environment_") + spec.name + ".dds";
        const std::string logical_id =
            std::string("render/texture/environment_") + spec.name + ".rpkg";
        environment_texture_ids[texture_index] = logical_id;
        environment_texture_descriptions[texture_index] = {
            spec.width, spec.height, spec.slices, spec.mips,
            static_cast<std::uint32_t>(spec.format)};
        RendererAssetSource path;
        if (auto resolved = ResolveRendererAsset(config, logical_id, path); !resolved)
            return resolved;
        std::vector<std::byte> storage;
        if (auto read = ReadBinary(path, storage); !read) return read;
        render_data::TexturePackageView package;
        auto invalid = [&] { return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Invalid environment texture: " + path.Describe(logical_id)); };
        {
            try
            {
                package = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return ReadRendererTexturePackage(config, logical_id, storage); });
            }
            catch (const std::exception &error)
            {
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Invalid environment texture package " + path.Describe(logical_id) + ": " + error.what());
            }
            if (package.description.width != spec.width ||
                package.description.height != spec.height ||
                package.description.array_size != spec.slices ||
                package.description.mip_count != spec.mips ||
                package.description.format != static_cast<std::uint32_t>(spec.format) ||
                !RendererTextureDependencyMatches(config, logical_id, package, source_id) ||
                package.subresources.size() != spec.slices * spec.mips)
                return invalid();
            try
            {
                auto descriptor = TextureStreamingDescriptor(config, MakeAssetId(logical_id), storage, package);
                if (auto supplied = texture_streaming.SupplyDescriptor(logical_id, std::move(descriptor)); !supplied)
                    return supplied;
            }
            catch (const std::exception &error)
            {
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Could not retain environment texture streaming metadata: " + std::string(error.what()));
            }
        }
        const auto source_mip_base = std::min(kFallbackFirstMip, spec.mips - 1u);
        const auto target_mips = spec.mips - source_mip_base;
        const auto target_width = std::max(1u, spec.width >> source_mip_base);
        const auto target_height = std::max(1u, spec.height >> source_mip_base);
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = target_width;
        description.Height = target_height;
        description.DepthOrArraySize = static_cast<UINT16>(spec.slices);
        description.MipLevels = static_cast<UINT16>(target_mips);
        description.Format = spec.format;
        description.SampleDesc = {1, 0};
        D3D12MA::ALLOCATION_DESC allocation{};
        allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        auto &texture = environment_textures[texture_index];
        if (auto created = CreateAllocation(texture, allocation, description,
                D3D12_RESOURCE_STATE_COPY_DEST); !created) return created;
        const auto count = spec.slices * target_mips;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        std::vector<UINT> rows(count); std::vector<UINT64> row_sizes(count);
        UINT64 upload_size{};
        device->GetCopyableFootprints(&description, 0, count, 0, footprints.data(),
            rows.data(), row_sizes.data(), &upload_size);
        for (UINT subresource = 0; subresource < count; ++subresource)
        {
            const auto &footprint = footprints[subresource];
            if (rows[subresource] == 0 || row_sizes[subresource] > footprint.Footprint.RowPitch ||
                footprint.Offset > upload_size ||
                static_cast<UINT64>(rows[subresource] - 1) * footprint.Footprint.RowPitch +
                    row_sizes[subresource] > upload_size - footprint.Offset)
                return invalid();
            {
                const auto target_slice = subresource / target_mips;
                const auto target_mip = subresource % target_mips;
                const auto source_mip = target_mip + source_mip_base;
                const auto source_index = target_slice * spec.mips + source_mip;
                const auto &source = package.subresources[source_index];
                if (source.array_slice != target_slice ||
                    source.mip_level != source_mip ||
                    source.width != std::max(1u, spec.width >> source_mip) ||
                    source.height != std::max(1u, spec.height >> source_mip) ||
                    row_sizes[subresource] != source.row_pitch ||
                    static_cast<UINT64>(rows[subresource]) * source.row_pitch != source.slice_pitch ||
                    source.bytes.size() != source.slice_pitch)
                    return invalid();
            }
        }
        uploads.emplace_back();
        allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto created = CreateAllocation(uploads.back(), allocation, BufferDescription(upload_size),
                D3D12_RESOURCE_STATE_GENERIC_READ); !created) return created;
        std::byte *mapped{}; D3D12_RANGE no_read{};
        result = uploads.back().resource->Map(0, &no_read, reinterpret_cast<void **>(&mapped));
        if (FAILED(result)) return HResultFailure("Map environment upload", result);
        StartupOperationScope pack_timing(RendererStartupOperation::HostUploadPack);
        for (UINT subresource = 0; subresource < count; ++subresource)
        {
            const auto target_slice = subresource / target_mips;
            const auto target_mip = subresource % target_mips;
            const auto source_mip = target_mip + source_mip_base;
            const auto source_index = target_slice * spec.mips + source_mip;
            const render_data::TextureSubresourceView *source =
                &package.subresources[source_index];
            for (UINT row = 0; row < rows[subresource]; ++row)
            {
                const auto *source_row = source->bytes.data() +
                    static_cast<std::size_t>(row) * source->row_pitch;
                std::memcpy(mapped + footprints[subresource].Offset +
                    static_cast<std::size_t>(row) * footprints[subresource].Footprint.RowPitch,
                    source_row, static_cast<std::size_t>(row_sizes[subresource]));
            }
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = texture.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = subresource;
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = uploads.back().resource.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprints[subresource];
            command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        uploads.back().resource->Unmap(0, nullptr);
        pack_timing.Stop();
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = enhanced ? D3D12_RESOURCE_STATE_COMMON : kAllShaderRead;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);
        texture.graph_access = enhanced ? Access::Common : Access::AllShaderRead;
    }
    if (auto prepared = MeasureStartupStep(RendererStartupStep::EnvironmentBrdfLut, [&] {
            return CreateEnvironmentBrdfLut(config, environment_brdf_lut, &uploads);
        }); !prepared) return prepared;
    result = command_list->Close();
    if (FAILED(result)) return HResultFailure("Close environment upload", result);
    if (auto waited = SubmitResidentUtilityUpload(command_list.Get(), frames[0].allocator.Get(), uploads); !waited)
        return waited;
    for (UINT frame = 0; frame < kFrameCount; ++frame)
    {
        for (UINT index = 0; index < std::size(specs); ++index)
        {
            const auto resource_description = environment_textures[index].resource->GetDesc();
            WriteEnvironmentTextureDescriptor(
                frame, index, environment_textures[index].resource.Get(), specs[index].format,
                resource_description.MipLevels, resource_description.DepthOrArraySize);
        }
        PublishEnvironmentMaterialTextures(frame);
    }
    if (environment_textures[kStreamTextureIndex].allocation)
        environment_fallback_residency = RegisterNativeAllocation(
            ResidencyCategory::Texture,
            environment_textures[kStreamTextureIndex].allocation->GetSize(), true,
            ResidencySegmentForHeap(D3D12_HEAP_TYPE_DEFAULT));
    if (!environment_fallback_residency)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Could not account the pinned coarse texture.");
    if (auto budget = UpdateResidencyBudget(); !budget) return budget;
    return PrepareEnvironmentStreaming();
}

void D3D12Renderer::Impl::WriteEnvironmentTextureDescriptor(
    std::uint32_t frame_slot, std::uint32_t texture_index, ID3D12Resource *resource,
    DXGI_FORMAT format, std::uint32_t mip_count, std::uint32_t array_size) const noexcept
{
    StartupOperationScope startup_timing(RendererStartupOperation::DescriptorWrite);
    auto handle = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr +=
        (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
         kPostTextureDescriptorCount + kCharacterDescriptorCount + kMonsterPbrDescriptorCount +
         texture_index) *
        srv_stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2DArray.MipLevels = mip_count;
    view.Texture2DArray.ArraySize = array_size;
    device->CreateShaderResourceView(resource, &view, handle);
}

Result D3D12Renderer::Impl::LoadEnvironmentMaterialTexture(
    const render_data::MaterialTextureDefinition &binding, std::uint32_t required_layer,
    std::vector<AllocationResource> &uploads, std::uint32_t &texture_index,
    render_data::TextureHandle compiled_texture)
{
    for (std::uint32_t index = 0; index < environment_texture_ids.size(); ++index)
        if (environment_texture_ids[index] == binding.product_id)
        {
            render_data::ValidateMaterialTexturePackage(
                binding, environment_texture_descriptions[index], required_layer);
            texture_index = index;
            return Result::Success();
        }
    for (std::size_t index = 0; index < environment_material_textures.size(); ++index)
        if (environment_material_textures[index].product_id == binding.product_id)
        {
            render_data::ValidateMaterialTexturePackage(
                binding, environment_material_textures[index].description, required_layer);
            texture_index = kEnvironmentTextureCount + static_cast<std::uint32_t>(index);
            return Result::Success();
        }

    RendererAssetSource path;
    if (compiled_texture)
    {
        if ((!config.packed_content && !config.development_catalog) ||
            compiled_texture.value >= config.packed_textures.size())
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Material texture handle is invalid.");
        const auto &texture = config.packed_textures[compiled_texture.value];
        if (auto resolved = ResolveRendererAsset(config, binding.product_id, path); !resolved) return resolved;
        const auto expected_bytes = config.packed_bulks[texture.bulk_index].range.byte_count;
        const auto loose_proof = path.AdmittedWholeFileProof();
        const bool size_matches = path.IsPacked()
            ? path.Packed().range.byte_count == expected_bytes
            : loose_proof && loose_proof->byte_count == expected_bytes;
        if (!size_matches)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Material texture product handle differs.");
    }
    else if (auto resolved = ResolveRendererAsset(config, binding.product_id, path); !resolved) return resolved;
    std::vector<std::byte> bytes;
    if (auto read = ReadBinary(path, bytes); !read) return read;
    const auto package = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] {
        return compiled_texture ? render_data::BindTextureDescriptor(bytes, config.packed_textures[compiled_texture.value]) :
            ReadRendererTexturePackage(config, binding.product_id, bytes);
    });
    try
    {
        auto descriptor = TextureStreamingDescriptor(config, MakeAssetId(binding.product_id), bytes, package);
        if (auto supplied = texture_streaming.SupplyDescriptor(binding.product_id, std::move(descriptor)); !supplied)
            return supplied;
    }
    catch (const std::exception &error)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Could not retain texture streaming metadata: " + std::string(error.what()));
    }
    render_data::ValidateMaterialTexturePackage(binding, package.description, required_layer);
    if (config.development_catalog)
    {
        const auto descriptor = config.development_catalog->FindTexture(MakeAssetId(binding.product_id));
        if (!descriptor || std::ranges::any_of(descriptor->dependencies, [&](AssetId asset) {
                if (!config.development_catalog->Contains(asset)) return true;
                return !RendererIsSourceRevision(config, asset) &&
                    !config.development_catalog->HasRuntimeProduct(asset); }))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Material texture dependency is absent.");
    }
    else if (!config.packed_content) for (const auto &dependency : package.dependencies)
    {
        if (RendererIsSourceRevision(config, dependency))
        {
            if (RendererProductDigest(config, dependency).empty())
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Texture package references an unauthenticated source revision " + dependency);
            continue;
        }
        if (!RendererIsAdmittedRuntimeProduct(config, dependency))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Texture package dependency is absent from runtime and provenance catalogs: " + dependency);
        RendererAssetSource dependency_path;
        if (auto resolved = ResolveRendererAsset(config, dependency, dependency_path); !resolved)
            return resolved;
    }
    const auto &source_description = package.description;
    const auto first_mip = std::min(4u, source_description.mip_count - 1u);
    const auto target_mips = source_description.mip_count - first_mip;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = std::max(1u, source_description.width >> first_mip);
    description.Height = std::max(1u, source_description.height >> first_mip);
    description.DepthOrArraySize = static_cast<UINT16>(source_description.array_size);
    description.MipLevels = static_cast<UINT16>(target_mips);
    // Color packages carry the sRGB format. The SRV performs the sole decode.
    description.Format = static_cast<DXGI_FORMAT>(source_description.format);
    description.SampleDesc = {1, 0};
    EnvironmentMaterialTextureGpu texture;
    texture.product_id = binding.product_id;
    texture.description = source_description;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (auto created = CreateAllocation(texture.texture, allocation, description,
                                        D3D12_RESOURCE_STATE_COPY_DEST); !created)
        return created;
    const auto count = source_description.array_size * target_mips;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
    std::vector<UINT> rows(count);
    std::vector<UINT64> row_sizes(count);
    UINT64 upload_size{};
    device->GetCopyableFootprints(&description, 0, count, 0, footprints.data(),
                                  rows.data(), row_sizes.data(), &upload_size);
    for (UINT subresource = 0; subresource < count; ++subresource)
    {
        const auto source_index = (subresource / target_mips) * source_description.mip_count +
            subresource % target_mips + first_mip;
        const auto &source = package.subresources[source_index];
        const auto &footprint = footprints[subresource];
        if (rows[subresource] == 0 || row_sizes[subresource] != source.row_pitch ||
            row_sizes[subresource] > footprint.Footprint.RowPitch ||
            static_cast<UINT64>(rows[subresource]) * source.row_pitch != source.slice_pitch ||
            source.bytes.size() != source.slice_pitch || footprint.Offset > upload_size ||
            static_cast<UINT64>(rows[subresource] - 1) * footprint.Footprint.RowPitch +
                row_sizes[subresource] > upload_size - footprint.Offset)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Invalid material texture footprint: " + path.Describe(binding.product_id));
    }
    uploads.emplace_back();
    allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    if (auto created = CreateAllocation(uploads.back(), allocation, BufferDescription(upload_size),
                                        D3D12_RESOURCE_STATE_GENERIC_READ); !created)
        return created;
    std::byte *mapped{};
    D3D12_RANGE no_read{};
    const auto mapped_result = uploads.back().resource->Map(
        0, &no_read, reinterpret_cast<void **>(&mapped));
    if (FAILED(mapped_result)) return HResultFailure("Map material texture upload", mapped_result);
    StartupOperationScope pack_timing(RendererStartupOperation::HostUploadPack);
    for (UINT subresource = 0; subresource < count; ++subresource)
    {
        const auto source_index = (subresource / target_mips) * source_description.mip_count +
            subresource % target_mips + first_mip;
        const auto &source = package.subresources[source_index];
        for (UINT row = 0; row < rows[subresource]; ++row)
            std::memcpy(mapped + footprints[subresource].Offset +
                            static_cast<std::size_t>(row) * footprints[subresource].Footprint.RowPitch,
                        source.bytes.data() + static_cast<std::size_t>(row) * source.row_pitch,
                        static_cast<std::size_t>(row_sizes[subresource]));
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = texture.texture.resource.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = subresource;
        D3D12_TEXTURE_COPY_LOCATION source_location{};
        source_location.pResource = uploads.back().resource.Get();
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source_location.PlacedFootprint = footprints[subresource];
        command_list->CopyTextureRegion(&destination, 0, 0, 0, &source_location, nullptr);
    }
    uploads.back().resource->Unmap(0, nullptr);
    pack_timing.Stop();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.texture.resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = enhanced ? D3D12_RESOURCE_STATE_COMMON : kAllShaderRead;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1, &barrier);
    texture.texture.graph_access = enhanced ? Access::Common : Access::AllShaderRead;
    texture_index = kEnvironmentTextureCount +
        static_cast<std::uint32_t>(environment_material_textures.size());
    environment_material_textures.push_back(std::move(texture));
    return Result::Success();
}

void D3D12Renderer::Impl::WriteEnvironmentMaterialTextureDescriptor(
    std::uint32_t frame_slot, std::uint32_t material, std::uint32_t slot,
    ID3D12Resource *resource) const noexcept
{
    StartupOperationScope startup_timing(RendererStartupOperation::DescriptorWrite);
    auto handle = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    auto &bank = environment_descriptor_banks[frame_slot];
    if (bank.heap.Get() != srv_cpu_heap.Get() || bank.first.ptr != handle.ptr || bank.stride != srv_stride)
    {
        bank = {};
        bank.heap = srv_cpu_heap;
        bank.first = handle;
        bank.stride = srv_stride;
    }
    auto &entry = bank.entries[material * render_data::kMaterialTextureSlots + slot];
    // This fixed SRV depends only on the retained resource's immutable native description.
    if (entry.written && entry.resource.Get() == resource) return;
    handle.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
                   kMaterialTextureDescriptorOffset +
                   material * render_data::kMaterialTextureSlots + slot) * srv_stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_BC7_UNORM_SRGB;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.ArraySize = 1;
    if (resource)
    {
        const auto description = resource->GetDesc();
        view.Format = description.Format;
        view.Texture2DArray.MipLevels = description.MipLevels;
        view.Texture2DArray.ArraySize = description.DepthOrArraySize;
    }
    device->CreateShaderResourceView(resource, &view, handle);
    entry.resource = resource;
    entry.written = true;
}

void D3D12Renderer::Impl::PublishEnvironmentMaterialTextures(std::uint32_t frame_slot) const noexcept
{
    StartupOperationScope startup_timing(RendererStartupOperation::DescriptorWrite);
    for (std::uint32_t material = 0; material < environment_material_bindings.size(); ++material)
        for (std::uint32_t slot = 0; slot < render_data::kMaterialTextureSlots; ++slot)
        {
            const auto texture_index = environment_material_bindings[material].texture_indices[slot];
            ID3D12Resource *resource = nullptr;
            if (texture_index < kEnvironmentTextureCount)
                resource = environment_textures[texture_index].resource.Get();
            else if (texture_index != UINT32_MAX)
                resource = environment_material_textures[texture_index - kEnvironmentTextureCount]
                               .texture.resource.Get();
            const auto streamed = texture_streaming.View(
                material_texture_stream_ids[material * render_data::kMaterialTextureSlots + slot]);
            if (streamed.resource) resource = streamed.resource;
            WriteEnvironmentMaterialTextureDescriptor(frame_slot, material, slot, resource);
        }
}

Result D3D12Renderer::Impl::PrepareHardwareTextureStreaming(const RendererAssetIo *live_transport)
{
    if (resource_generation_id.empty() || !resident_generation_lease)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Hardware asset streaming requires the actual resident Generation lease.");
    if (auto result = UpdateResidencyBudget(); !result) return result;
    if (live_transport && live_transport->NativeEnabled() && !config.packed_content)
    {
        if (auto result = asset_io.ShareTransportFrom(*live_transport, residency); !result) return result;
    }
    else if (auto result = asset_io.Initialize(HS_WITH_DIRECTSTORAGE != 0 && !config.packed_content, 32u * 1024u * 1024u, residency); !result)
        return result;
    if (auto result = texture_streaming.Initialize(device.Get(), allocator, config.texture_quality,
        QualityProfileFor(config.texture_quality).texture_finest_mip, config.texture_budget_mib, texture_streaming_capabilities, kFrameCount, config.packed_textures); !result) return result;
    if (auto result = texture_feedback.Initialize(device.Get(), allocator,
        texture_streaming_capabilities.sampler_feedback && texture_streaming_capabilities.tiled_resources,
        kFrameCount); !result) return result;
    const auto register_texture = [&](const std::string &id, const render_data::TextureDescription &description,
        AllocationResource &fallback, ResidencyTicket existing_ticket, TextureStreamId &stream_id) -> Result {
        if (!fallback.resource || !fallback.allocation ||
            fallback.resource->GetDesc().MipLevels > description.mip_count)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Selected texture fallback is incomplete.");
        if (auto checked = fallback.EnsureNativeContract(device.Get()); !checked) return checked;
        TextureStreamRegistration registration;
        registration.generation = resource_generation_id; registration.logical_id = id;
        if (auto result = ResolveRendererAsset(config, id, registration.source); !result) return result;
        registration.generation_lease = resident_generation_lease; registration.description = description;
        if (config.packed_content) registration.compiled_texture = FindRendererTexture(config, MakeAssetId(id));
        registration.fallback = fallback.resource;
        registration.fallback_contract = *fallback.NativeContract();
        registration.fallback_graph_access = fallback.graph_access;
        registration.fallback_first_mip = description.mip_count - fallback.resource->GetDesc().MipLevels;
        registration.fallback_allocation_bytes = fallback.allocation->GetSize();
        registration.fallback_residency = existing_ticket; registration.pinned = description.mip_count == 1;
        registration.opacity_sampling = false;
        for (std::size_t material = 0; material < environment_material_bindings.size(); ++material)
        {
            if (environment_material_variants[material] != render_data::EnvironmentMaterialVariant::MaskedTwoSided)
                continue;
            const auto &bindings = environment_material_bindings[material];
            for (std::size_t slot = 0; slot < bindings.product_ids.size(); ++slot)
                if ((bindings.opacity_texture_mask & (1u << slot)) && bindings.product_ids[slot] == id)
                {
                    registration.opacity_sampling = true;
                    break;
                }
            if (registration.opacity_sampling) break;
        }
        return texture_streaming.Register(std::move(registration), residency, stream_id);
    };
    for (std::uint32_t index = 0; index < kEnvironmentTextureCount; ++index)
        if (auto result = register_texture(environment_texture_ids[index], environment_texture_descriptions[index],
            environment_textures[index], index == 0 ? environment_fallback_residency : ResidencyTicket{},
            environment_texture_stream_ids[index]); !result) return result;
    environment_material_texture_stream_ids.resize(environment_material_textures.size());
    for (std::size_t index = 0; index < environment_material_textures.size(); ++index)
    {
        auto &texture = environment_material_textures[index];
        if (auto result = register_texture(texture.product_id, texture.description, texture.texture, {},
            environment_material_texture_stream_ids[index]); !result) return result;
    }
    preferred_texture_scene.reset();
    preferred_texture_materials.fill(false);
    preferred_texture_initialized = false;
    material_texture_stream_ids = {};
    for (std::size_t material = 0; material < environment_material_bindings.size(); ++material)
        for (std::size_t slot = 0; slot < render_data::kMaterialTextureSlots; ++slot)
        {
            const auto index = environment_material_bindings[material].texture_indices[slot];
            if (index == UINT32_MAX) continue;
            if (index >= kEnvironmentTextureCount && index - kEnvironmentTextureCount >= environment_material_texture_stream_ids.size())
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Material texture alias has no selected owner.");
            material_texture_stream_ids[material * render_data::kMaterialTextureSlots + slot] = index < kEnvironmentTextureCount
                ? environment_texture_stream_ids[index] : environment_material_texture_stream_ids[index - kEnvironmentTextureCount];
        }
    return texture_streaming.Configure(config.texture_quality, QualityProfileFor(config.texture_quality).texture_finest_mip, config.texture_budget_mib);
}

Result D3D12Renderer::Impl::UpdateResidencyBudget()
{
    const auto now = std::chrono::steady_clock::now();
    const auto recent = [&](auto attempted) {
        return attempted != std::chrono::steady_clock::time_point{} && now - attempted < std::chrono::seconds(1);
    };
    if (recent(local_budget_observation.attempted_at) &&
        (adapter_unified_memory || recent(nonlocal_budget_observation.attempted_at)) &&
        recent(host_budget_observation.attempted_at)) return Result::Success();
    return RefreshBudgetObservations();
}

Result D3D12Renderer::Impl::PrepareEnvironmentStreaming()
{
    if (resource_generation_id.empty())
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Texture streaming requires the selected content Generation ID.");
    return Result::Success();
}

void D3D12Renderer::Impl::MarkEnvironmentStreamingUse(
    std::uint32_t frame_slot, const CompletionSet &completion) noexcept
{
    for (auto &stream : environment_geometry_streams)
        if (frame_slot < kFrameCount && stream.frame_generation[frame_slot] == 1)
            stream.prior_use.Merge(completion);
}

void D3D12Renderer::Impl::ResetEnvironmentStreaming() noexcept
{
    preferred_texture_scene.reset();
    preferred_texture_materials.fill(false);
    preferred_texture_initialized = false;
    environment_fallback_residency = {};
    for (auto &stream : environment_geometry_streams) stream.ResetRuntime();
    environment_material_textures.clear();
    environment_material_bindings = {};
    retired_streaming_textures.clear();
    streaming.Reset();
    residency.Reset();
}
} // namespace hs
