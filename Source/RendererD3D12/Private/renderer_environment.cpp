#include "renderer_impl.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/core/sha256.hpp>
#include <hs/render_data/texture_package.hpp>

#include <stdexcept>
#include <bit>
#include <cmath>
#include <cstring>

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
    material_parameter_upload.Reset();
    std::vector<render_data::MaterialParameterSlot> slots;
    try
    {
        const auto material_count = scope == MaterialPreparationScope::StaticEnvironment
            ? kAuthoredMaterialNames.size() : kResidentMaterialCount;
        for (std::size_t ordinal = 0; ordinal < material_count; ++ordinal)
        {
            const bool hero = ordinal >= kAuthoredMaterialNames.size();
            const auto name = hero ? kHeroMaterialNames[ordinal - kAuthoredMaterialNames.size()]
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
    // Preparation completed and drained all readers. Publishing every frame
    // slot together prevents raster, shadow and compute observing mixed values.
    material_parameters = std::move(material_parameter_candidate);
    material_parameter_slot_count = material_parameter_candidate_slot_count;
    ++material_parameter_revision;
    material_parameter_bases = material_parameter_candidate_bases;
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
        {"terrain_histogram", DXGI_FORMAT_R32G32B32A32_FLOAT,4,1,256,16},
    };
    static_assert(std::size(specs) == kEnvironmentTextureCount);
    constexpr UINT kStreamTextureIndex = 0;
    constexpr UINT kFallbackFirstMip = 4;
    environment_stream.ResetRuntime();
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
            "Invalid environment texture: " + path.path.string()); };
        {
            try
            {
                package = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return ReadRendererTexturePackage(config, logical_id, storage); });
            }
            catch (const std::exception &error)
            {
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Invalid environment texture package " + path.path.string() + ": " + error.what());
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
        if (texture_index == kStreamTextureIndex)
        {
            environment_stream.source = path;
            environment_stream.logical_id = logical_id;
            environment_stream.source_id = source_id;
        }
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
        environment_stream.fallback_residency = RegisterNativeAllocation(
            ResidencyCategory::Texture,
            environment_textures[kStreamTextureIndex].allocation->GetSize(), true,
            ResidencySegmentForHeap(D3D12_HEAP_TYPE_DEFAULT));
    if (!environment_stream.fallback_residency)
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
        if (path.range.byte_count != config.packed_bulks[texture.bulk_index].range.byte_count)
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
                                   "Invalid material texture footprint: " + path.path.string());
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
            if (environment_material_variants[material] == render_data::EnvironmentMaterialVariant::MaskedTwoSided &&
                std::ranges::find(environment_material_bindings[material].product_ids, id) !=
                    environment_material_bindings[material].product_ids.end())
            {
                registration.opacity_sampling = true;
                break;
            }
        return texture_streaming.Register(std::move(registration), residency, stream_id);
    };
    for (std::uint32_t index = 0; index < kEnvironmentTextureCount; ++index)
        if (auto result = register_texture(environment_texture_ids[index], environment_texture_descriptions[index],
            environment_textures[index], index == 0 ? environment_stream.fallback_residency : ResidencyTicket{},
            environment_texture_stream_ids[index]); !result) return result;
    environment_material_texture_stream_ids.resize(environment_material_textures.size());
    for (std::size_t index = 0; index < environment_material_textures.size(); ++index)
    {
        auto &texture = environment_material_textures[index];
        if (auto result = register_texture(texture.product_id, texture.description, texture.texture, {},
            environment_material_texture_stream_ids[index]); !result) return result;
    }
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
    if (environment_stream.source.empty() || environment_stream.logical_id.empty())
        return Result::Success();
    if (resource_generation_id.empty())
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Texture streaming requires the selected content Generation ID.");
    return Result::Success();
}

Result D3D12Renderer::Impl::DemandEnvironmentTexture(std::uint32_t priority)
{
    auto &stream = environment_stream;
    if (stream.source.empty() || stream.logical_id.empty()) return Result::Success();
    const auto error = std::bit_cast<float>(priority);
    if (priority && (!std::isfinite(error) || error <= 0.0f))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Texture demand requires positive ordered error.");
    const auto demand_frame = rendered_submission_count + 1u;
    if (stream.last_demand_frame != demand_frame) stream.priority = 0u;
    stream.priority = std::max(stream.priority, priority);
    stream.last_demand_frame = demand_frame;
    if (stream.request && !stream.retiring) static_cast<void>(streaming.UpdatePriority(stream.request, stream.priority));
    if (!priority || stream.request || stream.fine_resident || stream.retiring) return Result::Success();
    stream.request = streaming.Request(stream.logical_id, "mips:0-3", resource_generation_id, stream.priority);
    if (!stream.request)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Could not create environment texture streaming request.");
    ++stream.requests;
    return Result::Success();
}

Result D3D12Renderer::Impl::PumpEnvironmentStreaming(std::uint32_t frame_slot)
{
    if (frame_slot >= kFrameCount)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Environment streaming frame slot is out of range.");
    if (auto budget = UpdateResidencyBudget(); !budget) return budget;
    const auto completed = graph_queue_executor.Completed();

    auto &stream = environment_stream;
    if (stream.retiring && stream.retirement.IsSatisfiedBy(completed))
    {
        stream.candidate.Reset();
        static_cast<void>(residency.ReleaseNow(stream.candidate_residency));
        stream.candidate_residency = {}; stream.request = {};
        stream.retiring = false; stream.retirement = {}; stream.fine_use = {};
        ++stream.reclaims;
    }

    if (environment_stream.upload_submitted &&
        environment_stream.upload_completion.IsSatisfiedBy(completed))
    {
        if (const auto *page = streaming.Find(environment_stream.request);
            page && page->state == StreamingPageState::Uploading)
        {
            if (!streaming.TryPublish(environment_stream.request,
                                      resource_generation_id, completed))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Completed texture upload could not be published.");
            if (stream.generation == UINT64_MAX)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Texture publication generation exhausted.");
            ++stream.generation;
            stream.fine_resident = true;
            stream.fine_use = stream.upload_completion;
            ++stream.publications;
        }
        if (environment_stream.upload.resource)
        {
            environment_stream.upload.Reset();
            static_cast<void>(residency.ReleaseNow(stream.upload_residency));
            stream.upload_residency = {};
            ++environment_stream.reclaims;
        }
        environment_stream.upload_submitted = false;
    }
    static_cast<void>(residency.Collect(completed));
    static_cast<void>(streaming.Collect(completed));

    if (stream.frame_generation[frame_slot] != stream.generation)
    {
        auto *resource = stream.fine_resident && !stream.retiring
            ? stream.candidate.resource.Get() : environment_textures[0].resource.Get();
        const auto description = resource->GetDesc();
        WriteEnvironmentTextureDescriptor(frame_slot, 0, resource,
            description.Format, description.MipLevels, description.DepthOrArraySize);
        stream.frame_generation[frame_slot] = stream.generation;
        PublishEnvironmentMaterialTextures(frame_slot);
    }

    const auto *page = streaming.Find(stream.request);
    if (!page) return Result::Success();
    if (page->state == StreamingPageState::Requested)
    {
        if (!stream.priority || stream.last_demand_frame < rendered_submission_count) return Result::Success();
        std::uint64_t file_bytes{};
        if (auto sized = stream.source.ByteCount(file_bytes); !sized) return sized;
        if (!file_bytes || file_bytes > UINT64_MAX / 3u ||
            file_bytes > std::numeric_limits<std::size_t>::max() ||
            file_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Texture stream read size overflow.");
        if (!residency.CanStage(file_bytes * 3u)) return Result::Success();
        stream.decoded_residency = residency.ReserveHost(file_bytes);
        stream.decode_reservation = residency.ReserveHost(file_bytes * 2u);
        if (!stream.decoded_residency || !stream.decode_reservation)
        {
            static_cast<void>(residency.ReleaseNow(stream.decoded_residency));
            static_cast<void>(residency.ReleaseNow(stream.decode_reservation));
            stream.decoded_residency = {}; stream.decode_reservation = {};
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Texture stream host reservation failed.");
        }
        stream.read_bytes = file_bytes;
        bool submitted{};
        const auto queued = asset_io.Enqueue({stream.request, resource_generation_id,
                stream.logical_id, stream.source, file_bytes, resident_generation_lease},
                streaming, residency, stream.decoded_residency, submitted);
        if (!submitted)
        {
            static_cast<void>(residency.ReleaseNow(stream.decoded_residency));
            static_cast<void>(residency.ReleaseNow(stream.decode_reservation));
            stream.decode_reservation = {};
        }
        stream.decoded_residency = {}; // IO owns submitted backing and its ticket.
        if (!queued) return queued;
        return Result::Success();
    }
    if (page->state == StreamingPageState::Reading)
    {
        AssetReadCompletion read;
        if (auto result = asset_io.Poll(stream.request, streaming, residency, read); !result) return result;
        if (!read.ready) return Result::Success();
        if (read.cancelled) return Result::Success();
        stream.decoded_storage = std::move(read.bytes);
        stream.decoded_residency = read.storage;
    }
    else if (page->state != StreamingPageState::Decoded) return Result::Success();

    render_data::TexturePackageView package;
    try
    {
        package = MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return ReadRendererTexturePackage(config, environment_stream.logical_id, stream.decoded_storage); });
    }
    catch (const std::exception &error)
    {
        static_cast<void>(streaming.Cancel(environment_stream.request));
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               std::string("Environment texture stream decode failed: ") +
                                   error.what());
    }
    constexpr std::uint32_t kWidth = 2048;
    constexpr std::uint32_t kHeight = 2048;
    constexpr std::uint32_t kArraySize = 4;
    constexpr std::uint32_t kMipCount = 12;
    if (package.description.width != kWidth || package.description.height != kHeight ||
        package.description.array_size != kArraySize ||
        package.description.mip_count != kMipCount ||
        package.description.format != static_cast<std::uint32_t>(DXGI_FORMAT_BC7_UNORM) ||
        !RendererTextureDependencyMatches(config, environment_stream.logical_id, package, environment_stream.source_id) ||
        package.subresources.size() != kArraySize * kMipCount)
    {
        static_cast<void>(streaming.Cancel(environment_stream.request));
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Streamed terrain Gaussian package shape changed.");
    }
    if (page->state == StreamingPageState::Reading && !streaming.CompleteRead(environment_stream.request,
                                resource_generation_id))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Environment texture stream read completion was stale.");
    if (!stream.priority || stream.last_demand_frame < rendered_submission_count)
        return Result::Success(); // Preserve the decoded package until a view needs it again.

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = kWidth;
    description.Height = kHeight;
    description.DepthOrArraySize = kArraySize;
    description.MipLevels = kMipCount;
    description.Format = DXGI_FORMAT_BC7_UNORM;
    description.SampleDesc = {1, 0};
    const auto subresource_count = kArraySize * kMipCount;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresource_count);
    std::vector<UINT> rows(subresource_count);
    std::vector<UINT64> row_sizes(subresource_count);
    UINT64 upload_size{};
    device->GetCopyableFootprints(&description, 0, subresource_count, 0,
                                  footprints.data(), rows.data(), row_sizes.data(),
                                  &upload_size);
    const auto allocation_info = device->GetResourceAllocationInfo(0, 1, &description);
    const auto upload_description = BufferDescription(upload_size);
    const auto upload_allocation = device->GetResourceAllocationInfo(0, 1, &upload_description).SizeInBytes;
    if (allocation_info.SizeInBytes == UINT64_MAX || upload_allocation == UINT64_MAX)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Streamed texture allocation size overflow.");
    SampleAllocatorBudgetStats(false, false);
    ScopedGpuAdmission admission(residency, allocation_info.SizeInBytes, upload_allocation);
    if (!admission)
    {
        return TryRelieveStreamingPressure(allocation_info.SizeInBytes, upload_allocation, stream.priority);
    }

    AllocationResource candidate;
    AllocationResource upload;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (auto created = CreateAllocation(candidate, allocation, description,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
        !created)
        return created;
    allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    if (auto created = CreateAllocation(upload, allocation,
                                        BufferDescription(upload_size),
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
        !created)
        return created;
    std::byte *mapped{};
    D3D12_RANGE no_read{};
    const auto mapped_result =
        upload.resource->Map(0, &no_read, reinterpret_cast<void **>(&mapped));
    if (FAILED(mapped_result))
        return HResultFailure("Map streamed environment texture upload", mapped_result);
    for (UINT subresource = 0; subresource < subresource_count; ++subresource)
    {
        const auto &source = package.subresources[subresource];
        if (source.array_slice != subresource / kMipCount ||
            source.mip_level != subresource % kMipCount ||
            row_sizes[subresource] != source.row_pitch ||
            static_cast<UINT64>(rows[subresource]) * source.row_pitch !=
                source.slice_pitch ||
            source.bytes.size() != source.slice_pitch)
        {
            upload.resource->Unmap(0, nullptr);
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Streamed terrain Gaussian subresource layout changed.");
        }
        for (UINT row = 0; row < rows[subresource]; ++row)
            std::memcpy(
                mapped + footprints[subresource].Offset +
                    static_cast<std::size_t>(row) *
                        footprints[subresource].Footprint.RowPitch,
                source.bytes.data() +
                    static_cast<std::size_t>(row) * source.row_pitch,
                static_cast<std::size_t>(row_sizes[subresource]));
    }
    upload.resource->Unmap(0, nullptr);

    const auto candidate_ticket = RegisterNativeAllocation(
        ResidencyCategory::Texture, candidate.allocation->GetSize(), false,
        ResidencySegmentForHeap(D3D12_HEAP_TYPE_DEFAULT));
    const auto upload_ticket = RegisterNativeAllocation(
        ResidencyCategory::Staging, upload.allocation->GetSize(), false,
        ResidencySegmentForHeap(D3D12_HEAP_TYPE_UPLOAD));
    if (!candidate_ticket || !upload_ticket)
    {
        if (candidate_ticket) static_cast<void>(residency.ReleaseNow(candidate_ticket));
        if (upload_ticket) static_cast<void>(residency.ReleaseNow(upload_ticket));
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Could not account streamed texture allocations.");
    }
    admission.Complete();
    if (!streaming.BeginUpload(environment_stream.request,
                               resource_generation_id))
    {
        static_cast<void>(residency.ReleaseNow(candidate_ticket));
        static_cast<void>(residency.ReleaseNow(upload_ticket));
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Environment texture stream upload transition failed.");
    }
    environment_stream.candidate = std::move(candidate);
    environment_stream.upload = std::move(upload);
    environment_stream.footprints = std::move(footprints);
    environment_stream.subresource_count = subresource_count;
    environment_stream.candidate_residency = candidate_ticket;
    environment_stream.upload_residency = upload_ticket;
    environment_stream.upload_pending = true;
    for (const auto ticket : {stream.decoded_residency, stream.decode_reservation})
        static_cast<void>(residency.ReleaseNow(ticket));
    stream.decoded_residency = {}; stream.decode_reservation = {}; stream.read_bytes = 0;
    std::vector<std::byte>{}.swap(stream.decoded_storage);
    return Result::Success();
}

void D3D12Renderer::Impl::MarkEnvironmentStreamingUse(
    std::uint32_t frame_slot, const CompletionSet &completion) noexcept
{
    if (frame_slot < kFrameCount)
    {
        if (environment_stream.fine_resident && !environment_stream.retiring &&
            environment_stream.frame_generation[frame_slot] == environment_stream.generation)
            environment_stream.fine_use.Merge(completion);
        else environment_stream.fallback_use.Merge(completion);
    }
    for (auto &stream : environment_geometry_streams)
        if (frame_slot < kFrameCount && stream.frame_generation[frame_slot] == 1)
            stream.prior_use.Merge(completion);
}

void D3D12Renderer::Impl::ResetEnvironmentStreaming() noexcept
{
    environment_stream.ResetRuntime();
    for (auto &stream : environment_geometry_streams) stream.ResetRuntime();
    environment_material_textures.clear();
    environment_material_bindings = {};
    retired_streaming_textures.clear();
    streaming.Reset();
    residency.Reset();
}
} // namespace hs
