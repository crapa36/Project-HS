#include "../AssetPipeline/action.hpp"
#include "radiance_hdr.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/render_data/lighting_probe.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace
{
namespace ap = hs::asset_pipeline;
namespace rd = hs::render_data;
namespace fs = std::filesystem;
constexpr std::string_view kProduct = "render/lighting/default_probe.rpkg";
constexpr std::string_view kNamespace = "render/lighting/default_probe";
constexpr double kPi = std::numbers::pi;
struct Vec3 { double x{}, y{}, z{}; };
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 operator*(Vec3 a, double s) { return {a.x*s,a.y*s,a.z*s}; }
double Dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Vec3 Normalize(Vec3 a) { return a*(1.0/std::sqrt(Dot(a,a))); }
void Require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error("lighting probe baker: " + std::string(message));
}
void Keys(const ap::Json &object, std::initializer_list<std::string_view> keys)
{
    Require(object.is_object() && object.size()==keys.size(), "invalid object keys");
    for (auto key:keys) Require(object.contains(std::string(key)), "missing required key");
}
double Number(const ap::Json &value, double low, double high)
{
    Require(value.is_number(), "expected numeric value");
    const auto number=value.get<double>();
    Require(std::isfinite(number) && number>=low && number<=high, "numeric value outside source bounds");
    return number;
}
Vec3 Radiance(const ap::Json &value)
{
    Require(value.is_array() && value.size()==3, "radiance must have three components");
    return {Number(value[0],0,1e6),Number(value[1],0,1e6),Number(value[2],0,1e6)};
}
struct Source
{
    Vec3 bottom, top;
    double scale{}, yaw{};
    std::uint32_t edge{}, samples{};
    std::string hdr_path; // Empty only for the existing authored hemisphere.
    hs::lighting_probe::HdrPrimariesPolicy primaries_policy{hs::lighting_probe::HdrPrimariesPolicy::RequireDeclaredRec709};
};
std::string HdrSourcePath(const ap::Json &value)
{
    Require(value.is_string(),"HDR source_path must be a portable path string");
    const auto path=value.get<std::string>();
    Require(path.size()<=512 && path.starts_with("ContentSource/") && path.ends_with(".hdr") && path.find("//")==std::string::npos,
        "HDR source_path must reference a .hdr file inside ContentSource");
    for(unsigned char character:path)
        Require((character>='a' && character<='z') || (character>='A' && character<='Z') ||
            (character>='0' && character<='9') || character=='/' || character=='_' || character=='-' || character=='.',
            "HDR source_path contains a nonportable character");
    const auto relative=fs::path(path);
    Require(!relative.is_absolute() && !relative.has_root_name() && !relative.has_root_directory(),"HDR path must be source relative");
    for(const auto &component:relative)
        Require(!component.empty() && component!="." && component!="..","HDR path contains traversal or an empty component");
    Require(relative.generic_string()==path,"HDR source_path must be canonical forward-slash spelling");
    return path;
}
fs::path ResolveHdrSource(const fs::path &sourceRoot,std::string_view source_path)
{
    const auto contentRoot=fs::canonical(sourceRoot/"ContentSource"),resolved=fs::canonical(sourceRoot/fs::path(source_path));
    const auto relative=resolved.lexically_relative(contentRoot);
    Require(!relative.empty() && !relative.is_absolute(),"HDR source does not resolve inside ContentSource");
    for(const auto &component:relative) Require(component!="..","HDR symlink/junction escapes ContentSource");
    ap::RequireRegularFile(resolved);
    Require(fs::file_size(resolved)<=512ull*1024*1024,"HDR source exceeds 512 MiB input budget");
    return resolved;
}
Source ParseSource(std::string_view text)
{
    std::vector<std::unordered_set<std::string>> objectKeys;
    const auto root=ap::Json::parse(text,[&](int,ap::Json::parse_event_t event,ap::Json &parsed) {
        if(event==ap::Json::parse_event_t::object_start) objectKeys.emplace_back();
        else if(event==ap::Json::parse_event_t::key)
            Require(!objectKeys.empty() && objectKeys.back().insert(parsed.get<std::string>()).second,"duplicate object key");
        else if(event==ap::Json::parse_event_t::object_end) objectKeys.pop_back();
        return true;
    });
    Require(root.is_object() && root.contains("kind") && root.at("kind").is_string(),"missing source kind");
    const bool hdr=root.at("kind")=="hdr_equirectangular";
    if(hdr && root.contains("source_primaries_policy"))
        Keys(root,{"version","id","kind","color_space","radiance_unit","sun_policy","source_path",
            "source_encoding","source_primaries_policy","panorama_mapping","radiance_scale","orientation_y_degrees","bake","author_notes"});
    else if(hdr) Keys(root,{"version","id","kind","color_space","radiance_unit","sun_policy","source_path",
        "source_encoding","panorama_mapping","radiance_scale","orientation_y_degrees","bake","author_notes"});
    else Keys(root,{"version","id","kind","color_space","radiance_unit","sun_policy",
        "bottom_radiance","top_radiance","radiance_scale","orientation_y_degrees","bake","author_notes"});
    Require(root.at("version").is_number_integer() && root.at("version")==1 &&
            root.at("id")=="default_probe" && (hdr || root.at("kind")=="hemisphere_radiance") &&
            root.at("color_space")=="rec709_linear" &&
            root.at("radiance_unit")=="scene_linear_radiance_per_steradian" &&
            root.at("sun_policy")=="separate_directional_light", "unsupported source identity or radiance convention");
    const auto &notes=root.at("author_notes");
    Require(notes.is_string() && !notes.get_ref<const std::string &>().empty() &&
            notes.get_ref<const std::string &>().size()<=2048, "invalid author notes");
    const auto &bake=root.at("bake"); Keys(bake,{"cube_edge","ggx_samples"});
    Require(bake.at("cube_edge").is_number_unsigned() && bake.at("ggx_samples").is_number_unsigned(),
            "bake settings must be unsigned integers");
    Source source;
    source.scale=Number(root.at("radiance_scale"),0,1e6);
    source.yaw=Number(root.at("orientation_y_degrees"),-180,180)*kPi/180;
    source.edge=static_cast<std::uint32_t>(Number(bake.at("cube_edge"),16,256));
    source.samples=static_cast<std::uint32_t>(Number(bake.at("ggx_samples"),64,4096));
    if(hdr)
    {
        Require(root.at("source_encoding")=="radiance_rgbe" &&
            root.at("panorama_mapping")=="longitude_atan2_z_x_latitude_acos_y","unsupported HDR encoding or spherical mapping");
        source.hdr_path=HdrSourcePath(root.at("source_path"));
        if(root.contains("source_primaries_policy"))
        {
            const auto &policy=root.at("source_primaries_policy");
            Require(policy=="require_declared_rec709" || policy=="authored_rec709_if_unspecified",
                "unsupported HDR primaries policy");
            if(policy=="authored_rec709_if_unspecified")
                source.primaries_policy=hs::lighting_probe::HdrPrimariesPolicy::AuthoredRec709IfUnspecified;
        }
    }
    else { source.bottom=Radiance(root.at("bottom_radiance")); source.top=Radiance(root.at("top_radiance")); }
    Require(source.edge>=16 && source.edge<=256 && std::has_single_bit(source.edge) &&
            source.samples>=64 && source.samples<=4096 && std::has_single_bit(source.samples), "invalid bake budget");
    return source;
}
// D3D cube slices +X,-X,+Y,-Y,+Z,-Z, with texture v increasing downwards.
Vec3 Direction(std::uint32_t face, double u, double v)
{
    switch(face)
    {
    case 0:return Normalize({1,-v,-u}); case 1:return Normalize({-1,-v,u});
    case 2:return Normalize({u,1,v}); case 3:return Normalize({u,-1,-v});
    case 4:return Normalize({u,-v,1}); default:return Normalize({-u,-v,-1});
    }
}
struct CubeUv { std::uint32_t face{}; double u{},v{}; };
CubeUv Project(Vec3 d)
{
    const double x=std::abs(d.x),y=std::abs(d.y),z=std::abs(d.z);
    if(x>=y && x>=z) return d.x>=0 ? CubeUv{0,-d.z/x,-d.y/x}:CubeUv{1,d.z/x,-d.y/x};
    if(y>=z) return d.y>=0 ? CubeUv{2,d.x/y,d.z/y}:CubeUv{3,d.x/y,-d.z/y};
    return d.z>=0 ? CubeUv{4,d.x/z,-d.y/z}:CubeUv{5,-d.x/z,-d.y/z};
}
Vec3 Color(hs::Float4 c) { return {c.x,c.y,c.z}; }
hs::Float4 Texel(Vec3 c)
{
    Require(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && c.x>=0 && c.y>=0 && c.z>=0 &&
        c.x<=std::numeric_limits<float>::max() && c.y<=std::numeric_limits<float>::max() &&
        c.z<=std::numeric_limits<float>::max(),"scaled/convolved radiance exceeds finite float32 convention");
    return {float(c.x),float(c.y),float(c.z),1.0f};
}
Vec3 SamplePanorama(const hs::lighting_probe::RadianceHdrPanorama &panorama,Vec3 direction)
{
    // Local +X at u=.5, +Z at u=.75, -X longitude seam; +Y at v=0.
    // The existing probe metadata owns yaw, so no rotation is baked twice.
    const double u=0.5+std::atan2(direction.z,direction.x)/(2*kPi),v=std::acos(std::clamp(direction.y,-1.0,1.0))/kPi;
    const double px=u*panorama.header.width-0.5,py=v*panorama.header.height-0.5;
    const int x=int(std::floor(px)),y=int(std::floor(py)),width=int(panorama.header.width),height=int(panorama.header.height);
    auto tap=[&](int tx,int ty) {
        return Color(panorama.At(std::uint32_t((tx%width+width)%width),std::uint32_t(std::clamp(ty,0,height-1))));
    };
    const double fx=px-x,fy=py-y;
    return (tap(x,y)*(1-fx)+tap(x+1,y)*fx)*(1-fy)+(tap(x,y+1)*(1-fx)+tap(x+1,y+1)*fx)*fy;
}
Vec3 SampleCube(const std::vector<hs::Float4> &cube,std::uint32_t edge,Vec3 d)
{
    const auto p=Project(d);
    const double px=(p.u+1)*0.5*edge-0.5,py=(p.v+1)*0.5*edge-0.5;
    const int x=int(std::floor(px)),y=int(std::floor(py));
    auto tap=[&](int tx,int ty) {
        auto face=p.face;
        if(tx<0 || ty<0 || tx>=int(edge) || ty>=int(edge))
        {
            const auto adjacent=Project(Direction(face,2*(double(tx)+0.5)/edge-1,2*(double(ty)+0.5)/edge-1));
            face=adjacent.face;
            tx=std::clamp(int((adjacent.u+1)*0.5*edge),0,int(edge)-1);
            ty=std::clamp(int((adjacent.v+1)*0.5*edge),0,int(edge)-1);
        }
        return Color(cube[(std::size_t(face)*edge+std::size_t(ty))*edge+std::size_t(tx)]);
    };
    const double fx=px-x,fy=py-y;
    return (tap(x,y)*(1-fx)+tap(x+1,y)*fx)*(1-fy)+(tap(x,y+1)*(1-fx)+tap(x+1,y+1)*fx)*fy;
}
std::array<double,9> ShBasis(Vec3 n)
{
    return {0.28209479177387814,0.4886025119029199*n.y,0.4886025119029199*n.z,
            0.4886025119029199*n.x,1.0925484305920792*n.x*n.y,1.0925484305920792*n.y*n.z,
            0.31539156525252005*(3*n.z*n.z-1),1.0925484305920792*n.x*n.z,
            0.5462742152960396*(n.x*n.x-n.y*n.y)};
}
double SolidAngle(double x,double y) { return std::atan2(x*y,std::sqrt(x*x+y*y+1)); }
// Integrates each cubemap texel's exact solid angle, rather than treating corner
    // texels like center texels. Cosine convolution multipliers: pi,2pi/3,pi/4.
rd::LightingProbeIrradiance ProjectIrradiance(const std::vector<hs::Float4> &cube,std::uint32_t edge)
{
    std::array<Vec3,9> coefficients{}; double total=0;
    for(std::uint32_t face=0;face<6;++face)
        for(std::uint32_t y=0;y<edge;++y) for(std::uint32_t x=0;x<edge;++x)
        {
            const double x0=2*double(x)/edge-1,x1=2*double(x+1)/edge-1;
            const double y0=2*double(y)/edge-1,y1=2*double(y+1)/edge-1;
            const double omega=SolidAngle(x1,y1)-SolidAngle(x0,y1)-SolidAngle(x1,y0)+SolidAngle(x0,y0);
            const auto basis=ShBasis(Direction(face,(x0+x1)*0.5,(y0+y1)*0.5));
            const auto color=Color(cube[(std::size_t(face)*edge+y)*edge+x]); total+=omega;
            for(std::size_t i=0;i<9;++i) coefficients[i]=coefficients[i]+color*(basis[i]*omega);
        }
    Require(std::isfinite(total) && total>0, "invalid cubemap solid angle");
    rd::LightingProbeIrradiance result;
    for(std::size_t i=0;i<9;++i)
    {
        const double convolution=i==0?kPi:i<4?2*kPi/3:kPi/4;
        const auto c=coefficients[i]*(4*kPi/total*convolution);
        result[i]={float(c.x),float(c.y),float(c.z),0};
    }
    return result;
}
double RadicalInverse(std::uint32_t bits)
{
    bits=(bits<<16)|(bits>>16);
    bits=((bits&0x55555555u)<<1)|((bits&0xaaaaaaaau)>>1);
    bits=((bits&0x33333333u)<<2)|((bits&0xccccccccu)>>2);
    bits=((bits&0x0f0f0f0fu)<<4)|((bits&0xf0f0f0f0u)>>4);
    bits=((bits&0x00ff00ffu)<<8)|((bits&0xff00ff00u)>>8);
    return double(bits)*2.3283064365386963e-10;
}
std::vector<hs::Float4> Prefilter(const std::vector<hs::Float4> &raw,const Source &source)
{
    const auto mips=std::bit_width(source.edge);
    std::vector<hs::Float4> result;
    // Karis, Real Shading in UE4 (2013): split-sum environment convolution,
    // alpha=roughness^2, N=V=R, GGX NDF samples weighted/normalized by NdotL.
    // Actual baked raw cube samples are used, with cross-face bilinear taps.
    for(std::uint32_t face=0;face<6;++face) for(std::uint32_t mip=0;mip<mips;++mip)
    {
        const auto edge=std::max(1u,source.edge>>mip);
        const double roughness=double(mip)/(mips-1),alpha=roughness*roughness;
        for(std::uint32_t y=0;y<edge;++y) for(std::uint32_t x=0;x<edge;++x)
        {
            const auto n=Direction(face,2*(double(x)+0.5)/edge-1,2*(double(y)+0.5)/edge-1);
            if(mip==0) { result.push_back(raw[(std::size_t(face)*edge+y)*edge+x]); continue; }
            const auto tangent=Normalize(Cross(std::abs(n.z)<0.999?Vec3{0,0,1}:Vec3{1,0,0},n));
            const auto bitangent=Cross(n,tangent); Vec3 sum{}; double total=0;
            for(std::uint32_t i=0;i<source.samples;++i)
            {
                const double phi=2*kPi*(double(i)+0.5)/source.samples,u=RadicalInverse(i);
                const double cosTheta=std::sqrt((1-u)/(1+(alpha*alpha-1)*u));
                const double sinTheta=std::sqrt(std::max(0.0,1-cosTheta*cosTheta));
                const auto h=tangent*(sinTheta*std::cos(phi))+bitangent*(sinTheta*std::sin(phi))+n*cosTheta;
                const auto l=h*(2*Dot(n,h))-n; const double nl=std::max(0.0,Dot(n,l));
                if(nl>0) { sum=sum+SampleCube(raw,source.edge,l)*nl; total+=nl; }
            }
            Require(total>0 && std::isfinite(total), "invalid GGX convolution weight");
            result.push_back(Texel(sum*(1/total)));
        }
    }
    return result;
}
std::vector<std::byte> Bake(const Source &source,const hs::lighting_probe::RadianceHdrPanorama *panorama)
{
    std::vector<hs::Float4> raw; raw.reserve(std::size_t(source.edge)*source.edge*6);
    for(std::uint32_t face=0;face<6;++face)
        for(std::uint32_t y=0;y<source.edge;++y) for(std::uint32_t x=0;x<source.edge;++x)
        {
            const auto d=Direction(face,2*(double(x)+0.5)/source.edge-1,2*(double(y)+0.5)/source.edge-1);
            if(panorama) raw.push_back(Texel(SamplePanorama(*panorama,d)*source.scale));
            else
            {
                const double blend=(d.y+1)*0.5;
                raw.push_back(Texel((source.bottom*(1-blend)+source.top*blend)*source.scale));
            }
        }
    const auto filtered=Prefilter(raw,source);
    const auto sh=ProjectIrradiance(raw,source.edge);
    return rd::BuildLightingProbePackage(hs::MakeAssetId(kProduct),{},
        {source.edge,float(source.yaw)},raw,filtered,sh);
}
void Produce(const fs::path &sourceRoot,const fs::path &buildRoot,const fs::path &executable)
{
    Require(sourceRoot.is_absolute() && buildRoot.is_absolute(), "roots must be absolute");
    ap::RequireDirectory(sourceRoot); ap::MakeDirectory(buildRoot);
    ap::ActionSpec spec;
    spec.action_id="render.lighting.default_probe"; spec.recipe_id="project-hs.render.lighting-probe";
    spec.recipe_version=3; spec.namespace_path=kNamespace;
    spec.inputs.push_back(ap::HashInput("tool/hs_lighting_probe",executable));
    const auto sourcePath=sourceRoot/"ContentSource/Lighting/default_probe.json";
    // Hash and bake the same source snapshot, even if an editor subsequently
    // replaces the authored JSON while this action is preparing its output.
    const auto sourceBytes=ap::ReadBytes(sourcePath);
    spec.inputs.push_back({"ContentSource/Lighting/default_probe.json",sourcePath,
        ap::Sha256Bytes(std::as_bytes(std::span(sourceBytes.data(),sourceBytes.size())))});
    const auto source=ParseSource(sourceBytes);
    std::string hdrBytes;
    if(!source.hdr_path.empty())
    {
        const auto hdrPath=ResolveHdrSource(sourceRoot,source.hdr_path);
        // The exact same byte snapshot is both a semantic Action input and
        // decoded source. Absolute host paths never enter product identity.
        hdrBytes=ap::ReadBytes(hdrPath);
        spec.inputs.push_back({source.hdr_path,hdrPath,
            ap::Sha256Bytes(std::as_bytes(std::span(hdrBytes.data(),hdrBytes.size())))});
    }
    for(const auto *relative:{"Schemas/lighting_probe.schema.json",
        "Source/Apps/LightingProbe/main.cpp","Source/Apps/LightingProbe/radiance_hdr.hpp",
        "Source/Apps/AssetPipeline/action.hpp","Source/Apps/AssetPipeline/action.cpp",
        "Source/RenderData/Public/hs/render_data/lighting_probe.hpp","Source/RenderData/Private/lighting_probe.cpp",
        "Source/RenderData/Public/hs/render_data/render_asset_format.hpp","Source/RenderData/Private/render_asset_format.cpp",
        "Source/RenderData/Public/hs/render_data/texture_package.hpp",
        "Source/Core/Public/hs/core/cooked_format.hpp","Source/Core/Private/cooked_format.cpp",
        "Source/Core/Public/hs/core/sha256.hpp","Source/Core/Private/sha256.cpp"})
        spec.inputs.push_back(ap::HashInput(relative,sourceRoot/relative));
    std::ranges::sort(spec.inputs,{},&ap::Input::logical_id);
    const auto resultPath=buildRoot/"Actions"/kNamespace/"result.json";
    if(fs::exists(resultPath) && fs::exists(resultPath.parent_path()/"result.d"))
    {
        try
        {
            const auto current=ap::Json::parse(ap::ReadBytes(resultPath));
            if(current.at("action_id")==spec.action_id && current.at("action_key")==ap::ActionKey(spec) && current.at("products").size()==1)
            {
                ap::UpstreamProduct ignored; fs::path path;
                (void)ap::ReadActionResult(buildRoot,kNamespace,kProduct,ignored,path);
                const auto existing=ap::ReadBytes(path);
                (void)rd::ReadLightingProbePackage(std::as_bytes(std::span(existing.data(),existing.size())),hs::MakeAssetId(kProduct));
                ap::TouchActionResult(resultPath); std::cout<<spec.action_id<<" current\n"; return;
            }
        }
        catch(const std::exception &) { /* Invalid immutable/current result is rebuilt by the normal action. */ }
    }
    const auto stage=ap::CreateStage(buildRoot/"Actions"/kNamespace);
    try
    {
        std::optional<hs::lighting_probe::RadianceHdrPanorama> panorama;
        if(!source.hdr_path.empty()) panorama.emplace(hs::lighting_probe::ReadRadianceHdr(hdrBytes,source.primaries_policy));
        const auto bytes=Bake(source,panorama?&*panorama:nullptr);
        (void)rd::ReadLightingProbePackage(bytes,hs::MakeAssetId(kProduct));
        const auto path=stage/"default_probe.rpkg"; ap::WriteBytes(path,bytes);
        ap::Publish(buildRoot,spec,stage,{ap::HashProduct(std::string(kProduct),"default_probe.rpkg",path)});
    }
    catch(...) { ap::RemoveStage(stage); throw; }
    ap::RemoveStage(stage); std::cout<<spec.action_id<<" published\n";
}
} // namespace
#ifdef _WIN32
int wmain(int argc,wchar_t **argv)
#else
int main(int argc,char **argv)
#endif
{
    fs::path sourceRoot,buildRoot; bool produce=false;
    try
    {
        for(int i=1;i<argc;++i)
        {
            const fs::path arg(argv[i]);
            if(arg=="--produce" && !produce) produce=true;
            else if(arg=="--source-root" && sourceRoot.empty() && ++i<argc) sourceRoot=argv[i];
            else if(arg=="--build-root" && buildRoot.empty() && ++i<argc) buildRoot=argv[i];
            else throw std::runtime_error("usage: hs_lighting_probe --produce --source-root <absolute> --build-root <absolute>");
        }
        Require(produce && !sourceRoot.empty() && !buildRoot.empty(),
                "usage: hs_lighting_probe --produce --source-root <absolute> --build-root <absolute>");
        Produce(sourceRoot.lexically_normal(),buildRoot.lexically_normal(),fs::absolute(fs::path(argv[0])).lexically_normal()); return 0;
    }
    catch(const std::exception &error)
    {
        ap::ReportFailure("hs_lighting_probe", error.what(),
                          {{"action_id", "render.lighting.default_probe"}, {"produce", produce},
                           {"source_root", ap::DiagnosticPath(sourceRoot)}, {"build_root", ap::DiagnosticPath(buildRoot)}});
        return 1;
    }
}
