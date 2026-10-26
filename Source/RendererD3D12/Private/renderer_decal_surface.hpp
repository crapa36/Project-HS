#pragma once

#include "d3d12_resources.hpp"
#include "view_state.hpp"
#include <hs/render_data/material_schema.hpp>

#include <DirectXMath.h>
#include <array>
#include <functional>
#include <memory>
#include <string_view>

namespace hs
{
class RendererPipelineCache;
// Append these ranges to existing root table32, all in space36. The service
// writes its descriptors into a waited frame bank in the caller's main heap.
inline constexpr std::uint32_t kDecalSurfaceCbvOffset = 0;
inline constexpr std::uint32_t kDecalSurfaceSrvOffset = 1;
inline constexpr std::uint32_t kDecalSurfaceSrvCount = 10;
inline constexpr std::uint32_t kDecalSurfaceUavOffset = 11;
inline constexpr std::uint32_t kDecalSurfaceUavCount = 4;
inline constexpr std::uint32_t kDecalSurfaceExportOffset = 15;
inline constexpr std::uint32_t kDecalSurfaceExportCount = 4;
inline constexpr std::uint32_t kDecalSurfaceDescriptorCount = 19;
inline constexpr std::uint32_t kDecalSurfaceRootTable = 32;
inline constexpr std::uint32_t kDecalSurfaceCommandStride = 96;

struct alignas(16) DecalSurfaceConstants
{
    DirectX::XMFLOAT4X4 inverse_view_projection{};
    DirectX::XMFLOAT4 view_origin{};
    DirectX::XMFLOAT4 render_size{};
    DirectX::XMUINT4 info{};
    std::array<DirectX::XMUINT4, 2> receivers{};
    DirectX::XMUINT4 parameter_bases{};
};
static_assert(sizeof(DecalSurfaceConstants) == 160);
static_assert(offsetof(DecalSurfaceConstants, receivers) == 112);

struct DecalSurfaceGuide { ResourceVersion version; ID3D12Resource *resource{}; };
struct DecalSurfaceMaterial
{
    render_data::DecalMaterialPackageData package;
    std::uint32_t parameter_base{}, parameter_count{};
};
struct DecalSurfaceInputs
{
    DecalSurfaceGuide depth, metadata, base, normal, material;
    // The existing admitted upload list contains six float4/uint4 rows, 96
    // bytes per command. Metadata.z now selects the packaged decal material.
    DecalSurfaceGuide commands, parameters, gradient;
    std::array<DecalSurfaceGuide, 2> textures;
    std::array<DecalSurfaceMaterial, 2> materials;
    std::uint64_t command_byte_offset{};
    std::uint32_t command_count{}, parameter_slot_count{};
    ViewExtent render_active;
    std::uint32_t frame_slot{};
    CompletionSet completed;
    // CPU suffix base is the service's 19-descriptor range. GPU base is the
    // complete existing main descriptor table, not the suffix start.
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor_suffix_cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE main_table_gpu{};
    ID3D12DescriptorHeap *main_heap{};
};
struct DecalSurfaceOutputs
{
    DecalSurfaceGuide base, normal, material, emission;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> export_srvs{};
    bool active{};
};
struct DecalSurfaceLayout
{
    bool valid{}, active{};
    ViewExtent capacity{};
    std::uint32_t frame_slots{};
    std::array<ResourceVersion,10> guides{};
    std::array<TextureHandle,4> outputs{};
    std::array<ResourceVersion,4> written{};
    BufferHandle constants;
    std::uint32_t pass{};
};
struct DecalSurfaceTargets;
class DecalSurfaceCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<DecalSurfaceTargets> targets_;
    bool prepared_{};
    friend class DecalSurfaceSystem;
};

// The four out-of-place images form one common Surface packet. Empty frames
// return the input packet and no emission guide; callers branch on active and
// must not read an unwritten export. All GPU queue completions own lifetime.
class DecalSurfaceSystem
{
  public:
    using ShaderReader = std::function<Result(std::string_view, std::vector<std::byte> &)>;
    [[nodiscard]] Result Prepare(ID3D12Device *, D3D12MA::Allocator *, ID3D12RootSignature *, RendererPipelineCache &,
        ViewExtent capacity, std::uint32_t frame_slots, bool has_admitted_work,
        const ShaderReader &, DecalSurfaceCandidate &) const;
    [[nodiscard]] Result Commit(DecalSurfaceCandidate &&, const CompletionSet &);
    [[nodiscard]] Result Register(RenderGraphBuilder &, const DecalSurfaceInputs &,
        DecalSurfaceConstants, DecalSurfaceOutputs &, DecalSurfaceLayout *layout=nullptr);
    [[nodiscard]] bool MatchesLayout(const DecalSurfaceInputs &,const DecalSurfaceLayout &)const noexcept;
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &,const DecalSurfaceInputs &,
        DecalSurfaceConstants,const DecalSurfaceLayout &,DecalSurfaceOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &, const CompletionSet &, bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    void SwapState(DecalSurfaceSystem &other) noexcept { targets_.swap(other.targets_); }
  private:
    [[nodiscard]] Result RegisterFrame(RenderGraphBuilder &,const DecalSurfaceInputs &,
        DecalSurfaceConstants,DecalSurfaceOutputs &,DecalSurfaceLayout *,const DecalSurfaceLayout *);
    std::shared_ptr<DecalSurfaceTargets> targets_;
    const RenderGraphBuilder *pending_graph_{};
    std::array<ResourceHandle, 4> pending_outputs_{};
    ResourceHandle pending_constants_{};
    std::uint32_t pending_slot_{};
};
} // namespace hs
