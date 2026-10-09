#include <hs/render_data/opacity_micromap_bake.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace hs::render_data
{
OpacityMicromapData BakeOpacityMicromaps(const OpacityMicromapBakeInput &in)
{
    if(in.subdivision_level<1||in.subdivision_level>4||in.canonical_indices.empty()||
       in.canonical_indices.size()%3||in.canonical_indices.size()/3>UINT32_MAX||
       in.triangle_materials.size()!=in.canonical_indices.size()/3||!in.byte_budget)
        throw std::runtime_error("OMM bake: invalid canonical triangles, level or byte budget");
    const auto count=std::uint32_t(in.triangle_materials.size());
    const auto cells=1u<<(2u*in.subdivision_level);
    const auto stride=std::max(4u,(cells*2u+7u)/8u);
    const auto bytes=std::uint64_t(count)*(stride+8u+4u)+12u;
    if(bytes>in.byte_budget||std::uint64_t(count)*stride>UINT32_MAX)
        throw std::runtime_error("OMM bake: native input exceeds explicit budget/range");
    OpacityMicromapData out;out.states.resize(std::size_t(count)*stride);
    out.descriptors.reserve(count);out.indices.reserve(count);
    for(std::uint32_t triangle=0;triangle<count;++triangle)
    {
        bool finite_uv=true;
        for(unsigned corner=0;corner<3;++corner)
        {
            const auto vertex=in.canonical_indices[std::size_t(triangle)*3+corner];
            if(vertex>=in.canonical_uvs.size())throw std::runtime_error("OMM bake: canonical index exceeds original UVs");
            for(float uv:in.canonical_uvs[vertex])finite_uv&=std::isfinite(uv);
        }
        std::uint32_t state=3;
        const auto material=in.triangle_materials[triangle];
        if(finite_uv&&material<in.materials.size())
        {
            const auto &proof=in.materials[material];
            if(proof.proven&&std::isfinite(proof.alpha_min)&&std::isfinite(proof.alpha_max)&&
               std::isfinite(proof.scale)&&std::isfinite(proof.cutoff)&&proof.alpha_min<=proof.alpha_max&&
               proof.cutoff>=0&&proof.cutoff<=1)
            {
                const double a=double(proof.alpha_min)*proof.scale,b=double(proof.alpha_max)*proof.scale;
                // Bound the shader float multiply and cutoff equality outward.
                const float lo=std::clamp(std::nextafter(float(std::min(a,b)),-std::numeric_limits<float>::infinity()),0.0f,1.0f);
                const float hi=std::clamp(std::nextafter(float(std::max(a,b)),std::numeric_limits<float>::infinity()),0.0f,1.0f);
                if(hi<proof.cutoff)state=0;else if(lo>=proof.cutoff)state=1;
            }
        }
        const auto offset=triangle*stride;
        std::fill_n(out.states.begin()+offset,stride,std::byte(state*0x55u));
        out.descriptors.push_back({offset,std::uint16_t(in.subdivision_level),kOpacityMicromapFourStateFormat});
        out.indices.push_back(triangle);
        (state<2?out.known_cells:out.unknown_cells)+=cells;
    }
    out.histogram.push_back({count,in.subdivision_level,kOpacityMicromapFourStateFormat});
    return out;
}
}
