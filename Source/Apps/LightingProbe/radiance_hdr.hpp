#pragma once

#include <DirectXTex.h>
#include <hs/core/types.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hs::lighting_probe
{
enum class HdrPrimariesPolicy
{
    RequireDeclaredRec709,
    // Authored interpretation for RGBE sources with absent/all-zero PRIMARIES.
    // This is not Radiance's historical absent-header chromaticity default.
    AuthoredRec709IfUnspecified
};
// Content-tool-only adapter around the existing pinned DirectXTex decoder.
// Radiance primary format reference (header, all eight orientations, RGBE/RLE):
// https://radsite.lbl.gov/radiance/refer/Notes/picture_format.html
// Input radiance is scene linear Rec709, with recorded EXPOSURE undone once.
// Conflicting profiles, XYZE, gamma, channel corrections and pixel aspect fail.
namespace hdr_detail
{
inline void Require(bool condition,std::string_view message)
{
    if(!condition) throw std::runtime_error("lighting probe HDR: "+std::string(message));
}
inline std::string_view Trim(std::string_view text)
{
    const auto first=text.find_first_not_of(" \t"),last=text.find_last_not_of(" \t");
    return first==std::string_view::npos?std::string_view{}:text.substr(first,last-first+1);
}
inline std::vector<std::string_view> Tokens(std::string_view text)
{
    std::vector<std::string_view> result;
    while(!(text=Trim(text)).empty())
    {
        const auto end=text.find_first_of(" \t");
        result.push_back(text.substr(0,end));
        if(end==std::string_view::npos) break;
        text.remove_prefix(end);
    }
    return result;
}
inline double Number(std::string_view text)
{
    text=Trim(text); if(!text.empty() && text.front()=='+') text.remove_prefix(1);
    Require(!text.empty(),"empty numeric header value");
    double value{}; const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    Require(!text.empty() && parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size() && std::isfinite(value),
        "invalid finite numeric header value");
    return value;
}
inline std::uint32_t Count(std::string_view text)
{
    Require(!text.empty(),"empty resolution count");
    std::uint32_t value{}; const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    Require(!text.empty() && parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size() && value>0,
        "invalid resolution count"); return value;
}
struct Header
{
    std::uint32_t width{},height{},scans{},scan_length{};
    char major{},minor{},major_sign{},minor_sign{};
    double exposure{1};
    std::size_t payload{};
    bool authored_primaries_applied{};
};
inline Header ReadHeader(std::string_view bytes,HdrPrimariesPolicy primaries_policy)
{
    Require(bytes.size()<=512ull*1024*1024,"HDR source exceeds 512 MiB input budget");
    std::size_t cursor=0;
    auto line=[&]() {
        const auto end=bytes.find('\n',cursor);
        Require(end!=std::string_view::npos && end<=65536 && end-cursor<=8192,"missing or oversized HDR header line");
        auto value=bytes.substr(cursor,end-cursor); cursor=end+1;
        if(!value.empty() && value.back()=='\r') value.remove_suffix(1);
        for(unsigned char character:value)
            Require(character=='\t' || (character>=32 && character<=126),"non-ASCII HDR header");
        return value;
    };
    const auto signature=line();
    Require(signature.starts_with("#?") && signature.size()>2,"missing Radiance header signature");
    Header result; bool format=false,primaries=false;
    while(true)
    {
        const auto text=line(); if(text.empty()) break;
        if(Trim(text).starts_with("#")) continue;
        const auto separator=text.find('='); if(separator==std::string_view::npos) continue;
        const auto key=Trim(text.substr(0,separator)),value=Trim(text.substr(separator+1));
        if(key=="FORMAT")
        {
            Require(!format && value=="32-bit_rle_rgbe","expected one FORMAT=32-bit_rle_rgbe; XYZE is unsupported");
            format=true;
        }
        else if(key=="EXPOSURE")
        {
            const double exposure=Number(value); Require(exposure>0,"EXPOSURE must be positive");
            result.exposure*=exposure;
            Require(result.exposure>0 && std::isfinite(result.exposure),"combined EXPOSURE cannot be represented");
        }
        else if(key=="PRIMARIES")
        {
            const auto values=Tokens(value);
            constexpr std::array rec709{0.64,0.33,0.30,0.60,0.15,0.06,0.3127,0.3290};
            Require(!primaries && values.size()==rec709.size(),"invalid or duplicate PRIMARIES");
            std::array<double,8> declared{};
            for(std::size_t i=0;i<values.size();++i)
                declared[i]=Number(values[i]);
            const bool unspecified=std::ranges::all_of(declared,[](double component) { return component==0; });
            if(unspecified)
            {
                Require(primaries_policy==HdrPrimariesPolicy::AuthoredRec709IfUnspecified,
                    "zero PRIMARIES requires explicit authored_rec709_if_unspecified policy");
                result.authored_primaries_applied=true;
            }
            else Require(declared==rec709,"PRIMARIES conflicts with authored rec709_linear source");
            primaries=true;
        }
        else if(key=="COLORCORR")
        {
            const auto values=Tokens(value); Require(values.size()==3,"invalid COLORCORR");
            for(auto component:values) Require(Number(component)==1.0,"nonidentity COLORCORR is unsupported");
        }
        else if(key=="GAMMA" || key=="PIXASPECT")
            Require(Number(value)==1.0,"nonidentity GAMMA/PIXASPECT conflicts with linear spherical panorama");
        else if(key.find("PROFILE")!=std::string_view::npos || key=="ICC" || key=="COLORSPACE" ||
            key=="COLOR_SPACE" || key=="TRANSFER" || key=="TRANSFER_FUNCTION")
            Require(false,"embedded color profile/transfer metadata is unsupported; source must declare Rec709 linear RGBE");
    }
    Require(format,"missing RGBE FORMAT");
    if(!primaries)
    {
        Require(primaries_policy==HdrPrimariesPolicy::AuthoredRec709IfUnspecified,
            "missing PRIMARIES requires explicit authored_rec709_if_unspecified policy");
        result.authored_primaries_applied=true;
    }
    const auto resolution=Tokens(line());
    Require(resolution.size()==4 && resolution[0].size()==2 && resolution[2].size()==2,
        "invalid HDR resolution/orientation line");
    auto axis=[](std::string_view token) {
        Require((token[0]=='+' || token[0]=='-') && (token[1]=='X' || token[1]=='Y'),"invalid signed HDR axis");
    };
    axis(resolution[0]); axis(resolution[2]);
    result.major=resolution[0][1]; result.major_sign=resolution[0][0];
    result.minor=resolution[2][1]; result.minor_sign=resolution[2][0];
    Require(result.major!=result.minor,"HDR axes must be distinct");
    result.scans=Count(resolution[1]); result.scan_length=Count(resolution[3]);
    result.width=result.major=='X'?result.scans:result.scan_length;
    result.height=result.major=='Y'?result.scans:result.scan_length;
    Require(result.width<=16384 && result.height<=8192 && std::uint64_t(result.width)==std::uint64_t(result.height)*2 &&
        std::uint64_t(result.width)*result.height<=64ull*1024*1024,"expected bounded 2:1 equirectangular panorama");
    result.payload=cursor; return result;
}
struct ValidatedPayload
{
    std::vector<bool> exponent_zero; // Exact RGBE exponent0 means black, never a tiny positive value.
    std::string short_scan_payload; // Adapts valid scan lengths<8 to the mature decoder's RLE classifier.
};
inline ValidatedPayload ValidatePayload(std::string_view payload,const Header &header)
{
    ValidatedPayload result;
    result.exponent_zero.resize(std::size_t(header.scans)*header.scan_length);
    std::size_t cursor=0;
    auto byte=[&]() {
        Require(cursor<payload.size(),"truncated RGBE scanline");
        return static_cast<std::uint8_t>(payload[cursor++]);
    };
    auto pixel=[&]() { return std::array<std::uint8_t,4>{byte(),byte(),byte(),byte()}; };
    for(std::uint32_t row=0;row<header.scans;++row)
    {
        auto first=pixel(); const auto base=std::size_t(row)*header.scan_length;
        if(header.scan_length>=8 && first[0]==2 && first[1]==2 && first[2]<128)
        {
            Require((std::uint32_t(first[2])<<8|first[3])==header.scan_length,"adaptive RLE scanline length mismatch");
            for(std::uint32_t channel=0;channel<4;++channel)
            {
                std::uint32_t position=0;
                while(position<header.scan_length)
                {
                    const auto code=byte(); const auto length=std::uint32_t(code>128?code-128:code);
                    Require(length>0 && length<=header.scan_length-position,"zero or overflowing adaptive RLE run");
                    if(code>128)
                    {
                        const auto value=byte();
                        if(channel==3) for(std::uint32_t i=0;i<length;++i) result.exponent_zero[base+position+i]=value==0;
                    }
                    else for(std::uint32_t i=0;i<length;++i)
                    {
                        const auto value=byte(); if(channel==3) result.exponent_zero[base+position+i]=value==0;
                    }
                    position+=length;
                }
            }
        }
        else
        {
            std::array<std::array<std::uint8_t,4>,8> short_scan{};
            std::array<std::uint8_t,4> previous{}; bool have_previous=false;
            std::uint32_t position=0,shift=0;
            while(position<header.scan_length)
            {
                const auto value=position==0 && !have_previous?first:pixel();
                if(value[0]==1 && value[1]==1 && value[2]==1)
                {
                    Require(have_previous && shift<=24,"old RGBE repeat needs a preceding pixel and bounded count");
                    const auto length=std::uint64_t(value[3])<<shift;
                    Require(length<=header.scan_length-position,"old RGBE repeat exceeds scanline");
                    for(std::uint64_t i=0;i<length;++i)
                    {
                        result.exponent_zero[base+position]=previous[3]==0;
                        if(header.scan_length<8) short_scan[position]=previous;
                        ++position;
                    }
                    shift+=8;
                }
                else
                {
                    previous=value; have_previous=true; shift=0;
                    result.exponent_zero[base+position]=value[3]==0;
                    if(header.scan_length<8) short_scan[position]=value;
                    ++position;
                }
            }
            if(header.scan_length<8)
            {
                // DirectXTex recognizes the adaptive marker regardless of scan
                // length. Pad only short scans to8, encode their literal channels,
                // and address just the original pixels after float decoding.
                result.short_scan_payload.append("\x02\x02\x00\x08",4);
                for(std::uint32_t channel=0;channel<4;++channel)
                {
                    result.short_scan_payload.push_back(char(8));
                    for(std::uint32_t x=0;x<8;++x)
                        result.short_scan_payload.push_back(char(short_scan[std::min(x,header.scan_length-1)][channel]));
                }
            }
        }
    }
    Require(cursor==payload.size(),"unexpected trailing RGBE payload"); return result;
}
} // namespace hdr_detail

struct RadianceHdrPanorama
{
    hdr_detail::Header header;
    DirectX::ScratchImage decoded;
    std::vector<bool> exponent_zero;
    [[nodiscard]] Float4 At(std::uint32_t x,std::uint32_t y) const
    {
        // Canonical coordinates: x rightward, y downward. Radiance true Y
        // increases upward; major/minor signs describe the original file order.
        auto coordinate=[&](char axis,char sign) {
            return axis=='X' ? (sign=='+'?x:header.width-1-x) : (sign=='-'?y:header.height-1-y);
        };
        const auto row=coordinate(header.major,header.major_sign),column=coordinate(header.minor,header.minor_sign);
        if(exponent_zero[std::size_t(row)*header.scan_length+column]) return {0,0,0,1};
        const auto *image=decoded.GetImage(0,0,0); Float4 value;
        std::memcpy(&value,image->pixels+std::size_t(row)*image->rowPitch+std::size_t(column)*16,16);
        const double r=double(value.x)/header.exposure,g=double(value.y)/header.exposure,b=double(value.z)/header.exposure;
        hdr_detail::Require(std::isfinite(r) && std::isfinite(g) && std::isfinite(b) && r>=0 && g>=0 && b>=0 &&
            r<=std::numeric_limits<float>::max() && g<=std::numeric_limits<float>::max() && b<=std::numeric_limits<float>::max(),
            "calibrated scene-linear radiance exceeds float32 convention");
        return {float(r),float(g),float(b),1};
    }
};
[[nodiscard]] inline RadianceHdrPanorama ReadRadianceHdr(std::string_view bytes,
    HdrPrimariesPolicy primaries_policy=HdrPrimariesPolicy::RequireDeclaredRec709)
{
    RadianceHdrPanorama result; result.header=hdr_detail::ReadHeader(bytes,primaries_policy);
    const auto payload=bytes.substr(result.header.payload);
    auto validation=hdr_detail::ValidatePayload(payload,result.header);
    const auto decode_width=std::max(result.header.scan_length,8u);
    std::string canonical="#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y "+std::to_string(result.header.scans)+
        " +X "+std::to_string(decode_width)+"\n";
    canonical.append(result.header.scan_length<8?std::string_view(validation.short_scan_payload):payload);
    DirectX::TexMetadata metadata;
    const auto native=DirectX::LoadFromHDRMemory(reinterpret_cast<const std::uint8_t *>(canonical.data()),
        canonical.size(),&metadata,result.decoded);
    hdr_detail::Require(SUCCEEDED(native),"DirectXTex RGBE decoding failed (HRESULT "+std::to_string(std::int32_t(native))+")");
    const auto *image=result.decoded.GetImage(0,0,0);
    hdr_detail::Require(image && metadata.width==decode_width && metadata.height==result.header.scans &&
        image->format==DXGI_FORMAT_R32G32B32A32_FLOAT,"unexpected decoded RGBE layout");
    result.exponent_zero=std::move(validation.exponent_zero);
    for(std::uint32_t y=0;y<result.header.height;++y)
        for(std::uint32_t x=0;x<result.header.width;++x) (void)result.At(x,y);
    return result;
}
} // namespace hs::lighting_probe
