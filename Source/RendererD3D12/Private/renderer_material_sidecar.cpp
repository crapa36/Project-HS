#include "renderer_impl.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>

namespace hs
{
Result D3D12Renderer::Impl::LoadMaterialSurfaceProfile(const RendererConfig &candidate)
{
    try
    {
        decltype(material_surface_layouts) layouts{};
        decltype(material_headers) headers{};
        std::uint32_t mask{};
        for (std::size_t ordinal = 0; ordinal < kResidentMaterialCount; ++ordinal)
        {
            const bool hero = ordinal >= kAuthoredMaterialNames.size();
            const std::string name(hero ? kHeroMaterialNames[ordinal - kAuthoredMaterialNames.size()]
                                        : kAuthoredMaterialNames[ordinal]);
            const auto id = render_data::MaterialProduct(name);
            if (candidate.packed_content || candidate.development_catalog)
            {
                const auto handle = FindRendererMaterial(candidate, MakeAssetId(id));
                if (!handle) throw std::runtime_error("Missing compiled material surface: " + id);
                const auto &material = candidate.packed_materials[handle.value];
                if (material.kind != (hero ? render_data::MaterialKind::Hero : render_data::MaterialKind::Environment))
                    throw std::runtime_error("Compiled material surface has the wrong domain: " + id);
                layouts[ordinal] = material.surface;
                mask |= layouts[ordinal].varying_mask;
                continue;
            }
            RendererAssetSource path;
            if (auto resolved = ResolveRendererAsset(candidate, id, path); !resolved) return resolved;
            auto &header = headers[ordinal];
            if (auto read = ReadBinary(path, header.bytes); !read) return read;
            header.view = render_data::ReadPackage(header.bytes, MakeAssetId(id));
            const auto &package = header.view;
            const auto chunk = std::ranges::find(package.chunks,
                render_data::ChunkType::MaterialSurfaceLayout, &render_data::ChunkView::type);
            if (chunk == package.chunks.end() || !chunk->required || chunk->version != 1 ||
                chunk->count != 1 || chunk->stride != render_data::kMaterialSurfaceLayoutStride)
                throw std::runtime_error("required material surface profile is missing: " + id);
            layouts[ordinal] = render_data::DecodeMaterialSurfaceLayout(chunk->bytes);
            mask |= layouts[ordinal].varying_mask;
        }
        const auto layout = render_data::ComputeMaterialSidecarLayout(mask);
        material_surface_layouts = std::move(layouts);
        material_sidecar_layout = layout;
        material_headers = std::move(headers);
        return Result::Success();
    }
    catch (const std::exception &error)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", error.what());
    }
}

DXGI_FORMAT D3D12Renderer::Impl::MaterialSidecarFormat(std::size_t plane) const noexcept
{
    const auto words = plane < material_sidecar_layout.plane_count
        ? std::min(4u, material_sidecar_layout.word_count - static_cast<std::uint32_t>(plane) * 4u) : 4u;
    return words == 1 ? DXGI_FORMAT_R32_UINT : words == 2 ? DXGI_FORMAT_R32G32_UINT
        : DXGI_FORMAT_R32G32B32A32_UINT;
}

Result D3D12Renderer::Impl::PrepareMaterialSidecars(std::array<AllocationResource, 2> &targets,
                                                  std::uint32_t width, std::uint32_t height)
{
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    for (std::size_t plane = 0; plane < targets.size(); ++plane)
    {
        targets[plane].Reset();
        if (plane >= material_sidecar_layout.plane_count) continue;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize = description.MipLevels = 1;
        description.Format = MaterialSidecarFormat(plane);
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{description.Format};
        const auto queried = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                        &support, sizeof(support));
        if (FAILED(queried)) return HResultFailure("Query material sidecar format", queried);
        const auto required = D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
        if ((support.Support1 & required) != required ||
            (config.surface_frontend == SurfaceFrontend::VisibilityBuffer &&
             !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Selected material sidecar format is unsupported.");
        description.SampleDesc.Count = 1;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (config.surface_frontend == SurfaceFrontend::VisibilityBuffer)
            description.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = description.Format;
        if (auto created = CreateAllocation(targets[plane], allocation, description,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear); !created) return created;
    }
    return Result::Success();
}

void D3D12Renderer::Impl::WriteMaterialSidecarDescriptors(OwnedView &owner, std::uint32_t frame) noexcept
{
    for (std::size_t plane = 0; plane < owner.targets.material_sidecars.size(); ++plane)
    {
        auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (kFrameCount + frame * kRtvBankStride + kMaterialSidecarRtvOffset + plane) * rtv_stride;
        D3D12_RENDER_TARGET_VIEW_DESC rtv_view{};
        rtv_view.Format = MaterialSidecarFormat(plane);
        rtv_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(owner.targets.material_sidecars[plane].resource.Get(), &rtv_view, rtv);
            auto srv = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            srv.ptr += (frame * kTextureDescriptorCount + kSurfaceDescriptorOffset + 2 + plane) * srv_stride;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv_view{};
            srv_view.Format = MaterialSidecarFormat(plane);
            srv_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv_view.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(owner.targets.material_sidecars[plane].resource.Get(), &srv_view, srv);
            srv = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            srv.ptr += (frame * kTextureDescriptorCount + kVisibilityUavDescriptorOffset + 12 + plane) * srv_stride;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav_view{};
            uav_view.Format = MaterialSidecarFormat(plane);
            uav_view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            auto *resource = config.surface_frontend == SurfaceFrontend::VisibilityBuffer
                ? owner.targets.material_sidecars[plane].resource.Get() : nullptr;
            device->CreateUnorderedAccessView(resource, nullptr, &uav_view, srv);
    }
}
} // namespace hs
