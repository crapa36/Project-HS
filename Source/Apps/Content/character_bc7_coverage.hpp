#pragma once
#include "character_texture_mips.hpp"
#include <limits>

namespace hs::content
{
// Narrow BC7 mode5 repair, not a replacement texture compressor. Keep an
// existing unrotated mode5 RGB payload verbatim; otherwise fit only this block.
// Mode5 has independent RGB7/A8 endpoints and two independent 2-bit index sets.
using CharacterBc7Block = std::array<std::uint8_t,16>;
using CharacterBc7Texels = std::array<CharacterTexel,16>;
namespace character_bc7_detail
{
inline constexpr std::array<unsigned,4> weights{0,21,43,64};
inline unsigned Interpolate(unsigned a,unsigned b,unsigned index)
{ return (a*(64-weights[index])+b*weights[index]+32)>>6; }
inline void Bits(CharacterBc7Block &block,unsigned offset,unsigned count,unsigned value)
{
    for(unsigned bit=0;bit<count;++bit)
    {
        const auto mask=static_cast<std::uint8_t>(1u<<((offset+bit)&7u));
        auto &byte=block[(offset+bit)>>3u];
        byte=static_cast<std::uint8_t>((byte&~mask)|((value&(1u<<bit))?mask:0));
    }
}
inline unsigned Quantize7(float value)
{
    const auto target=static_cast<unsigned>(std::lround(std::clamp(value,0.0f,1.0f)*255));
    unsigned best{},error=256;
    for(unsigned code=0;code<128;++code)
    {
        const auto decoded=(code<<1u)|(code>>6u);
        const auto delta=decoded>target?decoded-target:target-decoded;
        if(delta<error) { best=code;error=delta; }
    }
    return best;
}
using Colors=std::array<std::array<unsigned,3>,2>;
inline double ColorError(const CharacterBc7Texels &source,const Colors &endpoints,std::array<unsigned,16> &indices)
{
    std::array<std::array<double,3>,4> palette{};
    for(unsigned index=0;index<4;++index) for(unsigned channel=0;channel<3;++channel)
    {
        const auto a=(endpoints[0][channel]<<1u)|(endpoints[0][channel]>>6u);
        const auto b=(endpoints[1][channel]<<1u)|(endpoints[1][channel]>>6u);
        palette[index][channel]=CharacterSrgbToLinear(Interpolate(a,b,index)/255.0);
    }
    double total{};
    for(unsigned pixel=0;pixel<16;++pixel)
    {
        double best=std::numeric_limits<double>::infinity();
        for(unsigned index=0;index<4;++index)
        {
            double error{};
            for(unsigned channel=0;channel<3;++channel)
            {
                const double delta=palette[index][channel]-CharacterSrgbToLinear(source[pixel][channel]);
                error+=delta*delta;
            }
            if(error<best) { best=error;indices[pixel]=index; }
        }
        total+=best;
    }
    return total;
}
inline void FitColor(const CharacterBc7Texels &source,Colors &endpoints,std::array<unsigned,16> &indices)
{
    // Deterministic farthest-pair seed and bounded endpoint coordinate search.
    // Optimize RGB in linear light; alpha is independently hard-constrained.
    double farthest=-1;unsigned first{},last{};
    for(unsigned a=0;a<16;++a) for(unsigned b=a;b<16;++b)
    {
        double distance{};
        for(unsigned channel=0;channel<3;++channel)
        { const double delta=source[a][channel]-source[b][channel];distance+=delta*delta; }
        if(distance>farthest) { farthest=distance;first=a;last=b; }
    }
    for(unsigned channel=0;channel<3;++channel)
    { endpoints[0][channel]=Quantize7(source[first][channel]);endpoints[1][channel]=Quantize7(source[last][channel]); }
    double best=ColorError(source,endpoints,indices);
    for(unsigned iteration=0;iteration<8;++iteration)
    {
        bool changed{};
        for(unsigned endpoint=0;endpoint<2;++endpoint) for(unsigned channel=0;channel<3;++channel)
            for(const int step:{-1,1})
            {
                const int code=static_cast<int>(endpoints[endpoint][channel])+step;
                if(code<0 || code>127) continue;
                auto candidate=endpoints;candidate[endpoint][channel]=static_cast<unsigned>(code);
                std::array<unsigned,16> candidate_indices{};
                const double error=ColorError(source,candidate,candidate_indices);
                if(error<best) { best=error;endpoints=candidate;indices=candidate_indices;changed=true; }
            }
        if(!changed) break;
    }
    // BC7 fixes the high index bit of texel0 to zero, independently per stream.
    if(indices[0]>=2) { std::swap(endpoints[0],endpoints[1]);for(auto &index:indices) index=3-index; }
}
} // namespace character_bc7_detail

[[nodiscard]] inline bool EncodeCharacterBc7CoverageBlock(const CharacterBc7Texels &source,
    CharacterAlphaPolicy policy,const CharacterBc7Block &original,CharacterBc7Block &output)
{
    using namespace character_bc7_detail;
    if(!(policy.cutoff>0 && policy.cutoff<=1)) return false;
    unsigned minimum=255,maximum=0,threshold=256;bool covered{},uncovered{};
    for(unsigned value=0;value<256;++value)
        if(float(value)/255.0f>=policy.cutoff) { threshold=value;break; }
    for(const auto &pixel:source)
    {
        for(const auto value:pixel) if(!std::isfinite(value)||value<0||value>1) return false;
        const auto alpha=static_cast<unsigned>(std::lround(pixel[3]*255));
        minimum=std::min(minimum,alpha);maximum=std::max(maximum,alpha);
        covered|=pixel[3]>=policy.cutoff;uncovered|=pixel[3]<policy.cutoff;
    }
    // Reference is the semantic float mip, not its R8 wire quantization. If
    // that quantization crossed cutoff, include the closest byte on its proper side.
    if(uncovered && minimum>=threshold) { if(!threshold) return false;minimum=threshold-1; }
    if(covered && maximum<threshold) { if(threshold>255) return false;maximum=threshold; }
    std::array<unsigned,16> alpha_indices{};
    for(unsigned pixel=0;pixel<16;++pixel)
    {
        const auto alpha=source[pixel][3];
        const bool want_covered=alpha>=policy.cutoff;
        const bool exact_anchor=alpha==0 || alpha==1 || (policy.preserve_translucent &&
            std::abs(alpha-173.0f/255.0f)<1.e-7f);
        double best=std::numeric_limits<double>::infinity();bool found{};
        for(unsigned index=0;index<4;++index)
        {
            const float decoded=float(Interpolate(minimum,maximum,index))/255.0f;
            if((decoded>=policy.cutoff)!=want_covered || (exact_anchor && std::abs(decoded-alpha)>1.e-7f)) continue;
            const double delta=double(decoded)-alpha,error=delta*delta;
            if(error<best) { best=error;alpha_indices[pixel]=index;found=true; }
        }
        if(!found) return false; // No silent approximation of zero/body/opaque anchors.
    }
    if(alpha_indices[0]>=2)
    { std::swap(minimum,maximum);for(auto &index:alpha_indices) index=3-index; }
    const bool rgb_retained=(original[0]&0x3fu)==0x20u && (original[0]>>6u)==0;
    if(rgb_retained) output=original;
    else
    {
        output={};Bits(output,0,6,0x20); // mode5, rotation0
        Colors endpoints{};std::array<unsigned,16> indices{};FitColor(source,endpoints,indices);
        unsigned bit=8;
        for(unsigned channel=0;channel<3;++channel) for(unsigned endpoint=0;endpoint<2;++endpoint)
        { Bits(output,bit,7,endpoints[endpoint][channel]);bit+=7; }
        bit=66;
        for(unsigned pixel=0;pixel<16;++pixel)
        { const unsigned count=pixel?2:1;Bits(output,bit,count,indices[pixel]);bit+=count; }
    }
    Bits(output,50,8,minimum);Bits(output,58,8,maximum);
    unsigned bit=97;
    for(unsigned pixel=0;pixel<16;++pixel)
    { const unsigned count=pixel?2:1;Bits(output,bit,count,alpha_indices[pixel]);bit+=count; }
    return true;
}
} // namespace hs::content
