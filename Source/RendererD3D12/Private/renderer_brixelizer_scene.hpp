#pragma once

#include "renderer_brixelizer_gi.hpp"
#include <hs/render_data/geometry_package.hpp>
#include <hs/render_data/material_schema.hpp>

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace hs
{
[[nodiscard]] constexpr bool IsBrixelizerVertexStride(std::uint32_t stride) noexcept
{ return stride >= 12 && stride <= 63 && stride % 4 == 0; }

// Refresh only when frame registration replaced projected/placeholder versions.
[[nodiscard]] inline bool RefreshBrixelizerGeometryReadSet(
    BrixelizerGeometryRange geometries,BrixelizerFrameRecordData &record)
{
    bool changed=record.geometry_reads.size()!=geometries.size()*2u;
    if(!changed)
        for(std::size_t i=0;i<geometries.size();++i)
        {
            const auto versions=geometries.VersionsAt(i);
            if(record.geometry_reads[i*2u]!=SceneGeometryReadToken{versions.vertex,Access::ComputeRead}||
                record.geometry_reads[i*2u+1u]!=SceneGeometryReadToken{versions.index,Access::ComputeRead})
            {changed=true;break;}
        }
    if(!changed)return false;
    record.geometry_reads.clear();record.geometry_reads.reserve(geometries.size()*2u);
    for(std::size_t i=0;i<geometries.size();++i)
    {
        const auto versions=geometries.VersionsAt(i);
        record.geometry_reads.push_back({versions.vertex,Access::ComputeRead});
        record.geometry_reads.push_back({versions.index,Access::ComputeRead});
    }
    record.unique_geometry_reads.clear();record.unique_geometry_reads.reserve(record.geometry_reads.size());
    for(const auto &read:record.geometry_reads)if(read.version.resource)record.unique_geometry_reads.push_back(read);
    const auto key=[](const SceneGeometryReadToken &read) {
        return std::tuple{read.version.resource.value,read.version.resource.kind,read.version.ordinal,read.access};
    };
    std::ranges::sort(record.unique_geometry_reads,[&](const auto &left,const auto &right) {
        return key(left)<key(right);
    });
    record.unique_geometry_reads.erase(std::unique(record.unique_geometry_reads.begin(),
        record.unique_geometry_reads.end()),record.unique_geometry_reads.end());
    return true;
}
[[nodiscard]] inline bool RefreshBrixelizerGeometryReadSet(
    std::span<const SoftwareSceneGeometry> geometries,BrixelizerFrameRecordData &record)
{ return RefreshBrixelizerGeometryReadSet({{},geometries,{}},record); }

// Validate the merged run shape and total once at the GI admission boundary.
[[nodiscard]] inline Result CountBrixelizerSceneRows(const BrixelizerSceneInputs &input,
    std::uint32_t &instance_count,std::uint32_t &immutable_count)
{
    instance_count=immutable_count=0;
    const auto immutable=input.immutable_section?input.immutable_section->Runs():std::span<const BrixelizerSceneRun>{};
    std::size_t a{},b{},previous_end{};
    while(a<immutable.size()||b<input.current_runs.size())
    {
        if(a<immutable.size()&&input.static_job_base>std::numeric_limits<std::size_t>::max()-immutable[a].job_begin)
            return Result::Failure(ErrorCode::InvalidArgument,"hs_renderer_d3d12","Brixelizer immutable run index overflows.");
        const auto immutable_begin=a<immutable.size()?input.static_job_base+immutable[a].job_begin:
            std::numeric_limits<std::size_t>::max();
        const auto current_begin=b<input.current_runs.size()?input.current_runs[b].job_begin:
            std::numeric_limits<std::size_t>::max();
        if(immutable_begin==current_begin)
            return Result::Failure(ErrorCode::InvalidArgument,"hs_renderer_d3d12","Brixelizer immutable and current runs overlap.");
        const bool use_immutable=immutable_begin<current_begin;
        const auto job_begin=use_immutable?immutable_begin:current_begin;
        const auto job_count=use_immutable?immutable[a].job_count:input.current_runs[b].job_count;
        const auto row_begin=use_immutable?immutable[a].instance_begin:input.current_runs[b].instance_begin;
        const auto row_count=use_immutable?immutable[a].instance_count:input.current_runs[b].instance_count;
        const auto rows=use_immutable?input.immutable_section->Instances():input.current_instances;
        if(!job_count||job_begin>std::numeric_limits<std::size_t>::max()-job_count||job_begin<previous_end||
            row_begin>rows.size()||row_count>rows.size()-row_begin||row_count>UINT32_MAX-instance_count||
            (use_immutable&&row_count>UINT32_MAX-immutable_count))
            return Result::Failure(ErrorCode::InvalidArgument,"hs_renderer_d3d12","Invalid or overflowing Brixelizer scene run.");
        previous_end=job_begin+job_count;
        instance_count+=static_cast<std::uint32_t>(row_count);
        if(use_immutable){immutable_count+=static_cast<std::uint32_t>(row_count);++a;}else ++b;
    }
    return Result::Success();
}

// Keep static rows owned by their immutable section and merge them with the
// waited-slot rows without flattening either source.
template<class Visitor>
[[nodiscard]] Result ForEachBrixelizerSceneInstance(const BrixelizerSceneInputs &input,
    bool skip_immutable,Visitor &&visitor)
{
    const auto immutable=input.immutable_section?input.immutable_section->Runs():std::span<const BrixelizerSceneRun>{};
    std::size_t a{},b{};
    std::uint32_t logical_index{};
    while(a<immutable.size()||b<input.current_runs.size())
    {
        const auto immutable_begin=a<immutable.size()?input.static_job_base+immutable[a].job_begin:
            std::numeric_limits<std::size_t>::max();
        const auto current_begin=b<input.current_runs.size()?input.current_runs[b].job_begin:
            std::numeric_limits<std::size_t>::max();
        const bool use_immutable=immutable_begin<current_begin;
        const auto row_begin=use_immutable?immutable[a].instance_begin:input.current_runs[b].instance_begin;
        const auto row_count=use_immutable?immutable[a].instance_count:input.current_runs[b].instance_count;
        const auto rows=use_immutable?input.immutable_section->Instances():input.current_instances;
        if(row_begin>rows.size()||row_count>rows.size()-row_begin)
            return Result::Failure(ErrorCode::InvalidArgument,"hs_renderer_d3d12","Brixelizer scene run exceeds its row owner.");
        if(skip_immutable&&use_immutable)logical_index+=static_cast<std::uint32_t>(row_count);
        else for(const auto &instance:rows.subspan(row_begin,row_count))
        {
            if(auto result=visitor(instance,logical_index,use_immutable);!result)return result;
            ++logical_index;
        }
        if(use_immutable)++a;else ++b;
    }
    return Result::Success();
}

[[nodiscard]] std::uint64_t BrixelizerSubmeshKey(std::uint64_t source_key, std::size_t part);

// A frame-local geometry identity. The stable key alone is insufficient for
// SkinCache and rewritten buffers, where one mesh key can have distinct byte
// ranges or graph versions in the same frame.
struct BrixelizerGeometryKey
{
    std::uint64_t stable_key{};
    ID3D12Resource *vertices{}, *indices{};
    std::uint64_t generation{}, current_revision{};
    std::uint32_t vertex_resource{}, vertex_kind{}, vertex_ordinal{};
    std::uint32_t index_resource{}, index_kind{}, index_ordinal{};
    std::uint64_t vertex_byte_offset{}, index_byte_offset{};
    std::uint32_t vertex_stride{}, vertex_count{}, index_count{};
    DXGI_FORMAT index_format{};
    bool dynamic{}, opaque{};
    bool operator==(const BrixelizerGeometryKey &) const = default;
};

struct BrixelizerGeometryKeyHash
{
    std::size_t operator()(const BrixelizerGeometryKey &key) const noexcept
    {
        std::size_t hash = std::hash<std::uint64_t>{}(key.stable_key);
        const auto mix = [&](std::uint64_t value) {
            const auto part = std::hash<std::uint64_t>{}(value);
            hash ^= part + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) +
                (hash << 6) + (hash >> 2);
        };
        mix(reinterpret_cast<std::uintptr_t>(key.vertices));
        mix(reinterpret_cast<std::uintptr_t>(key.indices));
        mix(key.generation); mix(key.current_revision);
        mix(key.vertex_resource); mix(key.vertex_kind); mix(key.vertex_ordinal);
        mix(key.index_resource); mix(key.index_kind); mix(key.index_ordinal);
        mix(key.vertex_byte_offset); mix(key.index_byte_offset);
        mix(key.vertex_stride); mix(key.vertex_count); mix(key.index_count);
        mix(static_cast<std::uint64_t>(key.index_format));
        mix(key.dynamic); mix(key.opaque);
        return hash;
    }
};

class BrixelizerGeometryLookup
{
public:
    BrixelizerGeometryLookup()=default;
    BrixelizerGeometryLookup(const BrixelizerGeometryLookup *prefix,std::uint32_t prefix_size) noexcept
        : prefix_(prefix),prefix_size_(prefix_size) {}
    [[nodiscard]] std::uint32_t FindOrAdd(const SoftwareSceneGeometry &geometry,
        std::vector<SoftwareSceneGeometry> &geometries)
    {
        const BrixelizerGeometryKey key{
            geometry.stable_key, geometry.vertices, geometry.indices,
            geometry.generation, geometry.current_revision,
            geometry.vertex_version.resource.value,
            static_cast<std::uint32_t>(geometry.vertex_version.resource.kind),
            geometry.vertex_version.ordinal,
            geometry.index_version.resource.value,
            static_cast<std::uint32_t>(geometry.index_version.resource.kind),
            geometry.index_version.ordinal,
            geometry.vertex_byte_offset, geometry.index_byte_offset,
            geometry.vertex_stride, geometry.vertex_count, geometry.index_count,
            geometry.index_format, geometry.dynamic, geometry.opaque};
        if(prefix_)
        {
            const auto existing=prefix_->entries_.find(key);
            if(existing!=prefix_->entries_.end())return existing->second;
        }
        const auto [found, inserted] = entries_.try_emplace(key,
            prefix_size_+static_cast<std::uint32_t>(geometries.size()));
        if (inserted) geometries.push_back(geometry);
        return found->second;
    }

private:
    std::unordered_map<BrixelizerGeometryKey, std::uint32_t,
        BrixelizerGeometryKeyHash> entries_;
    const BrixelizerGeometryLookup *prefix_{};
    std::uint32_t prefix_size_{};
};

// Shared domain precedence for homogeneous early exclusions and mixed-page expansion.
[[nodiscard]] BrixelizerGeometryDomain ClassifyBrixelizerDomain(std::uint32_t capabilities, bool masked, bool transmissive);
[[nodiscard]] Result AddBrixelizerExcludedCount(std::uint32_t &count, std::size_t submeshes);

// Preserve actual submesh ranges when a page mixes opaque and translucent
// character slots. The vectors are CPU registration inputs for this frame.
[[nodiscard]] Result AppendBrixelizerLeaf(
    const SoftwareSceneGeometry &, const SoftwareSceneInstance &,
    std::span<const render_data::GeometrySubmesh>,
    std::span<const std::uint64_t> submesh_keys,
    std::span<const render_data::CharacterMaterialBinding>,
    std::span<const render_data::MaterialSurfaceLayout> hero_layouts,
    std::uint32_t environment_capabilities, bool masked, bool transmissive,
    DirectX::XMFLOAT3 bounds_min, DirectX::XMFLOAT3 bounds_max,
    std::uint64_t material_revision,
    BrixelizerGeometryLookup &,
    std::vector<SoftwareSceneGeometry> &, std::vector<BrixelizerSceneInstance> &);
} // namespace hs
