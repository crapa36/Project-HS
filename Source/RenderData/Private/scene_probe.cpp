#include <hs/render_data/scene_probe.hpp>
#include <hs/core/sha256.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace hs::render_data
{
namespace
{
constexpr std::array<std::byte, 8> kMagic{std::byte{'H'},std::byte{'S'},std::byte{'S'},std::byte{'P'},
    std::byte{'R'},std::byte{'O'},std::byte{'B'},std::byte{'E'}};
constexpr std::uint64_t kMaximumBytes = 256ull * 1024 * 1024;
std::array<std::byte,32> PackageDigest(std::span<const std::byte> bytes)
{
    // Include revision/dependency metadata as well as payloads. Only the digest
    // field itself is omitted, avoiding a second whole-package allocation.
    hs::Sha256Stream digest; digest.Add(bytes.first(160));
    digest.Add(bytes.subspan(kSceneProbeHeaderStride)); return digest.Finish();
}
void Require(bool valid, std::string_view message)
{
    if (!valid) throw std::runtime_error("scene probe: " + std::string(message));
}
std::uint64_t Add(std::uint64_t a, std::uint64_t b)
{
    Require(b <= kMaximumBytes && a <= kMaximumBytes - b, "package size overflow"); return a + b;
}
std::uint64_t Mul(std::uint64_t a, std::uint64_t b)
{
    Require(!b || a <= kMaximumBytes / b, "package count/stride overflow"); return a * b;
}
void Put(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value, std::size_t count)
{
    Require(offset <= bytes.size() && count <= bytes.size() - offset, "write bounds");
    for (std::size_t i = 0; i < count; ++i) bytes[offset+i] = std::byte((value >> (8*i)) & 255);
}
std::uint64_t Get(std::span<const std::byte> bytes, std::size_t offset, std::size_t count)
{
    Require(offset <= bytes.size() && count <= bytes.size() - offset, "read bounds");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) value |= std::uint64_t(std::to_integer<unsigned>(bytes[offset+i])) << (8*i);
    return value;
}
void PutFloat(std::span<std::byte> bytes, std::size_t offset, float value)
{ Put(bytes, offset, std::bit_cast<std::uint32_t>(value), 4); }
float GetFloat(std::span<const std::byte> bytes, std::size_t offset)
{ return std::bit_cast<float>(static_cast<std::uint32_t>(Get(bytes, offset, 4))); }
bool Finite(Float3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
bool Contains(Float3 low, Float3 high, Float3 p)
{ return low.x <= p.x && low.y <= p.y && low.z <= p.z && high.x >= p.x && high.y >= p.y && high.z >= p.z; }
void ValidateRecord(const SceneProbeRecord &p)
{
    Require(p.stable_id && Finite(p.position) && Finite(p.influence_min) && Finite(p.influence_max) &&
        Finite(p.projection_min) && Finite(p.projection_max), "nonfinite record or zero probe identity");
    Require(p.influence_min.x < p.influence_max.x && p.influence_min.y < p.influence_max.y &&
        p.influence_min.z < p.influence_max.z && Contains(p.influence_min,p.influence_max,p.position) &&
        Contains(p.projection_min,p.projection_max,p.influence_min) &&
        Contains(p.projection_min,p.projection_max,p.influence_max), "invalid influence/projection bounds");
    Require(std::isfinite(p.weight) && p.weight > 0 && p.weight <= 1 &&
        std::isfinite(p.influence_fade) && p.influence_fade > 0 &&
        std::isfinite(p.visibility_bias) && p.visibility_bias >= 0 &&
        std::isfinite(p.visibility_fade) && p.visibility_fade > 0 &&
        std::isfinite(p.capture_distance) && p.capture_distance > 0.1f && p.capture_distance <= 100000,
        "invalid influence/visibility/capture parameters");
    const auto extent = Float3{p.projection_max.x-p.projection_min.x,p.projection_max.y-p.projection_min.y,
        p.projection_max.z-p.projection_min.z};
    Require(Finite(extent) && double(extent.x)*extent.x + double(extent.y)*extent.y + double(extent.z)*extent.z <=
        double(p.capture_distance)*p.capture_distance, "projection box exceeds capture distance");
}
bool ValidDependency(std::string_view id)
{
    if (id.empty() || id.size() > 512 || id.front() == '/' || id.back() == '/') return false;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= id.size(); ++i)
    {
        if (i == id.size() || id[i] == '/')
        {
            const auto component = id.substr(start,i-start);
            if (component.empty() || component == "." || component == "..") return false;
            start=i+1;
        }
        else if (!((id[i]>='a' && id[i]<='z') || (id[i]>='A' && id[i]<='Z') ||
            (id[i]>='0' && id[i]<='9') || id[i]=='_' || id[i]=='-' || id[i]=='.')) return false;
    }
    return true;
}
std::uint64_t ValidateDescription(const SceneProbeDescription &d, std::span<const std::string> dependencies)
{
    Require(d.edge>=16 && d.edge<=128 && std::has_single_bit(d.edge) && d.ggx_samples>=64 &&
        d.ggx_samples<=2048 && std::has_single_bit(d.ggx_samples) &&
        d.blend==SceneProbeBlend::NormalizedInfluenceVisibility, "unsupported bake/blend policy");
    Require(d.revisions.environment && d.revisions.material && d.revisions.light && d.revisions.static_layout,
        "missing scene input revision");
    Require(!dependencies.empty() && dependencies.size()<=256, "invalid dependency count");
    std::uint64_t bytes=0; std::string_view previous;
    for (const auto &id : dependencies)
    {
        Require(ValidDependency(id) && (previous.empty() || previous<id), "dependencies must be canonical sorted unique IDs");
        bytes=Add(bytes,2+id.size()); previous=id;
    }
    for (auto index : {d.environment_dependency,d.material_dependency,d.light_dependency,d.static_layout_dependency})
        Require(index<dependencies.size(), "referenced scene dependency is missing");
    return bytes;
}
std::uint64_t Texels(std::uint32_t edge, std::uint32_t mips, std::size_t probes)
{
    std::uint64_t face=0;
    for (std::uint32_t mip=0;mip<mips;++mip) { const auto e=std::max(1u,edge>>mip); face=Add(face,Mul(e,e)); }
    return Mul(Mul(face,6),probes);
}
void Put3(std::span<std::byte> bytes,std::size_t offset,Float3 p)
{ PutFloat(bytes,offset,p.x); PutFloat(bytes,offset+4,p.y); PutFloat(bytes,offset+8,p.z); }
Float3 Get3(std::span<const std::byte> bytes,std::size_t offset)
{ return {GetFloat(bytes,offset),GetFloat(bytes,offset+4),GetFloat(bytes,offset+8)}; }
void EncodeRecord(std::span<std::byte> bytes,std::size_t offset,const SceneProbeRecord &p)
{
    ValidateRecord(p); Put(bytes,offset,p.stable_id,8); Put3(bytes,offset+8,p.position); PutFloat(bytes,offset+20,p.weight);
    Put3(bytes,offset+24,p.influence_min); PutFloat(bytes,offset+36,p.influence_fade);
    Put3(bytes,offset+40,p.influence_max); PutFloat(bytes,offset+52,p.visibility_bias);
    Put3(bytes,offset+56,p.projection_min); PutFloat(bytes,offset+68,p.visibility_fade);
    Put3(bytes,offset+72,p.projection_max); PutFloat(bytes,offset+84,p.capture_distance);
}
SceneProbeRecord DecodeRecord(std::span<const std::byte> bytes,std::size_t offset)
{
    SceneProbeRecord p;
    p.stable_id=Get(bytes,offset,8); p.position=Get3(bytes,offset+8); p.weight=GetFloat(bytes,offset+20);
    p.influence_min=Get3(bytes,offset+24); p.influence_fade=GetFloat(bytes,offset+36);
    p.influence_max=Get3(bytes,offset+40); p.visibility_bias=GetFloat(bytes,offset+52);
    p.projection_min=Get3(bytes,offset+56); p.visibility_fade=GetFloat(bytes,offset+68);
    p.projection_max=Get3(bytes,offset+72); p.capture_distance=GetFloat(bytes,offset+84);
    for (std::size_t at=88;at<128;at+=4) Require(Get(bytes,offset+at,4)==0,"reserved record field");
    ValidateRecord(p); return p;
}
void Validate4(Float4 p,bool sh)
{
    Require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(p.w) &&
        (sh ? p.w==0 : p.w==1 && p.x>=0 && p.y>=0 && p.z>=0), "invalid radiance/SH texel");
}
void Encode4(std::span<std::byte> bytes,std::size_t offset,Float4 p,bool sh)
{ Validate4(p,sh); Put3(bytes,offset,{p.x,p.y,p.z}); PutFloat(bytes,offset+12,p.w); }
TexturePackageView CubeView(std::span<const std::byte> bytes,std::uint32_t edge,std::uint32_t mips,
    std::size_t probes,const std::vector<std::string> &dependencies)
{
    Require(bytes.size()==Mul(Texels(edge,mips,probes),16),"cube payload byte count");
    TexturePackageView cube; cube.description={edge,edge,static_cast<std::uint32_t>(6*probes),mips,kTextureFormatRgba32Float};
    cube.dependencies=dependencies; std::size_t offset=0;
    for (std::uint32_t layer=0;layer<6*probes;++layer) for (std::uint32_t mip=0;mip<mips;++mip)
    {
        const auto e=std::max(1u,edge>>mip),row=e*16,slice=e*row;
        cube.subresources.push_back({layer,mip,e,e,row,slice,bytes.subspan(offset,slice)});
        for (std::size_t at=0;at<slice;at+=16)
            Validate4({GetFloat(bytes,offset+at),GetFloat(bytes,offset+at+4),GetFloat(bytes,offset+at+8),GetFloat(bytes,offset+at+12)},false);
        offset+=slice;
    }
    return cube;
}
} // namespace

std::uint64_t DeriveSceneProbeRevision(SceneProbeRevisionDomain domain,
    std::span<const SceneProbeRevisionInput> inputs)
{
    Require(domain>=SceneProbeRevisionDomain::Environment && domain<=SceneProbeRevisionDomain::StaticLayout,
        "invalid revision domain");
    Require(!inputs.empty() && inputs.size()<=8192,"invalid revision input count");
    hs::Sha256Stream hash;
    constexpr std::string_view tag="HSSPROBE-REVISION-1";
    hash.Add(std::as_bytes(std::span(tag.data(),tag.size())));
    auto integer=[&](std::uint64_t value,std::size_t count) {
        std::array<std::byte,8> encoded{};
        for(std::size_t i=0;i<count;++i) encoded[i]=std::byte((value>>(8*i))&255);
        hash.Add(std::span(encoded).first(count));
    };
    integer(static_cast<std::uint32_t>(domain),4); integer(inputs.size(),8);
    std::string_view previous;
    for(const auto &input:inputs)
    {
        Require(ValidDependency(input.logical_id) && (previous.empty() || previous<input.logical_id),
            "revision input identities must be canonical sorted unique IDs");
        Require(input.sha256.size()==64,"invalid revision input SHA256 length");
        std::array<std::byte,32> digest{};
        auto nibble=[](char c)->unsigned {
            Require((c>='0' && c<='9') || (c>='a' && c<='f'),"revision SHA256 must be lowercase hex");
            return c<='9'?unsigned(c-'0'):unsigned(c-'a'+10);
        };
        for(std::size_t i=0;i<digest.size();++i)
            digest[i]=std::byte((nibble(input.sha256[2*i])<<4)|nibble(input.sha256[2*i+1]));
        integer(input.logical_id.size(),8);
        hash.Add(std::as_bytes(std::span(input.logical_id.data(),input.logical_id.size()))); hash.Add(digest);
        previous=input.logical_id;
    }
    const auto digest=hash.Finish(); std::uint64_t revision=0;
    for(std::size_t i=0;i<8;++i) revision=(revision<<8)|std::to_integer<unsigned>(digest[i]);
    return revision|std::uint64_t(1);
}

std::string DeriveSceneProbeInputIdentity(const SceneProbeDescription &description,
    std::span<const SceneProbeRecord> probes, std::span<const std::string> dependencies,
    std::span<const SceneProbeRevisionInput> source_inputs)
{
    Require(!probes.empty() && probes.size()<=kSceneProbeMaximumCount,"invalid semantic probe count");
    (void)ValidateDescription(description,dependencies);
    Require(source_inputs.size()==dependencies.size(),"source identity count differs from dependencies");
    for(std::size_t i=0;i<source_inputs.size();++i)
        Require(source_inputs[i].logical_id==dependencies[i],"source identities must match canonical dependencies");

    hs::Sha256Stream hash;
    constexpr std::string_view tag="HSSPROBE-INPUT-IDENTITY-1";
    auto bytes=[&](std::string_view value) { hash.Add(std::as_bytes(std::span(value.data(),value.size()))); };
    auto integer=[&](std::uint64_t value,std::size_t count=8) {
        std::array<std::byte,8> encoded{};
        for(std::size_t i=0;i<count;++i) encoded[i]=std::byte((value>>(8*i))&255);
        hash.Add(std::span(encoded).first(count));
    };
    auto text=[&](std::string_view value) { integer(value.size()); bytes(value); };
    auto floating=[&](float value) {
        Require(std::isfinite(value),"nonfinite semantic probe input");
        integer(std::bit_cast<std::uint32_t>(value),4);
    };
    auto vector=[&](Float3 value) { floating(value.x); floating(value.y); floating(value.z); };
    bytes(tag);
    integer(description.edge,4); integer(description.ggx_samples,4);
    integer(static_cast<std::uint32_t>(description.blend),4);
    integer(probes.size());
    for(const auto &probe:probes)
    {
        ValidateRecord(probe);
        integer(probe.stable_id); vector(probe.position); floating(probe.weight);
        vector(probe.influence_min); floating(probe.influence_fade);
        vector(probe.influence_max); floating(probe.visibility_bias);
        vector(probe.projection_min); floating(probe.visibility_fade);
        vector(probe.projection_max); floating(probe.capture_distance);
    }
    text(dependencies[description.environment_dependency]);
    text(dependencies[description.material_dependency]);
    text(dependencies[description.light_dependency]);
    text(dependencies[description.static_layout_dependency]);
    integer(source_inputs.size()); std::string_view previous;
    for(const auto &input:source_inputs)
    {
        Require(ValidDependency(input.logical_id) && (previous.empty() || previous<input.logical_id),
            "source identities must be canonical sorted unique logical IDs");
        Require(input.sha256.size()==64,"invalid source identity SHA256 length");
        std::array<std::byte,32> digest{};
        auto nibble=[](char c)->unsigned {
            Require((c>='0' && c<='9') || (c>='a' && c<='f'),"source identity SHA256 must be lowercase hex");
            return c<='9'?unsigned(c-'0'):unsigned(c-'a'+10);
        };
        for(std::size_t i=0;i<digest.size();++i)
            digest[i]=std::byte((nibble(input.sha256[2*i])<<4)|nibble(input.sha256[2*i+1]));
        text(input.logical_id); hash.Add(digest); previous=input.logical_id;
    }
    const auto digest=hash.Finish(); constexpr char digits[]="0123456789abcdef";
    std::string result="sha256:"; result.reserve(71);
    for(const auto byte:digest) { const auto value=std::to_integer<unsigned>(byte); result+=digits[value>>4]; result+=digits[value&15]; }
    return result;
}

std::vector<std::byte> BuildSceneProbePackage(AssetId asset,std::span<const std::string> dependencies,
    const SceneProbeDescription &d,std::span<const SceneProbeRecord> probes,SceneProbeBakeData bake)
{
    Require(asset.value && !probes.empty() && probes.size()<=kSceneProbeMaximumCount,"invalid asset/probe count");
    const auto depBytes=ValidateDescription(d,dependencies);
    const auto mips=std::bit_width(d.edge);
    const bool baked=!bake.radiance.empty() || !bake.prefiltered.empty() || !bake.cosine_sh.empty() || !bake.first_hit_distance.empty();
    const auto rawCount=Texels(d.edge,1,probes.size()),ggxCount=Texels(d.edge,mips,probes.size());
    Require(!baked || (bake.radiance.size()==rawCount && bake.prefiltered.size()==ggxCount &&
        bake.cosine_sh.size()==Mul(probes.size(),9) && bake.first_hit_distance.size()==rawCount),"incomplete baked payloads");
    const std::uint64_t rawBytes=baked?Mul(rawCount,16):0,ggxBytes=baked?Mul(ggxCount,16):0,
        shBytes=baked?Mul(Mul(probes.size(),9),16):0,distanceBytes=baked?Mul(rawCount,4):0;
    auto total=Add(kSceneProbeHeaderStride,depBytes); total=Add(total,Mul(probes.size(),kSceneProbeRecordStride));
    for (auto size : {rawBytes,ggxBytes,shBytes,distanceBytes}) total=Add(total,size);
    std::vector<std::byte> bytes(static_cast<std::size_t>(total)); std::copy(kMagic.begin(),kMagic.end(),bytes.begin());
    Put(bytes,8,kSceneProbeVersion,4); Put(bytes,12,1,4); Put(bytes,16,kSceneProbeHeaderStride,4); Put(bytes,20,kSceneProbeRecordStride,4);
    Put(bytes,24,asset.value,8); Put(bytes,32,total,8); Put(bytes,40,baked?2:1,4); Put(bytes,44,probes.size(),4);
    Put(bytes,48,d.edge,4); Put(bytes,52,mips,4); Put(bytes,56,d.ggx_samples,4); Put(bytes,60,static_cast<std::uint32_t>(d.blend),4);
    Put(bytes,64,d.revisions.environment,8); Put(bytes,72,d.revisions.material,8); Put(bytes,80,d.revisions.light,8); Put(bytes,88,d.revisions.static_layout,8);
    Put(bytes,96,dependencies.size(),4); Put(bytes,100,d.environment_dependency,4); Put(bytes,104,d.material_dependency,4);
    Put(bytes,108,d.light_dependency,4); Put(bytes,112,d.static_layout_dependency,4); Put(bytes,120,depBytes,8);
    Put(bytes,128,rawBytes,8); Put(bytes,136,ggxBytes,8); Put(bytes,144,shBytes,8); Put(bytes,152,distanceBytes,8);
    std::size_t offset=kSceneProbeHeaderStride;
    for (const auto &id : dependencies) { Put(bytes,offset,id.size(),2); offset+=2; std::memcpy(bytes.data()+offset,id.data(),id.size()); offset+=id.size(); }
    std::uint64_t previous=0;
    for (const auto &p : probes) { Require(p.stable_id>previous,"probe identities must be sorted unique"); previous=p.stable_id; EncodeRecord(bytes,offset,p); offset+=kSceneProbeRecordStride; }
    for (auto texel : bake.radiance) { Encode4(bytes,offset,texel,false); offset+=16; }
    for (auto texel : bake.prefiltered) { Encode4(bytes,offset,texel,false); offset+=16; }
    for (auto texel : bake.cosine_sh) { Encode4(bytes,offset,texel,true); offset+=16; }
    for (std::size_t i=0;i<bake.first_hit_distance.size();++i)
    {
        const auto distance=bake.first_hit_distance[i];
        Require(std::isfinite(distance) && distance>=0 && distance<=probes[i/(std::size_t(d.edge)*d.edge*6)].capture_distance,"invalid first-hit distance");
        PutFloat(bytes,offset,distance); offset+=4;
    }
    const auto digest=PackageDigest(bytes);
    std::copy(digest.begin(),digest.end(),bytes.begin()+160); return bytes;
}

SceneProbePackageView ReadSceneProbePackage(std::span<const std::byte> bytes,AssetId expected)
{
    Require(bytes.size()>=kSceneProbeHeaderStride && bytes.size()<=kMaximumBytes && expected.value &&
        std::equal(kMagic.begin(),kMagic.end(),bytes.begin()) && Get(bytes,8,4)==kSceneProbeVersion && Get(bytes,12,4)==1 &&
        Get(bytes,16,4)==kSceneProbeHeaderStride && Get(bytes,20,4)==kSceneProbeRecordStride &&
        Get(bytes,24,8)==expected.value && Get(bytes,32,8)==bytes.size() && Get(bytes,116,4)==0,"invalid package header/identity");
    const auto digest=PackageDigest(bytes);
    Require(std::equal(digest.begin(),digest.end(),bytes.begin()+160),"payload hash mismatch");
    SceneProbePackageView p; const auto kind=Get(bytes,40,4);
    Require(kind==1 || kind==2,"unsupported payload kind"); p.payload=static_cast<SceneProbePayload>(kind);
    const auto count=Get(bytes,44,4),depCount=Get(bytes,96,4),depBytes=Get(bytes,120,8);
    Require(count && count<=kSceneProbeMaximumCount && depCount && depCount<=256 && depBytes<=kMaximumBytes,"invalid counts");
    auto &d=p.description;
    d.edge=static_cast<std::uint32_t>(Get(bytes,48,4)); d.ggx_samples=static_cast<std::uint32_t>(Get(bytes,56,4));
    d.blend=static_cast<SceneProbeBlend>(Get(bytes,60,4));
    d.revisions={Get(bytes,64,8),Get(bytes,72,8),Get(bytes,80,8),Get(bytes,88,8)};
    d.environment_dependency=static_cast<std::uint32_t>(Get(bytes,100,4)); d.material_dependency=static_cast<std::uint32_t>(Get(bytes,104,4));
    d.light_dependency=static_cast<std::uint32_t>(Get(bytes,108,4)); d.static_layout_dependency=static_cast<std::uint32_t>(Get(bytes,112,4));
    const auto depEnd=Add(kSceneProbeHeaderStride,depBytes); Require(depEnd<=bytes.size(),"dependency range");
    std::size_t offset=kSceneProbeHeaderStride;
    for (std::size_t i=0;i<depCount;++i)
    {
        const auto length=Get(bytes,offset,2); offset+=2;
        Require(offset<=depEnd && length<=depEnd-offset,"dependency string bounds");
        p.dependencies.emplace_back(reinterpret_cast<const char *>(bytes.data()+offset),static_cast<std::size_t>(length)); offset+=static_cast<std::size_t>(length);
    }
    Require(offset==depEnd && ValidateDescription(d,p.dependencies)==depBytes && Get(bytes,52,4)==std::bit_width(d.edge),"dependency/mip layout mismatch");
    Require(Add(offset,Mul(count,kSceneProbeRecordStride))<=bytes.size(),"probe record bounds");
    std::uint64_t previous=0;
    for (std::size_t i=0;i<count;++i) { auto record=DecodeRecord(bytes,offset); Require(record.stable_id>previous,"duplicate/unsorted probe identity"); previous=record.stable_id; p.probes.push_back(record); offset+=kSceneProbeRecordStride; }
    const bool baked=p.payload==SceneProbePayload::Baked;
    const auto raw=baked?Mul(Texels(d.edge,1,count),16):0,ggx=baked?Mul(Texels(d.edge,std::bit_width(d.edge),count),16):0,
        sh=baked?Mul(Mul(count,9),16):0,distance=baked?Mul(Texels(d.edge,1,count),4):0;
    Require(Get(bytes,128,8)==raw && Get(bytes,136,8)==ggx && Get(bytes,144,8)==sh && Get(bytes,152,8)==distance &&
        Add(Add(Add(Add(offset,raw),ggx),sh),distance)==bytes.size(),"payload counts/strides/extent mismatch");
    if (baked)
    {
        p.radiance=CubeView(bytes.subspan(offset,static_cast<std::size_t>(raw)),d.edge,1,count,p.dependencies); offset+=static_cast<std::size_t>(raw);
        p.prefiltered=CubeView(bytes.subspan(offset,static_cast<std::size_t>(ggx)),d.edge,std::bit_width(d.edge),count,p.dependencies); offset+=static_cast<std::size_t>(ggx);
        p.cosine_sh=bytes.subspan(offset,static_cast<std::size_t>(sh)); offset+=static_cast<std::size_t>(sh);
        for (std::size_t at=0;at<sh;at+=16) Validate4({GetFloat(p.cosine_sh,at),GetFloat(p.cosine_sh,at+4),GetFloat(p.cosine_sh,at+8),GetFloat(p.cosine_sh,at+12)},true);
        p.first_hit_distance=bytes.subspan(offset,static_cast<std::size_t>(distance));
        for (std::size_t at=0;at<distance;at+=4)
        {
            const auto value=GetFloat(p.first_hit_distance,at),maximum=p.probes[at/(std::size_t(d.edge)*d.edge*6*4)].capture_distance;
            Require(std::isfinite(value) && value>=0 && value<=maximum,"invalid first-hit-distance payload");
        }
    }
    return p;
}
} // namespace hs::render_data
