#pragma once
#include "render_graph.hpp"
#include <hs/core/result.hpp>
#include <hs/render_data/opacity_micromap_package.hpp>
#include <d3d12.h>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace D3D12MA {class Allocator;}
namespace hs
{
using RayOmmProductReader=std::function<Result(std::string_view,std::vector<std::byte> &)>;
struct RayOmmProfile
{
    bool enabled{};
    bool enhanced_barriers{};
    std::uint32_t frame_slots{},maximum_subdivision{4};
    // Covers all live arrays/input/scratch/upload including retired generations
    // and the old service overlap while staging a replacement.
    std::uint64_t native_byte_budget{},old_service_bytes{};
};
struct RayOmmGeometryRequest
{
    std::string product;
    AssetId product_asset{};
    render_data::OpacityMicromapIdentity identity;
    std::uint64_t generation{};
    std::shared_ptr<const void> generation_owner;
    std::span<const std::uint32_t> actual_opacity_words;
};
struct RayOmmBinding
{
    ResourceVersion array_version{},index_version{};
    ID3D12Resource *array_resource{},*index_resource{};
    render_data::OpacityMicromapIdentity identity;
    std::uint64_t generation{},known_cells{},unknown_cells{};
    std::uint32_t index_count{},base_location{};
    std::uint64_t index_byte_offset{};
    std::shared_ptr<const void> owner;
    explicit operator bool()const noexcept{return array_resource&&index_resource&&owner;}
};
struct RayOmmStatus
{
    bool enabled{},ready{},applied{},usable{true};
    std::uint64_t allocated_bytes{},linked_triangles{},known_cells{},unknown_cells{};
    std::uint32_t unavailable_geometries{};
    std::string reason{"Opacity micromaps are Off."};
};
struct RayOmmStorage;
class RayOmmCandidate
{
  public:[[nodiscard]] std::uint64_t AllocatedBytes()const noexcept;
  private:std::shared_ptr<RayOmmStorage> storage_;friend class RayOpacityMicromapSystem;
};
class RayOpacityMicromapSystem
{
  public:
    RayOpacityMicromapSystem();~RayOpacityMicromapSystem();
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,const RayOmmProfile &,
        const RayOmmProductReader &,RayOmmCandidate &)const;
    [[nodiscard]] Result Commit(RayOmmCandidate &&,const CompletionSet &);
    [[nodiscard]] Result Register(RenderGraphBuilder &,std::span<const RayOmmGeometryRequest>,
        std::uint32_t frame_slot,const CompletionSet &completed,std::vector<RayOmmBinding> &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] RayOmmStatus Status()const;
    [[nodiscard]] Result SetNativeByteBudget(std::uint64_t);
    [[nodiscard]] std::uint64_t AllocatedBytes()const noexcept;
    void SwapState(RayOpacityMicromapSystem &other)noexcept{impl_.swap(other.impl_);}
  private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
