#include "environment_texture_cooker.hpp"
#include "environment_histogram_cooker.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXTex.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

namespace hs::content
{
namespace
{
using Json = nlohmann::json;
struct Pixel { float r{}, g{}, b{}, a{}; };
struct Rect { int page{}, x{}, y{}, w{}, h{}; };
struct Group { std::string name; std::vector<Json> pages; std::vector<Rect> rects; bool foliage{}; };
void Check(HRESULT hr, const std::string& action) { if (FAILED(hr)) throw std::runtime_error(action + " (HRESULT " + std::to_string(hr) + ")"); }
Json Read(const std::filesystem::path& root)
{
    std::ifstream input(root / "project_hs_environment_materials_v9_LEAN.json");
    if (!input) throw std::runtime_error("Missing environment material manifest");
    Json j; input >> j;
    if (j.at("schema_version") != 9 || j.at("target").at("texture_size_px") != 2048 ||
        j.at("texture_cook").at("mip_levels").at("opaque") != 12 ||
        j.at("texture_cook").at("mip_levels").at("foliage") != 6)
        throw std::runtime_error("Unsupported environment texture contract");
    std::ifstream baked_input(root / "baked_materials.json");
    if (!baked_input) throw std::runtime_error("Missing Blender baked material manifest");
    Json baked; baked_input >> baked;
    if (baked.at("version") != 1 || baked.at("bark").size() != 3 || baked.at("rock").size() != 4)
        throw std::runtime_error("Invalid Blender baked material arrays");
    j["baked_materials"] = std::move(baked);
    return j;
}
std::vector<Group> Groups(const Json& j)
{
    std::vector<Group> groups;
    Group terrain{"terrain"};
    for (const auto& layer : j.at("terrain").at("layers")) {
        if (layer.at("slice").get<size_t>() != terrain.pages.size()) throw std::runtime_error("Environment terrain slices must be ordered");
        terrain.pages.push_back(layer.at("textures"));
    }
    if (terrain.pages.size() != 4) throw std::runtime_error("Expected four terrain slices");
    groups.push_back(std::move(terrain));
    for (const auto& material : j.at("opaque_mesh").at("materials")) {
        const auto id = material.at("id").get<std::string>();
        if (id != "mesh_rock" && id != "mesh_bark") throw std::runtime_error("Unknown environment material " + id);
        Group group{id.substr(5)};
        for (const auto &page : j.at("baked_materials").at(group.name)) group.pages.push_back(page);
        groups.push_back(std::move(group));
    }
    for (const auto* name : {"grass", "leaf"}) {
        const auto& f = j.at("foliage").at(name);
        Group g{name, {}, {}, true};
        for (const auto& page : f.at("pages")) {
            auto baked_page = page;
            baked_page["normal"] = j.at("baked_materials").at("foliage_normals").at(name).at(g.pages.size());
            g.pages.push_back(std::move(baked_page));
        }
        if (g.pages.size() != (g.name == "grass" ? 2u : 3u)) throw std::runtime_error("Invalid foliage page count");
        for (const auto& e : f.at("entries")) {
            if (e.size() != 6 || !e[5].is_string()) throw std::runtime_error("Invalid foliage entry");
            Rect r{e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>(), e[4].get<int>()};
            if (r.page < 0 || r.page >= static_cast<int>(g.pages.size()) || r.x < 48 || r.y < 48 || r.w <= 0 || r.h <= 0 || r.x+r.w+48 > 2048 || r.y+r.h+48 > 2048)
                throw std::runtime_error("Invalid foliage rectangle/gutter");
            for (const auto& old : g.rects)
                if (old.page == r.page && old.x-48 < r.x+r.w+48 && r.x-48 < old.x+old.w+48 && old.y-48 < r.y+r.h+48 && r.y-48 < old.y+old.h+48)
                    throw std::runtime_error("Overlapping foliage gutters");
            g.rects.push_back(r);
        }
        groups.push_back(std::move(g));
    }
    return groups;
}
std::filesystem::path TexturePath(const std::filesystem::path& root, const Json& page, const std::string& role)
{
    const std::filesystem::path relative(page.at(role).get<std::string>());
    if (relative.is_absolute()) throw std::runtime_error("Absolute environment source path");
    for (const auto& part : relative) if (part == "..") throw std::runtime_error("Environment source path escapes pack");
    return root / relative;
}
std::string HashBytes(const std::string& bytes)
{
    uint64_t hash=14695981039346656037ull;
    for (unsigned char byte:bytes) { hash^=byte; hash*=1099511628211ull; }
    return std::to_string(hash);
}
std::string HashFile(const std::filesystem::path& path)
{
    std::ifstream input(path,std::ios::binary);
    if (!input) throw std::runtime_error("Cannot hash "+path.string());
    uint64_t hash=14695981039346656037ull;
    std::array<char,65536> bytes{};
    while (input) {
        input.read(bytes.data(),bytes.size());
        for (std::streamsize i=0;i<input.gcount();++i) { hash^=static_cast<unsigned char>(bytes[static_cast<size_t>(i)]); hash*=1099511628211ull; }
    }
    if (!input.eof()) throw std::runtime_error("Failed hashing "+path.string());
    return std::to_string(hash);
}
std::filesystem::path OutputPath(const std::filesystem::path& output,const Group& group,const std::string& role)
{
    const std::string suffix=role=="base_color"?"basecolor":role=="surface_ra"?"surface":role=="detail_normal"?"detail":role;
    return output/("environment_"+group.name+"_"+suffix+".dds");
}
std::string InputHash(const std::filesystem::path& root,const Json& manifest,const Group& group,const std::string& role,bool gpu)
{
    // Bump the recipe for any mip/filter/encoder change. No build timestamps: unrelated
    // mesh or shader builds must retain identical texture cache identities.
    Json identity{{"recipe","environment-v9-mips-1-bc7-d3d11-1"},{"manifest",manifest},
        {"group",group.name},{"role",role},{"encoder",role=="base_color"&&gpu?"gpu":"cpu"}};
    std::set<std::filesystem::path> files;
    for (const auto& page:group.pages) {
        files.insert(TexturePath(root,page,role));
        if (role=="surface_ra"||role=="roughness") files.insert(TexturePath(root,page,"normal"));
        if (group.foliage) files.insert(TexturePath(root,page,"base_color"));
    }
    for (const auto& file:files) identity["sources"][file.lexically_relative(root).generic_string()]=HashFile(file);
    return HashBytes(identity.dump());
}
bool CacheHit(const std::filesystem::path& path,const std::string& inputHash)
{
    if (!std::filesystem::is_regular_file(path)) return false;
    std::ifstream stream(path.string()+".cache.json");
    if (!stream) return false;
    const auto cache=Json::parse(stream,nullptr,false);
    return cache.is_object()&&cache.contains("input")&&cache["input"].is_string()&&
        cache.contains("output")&&cache["output"].is_string()&&
        cache["input"]==inputHash&&cache["output"]==HashFile(path);
}
std::vector<Pixel> Load(const std::filesystem::path& path, const std::string& role)
{
    // Check PNG IHDR, including precision before WIC conversion.
    std::array<unsigned char, 26> header{};
    std::ifstream f(path, std::ios::binary); f.read(reinterpret_cast<char*>(header.data()), header.size());
    const std::array<unsigned char, 8> signature{137,80,78,71,13,10,26,10};
    if (!f || !std::equal(signature.begin(), signature.end(), header.begin()) ||
        (role == "height" && (header[24] != 16 || header[25] != 0)) ||
        ((role == "normal" || role == "detail_normal") && (header[24] != 8 || header[25] != 2)))
        throw std::runtime_error("Invalid environment PNG role/precision: " + path.string());
    DirectX::ScratchImage source, converted;
    DirectX::TexMetadata metadata{};
    Check(DirectX::LoadFromWICFile(path.c_str(), DirectX::WIC_FLAGS_IGNORE_SRGB, &metadata, source), "Load " + path.string());
    if (metadata.width != 2048 || metadata.height != 2048) throw std::runtime_error("Environment PNG must be 2048 square: " + path.string());
    Check(DirectX::Convert(source.GetImages(), source.GetImageCount(), metadata, DXGI_FORMAT_R32G32B32A32_FLOAT,
        DirectX::TEX_FILTER_DEFAULT, 0, converted), "Convert " + path.string());
    std::vector<Pixel> pixels(2048*2048);
    const auto* image = converted.GetImage(0,0,0);
    for (size_t y=0; y<2048; ++y) std::memcpy(pixels.data()+y*2048, image->pixels+y*image->rowPitch, 2048*sizeof(Pixel));
    return pixels;
}
float Linear(float x) { return x <= .04045f ? x/12.92f : std::pow((x+.055f)/1.055f,2.4f); }
float Srgb(float x) { return x <= .0031308f ? x*12.92f : 1.055f*std::pow(x,1.f/2.4f)-.055f; }
float Length(const Pixel& p) { return std::sqrt(p.r*p.r+p.g*p.g+p.b*p.b); }
Pixel Average(const std::vector<Pixel>& src, int x0,int y0,int x1,int y1, bool normal, bool color, const std::vector<Pixel>* mask = nullptr)
{
    Pixel p{}; float weightSum=0;
    for(int y=y0;y<y1;++y) for(int x=x0;x<x1;++x) {
        auto s=src[static_cast<size_t>(y)*2048+x];
        if(normal) { s.r=s.r*2-1; s.g=s.g*2-1; s.b=s.b*2-1; }
        if(color) { s.r=Linear(s.r);s.g=Linear(s.g);s.b=Linear(s.b); }
        const float weight=mask?(*mask)[static_cast<size_t>(y)*2048+x].a:1.f;
        p.r+=s.r*weight;p.g+=s.g*weight;p.b+=s.b*weight;p.a+=s.a*weight;weightSum+=weight;
    }
    const float n=std::max(weightSum,1e-8f);
    p.r/=n;p.g/=n;p.b/=n;p.a/=n;
    return p;
}
void Encode(Pixel& p, bool normal, bool color)
{
    if(normal) { const float length=Length(p); if(length>1e-8f) {p.r/=length;p.g/=length;p.b/=length;} else {p.r=0;p.g=0;p.b=1;} p.r=p.r*.5f+.5f;p.g=p.g*.5f+.5f;p.b=p.b*.5f+.5f; }
    if(color) {p.r=Srgb(p.r);p.g=Srgb(p.g);p.b=Srgb(p.b);}
}
float RoughnessAA(float roughness,float r)
{
    if(r>=.9999f) return roughness;
    const float k=(3*r-r*r*r)/std::max(1-r*r,1e-5f);
    return std::clamp(std::sqrt(roughness*roughness+1/std::max(k,1e-5f)),0.f,1.f);
}
std::vector<std::string> Roles(const Group& g)
{
    if(g.foliage) return {"base_color","normal","roughness"};
    if(g.name=="terrain") return {"base_color","surface_ra","normal","detail_normal","height"};
    return {"base_color","surface_ra","normal","detail_normal"};
}
void CookRole(const std::filesystem::path& root,const std::filesystem::path& output,const Group& group,const std::string& role,ID3D11Device* device)
{
    const bool normal=role=="normal"||role=="detail_normal", color=role=="base_color";
    const bool rough=role=="surface_ra"||role=="roughness";
    const int mips=group.foliage?6:12;
    DirectX::ScratchImage array;
    Check(array.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM,2048,2048,group.pages.size(),mips),"Allocate environment array");
    std::memset(array.GetPixels(),0,array.GetPixelsSize());
    for(size_t page=0;page<group.pages.size();++page) {
        const auto source=Load(TexturePath(root,group.pages[page],role),role);
        const auto normals=rough?Load(TexturePath(root,group.pages[page],"normal"),"normal"):std::vector<Pixel>{};
        const auto alpha=group.foliage?Load(TexturePath(root,group.pages[page],"base_color"),"base_color"):std::vector<Pixel>{};
        std::vector<Rect> rects;
        if(group.foliage) {for(const auto& r:group.rects) if(r.page==static_cast<int>(page)) rects.push_back(r);}
        else rects.push_back(Rect{0,0,0,2048,2048});
        for(int mip=0;mip<mips;++mip) {
            const int step=1<<mip, size=2048/step;
            const auto* target=array.GetImage(mip,page,0);
            for(const auto& rect:rects) {
                const int left=rect.x/step,top=rect.y/step,right=(rect.x+rect.w+step-1)/step,bottom=(rect.y+rect.h+step-1)/step;
                const int gutter=group.foliage?(48+step-1)/step:0;
                const int gl=std::max(0,left-gutter),gt=std::max(0,top-gutter),gr=std::min(size,right+gutter),gb=std::min(size,bottom+gutter),w=gr-gl,h=gb-gt;
                std::vector<Pixel> pixels(static_cast<size_t>(w)*h);
                std::vector<int> nearest(pixels.size(),-1);
                std::queue<int> queue;
                for(int y=top;y<bottom;++y) for(int x=left;x<right;++x) {
                    const int x0=std::max(x*step,rect.x),y0=std::max(y*step,rect.y),x1=std::min((x+1)*step,rect.x+rect.w),y1=std::min((y+1)*step,rect.y+rect.h);
                    auto p=Average(source,x0,y0,x1,y1,normal,color,group.foliage?&alpha:nullptr);
                    if(rough&&mip>0) p.r=RoughnessAA(p.r,Length(Average(normals,x0,y0,x1,y1,true,false,group.foliage?&alpha:nullptr)));
                    Encode(p,normal,color);
                    if(group.foliage) p.a=Average(alpha,x0,y0,x1,y1,false,false).a;
                    const int index=(y-gt)*w+x-gl; pixels[index]=p;
                    if(group.foliage&&p.a>0.f) {nearest[index]=index;queue.push(index);}
                }
                // Coverage is measured per entry; a scale is fitted before alpha quantization.
                if(group.foliage&&mip>0) {
                    size_t covered=0;
                    for(int y=rect.y;y<rect.y+rect.h;++y) for(int x=rect.x;x<rect.x+rect.w;++x) if(alpha[static_cast<size_t>(y)*2048+x].a>=.5f) ++covered;
                    const float goal=static_cast<float>(covered)/(rect.w*rect.h);
                    float lo=0,hi=16;
                    for(int iteration=0;iteration<20;++iteration) {
                        const float scale=(lo+hi)*.5f;size_t count=0;
                        for(int y=top;y<bottom;++y) for(int x=left;x<right;++x) if(pixels[(y-gt)*w+x-gl].a*scale>=.5f) ++count;
                        if(static_cast<float>(count)/((right-left)*(bottom-top))<goal) lo=scale;else hi=scale;
                    }
                    for(auto& p:pixels) p.a=std::clamp(p.a*(lo+hi)*.5f,0.f,1.f);
                }
                // Multi-source flood fill dilates all material channels, retaining zero gutter alpha.
                while(!queue.empty()) {
                    const int i=queue.front();queue.pop();
                    const std::array<int,4> adjacent{i%w?i-1:-1,i%w+1<w?i+1:-1,i>=w?i-w:-1,i+w<w*h?i+w:-1};
                    for(int next:adjacent) if(next>=0&&nearest[next]<0) {nearest[next]=nearest[i];queue.push(next);}
                }
                for(int y=gt;y<gb;++y) for(int x=gl;x<gr;++x) {
                    const int i=(y-gt)*w+x-gl;auto p=pixels[i];
                    if(p.a==0&&nearest[i]>=0) {const auto n=pixels[nearest[i]];p.r=n.r;p.g=n.g;p.b=n.b;}
                    auto* dst=target->pixels+static_cast<size_t>(y)*target->rowPitch+x*4;
                    const auto byte=[](float v){return static_cast<uint8_t>(std::lround(std::clamp(v,0.f,1.f)*255));};
                    dst[0]=byte(p.r);dst[1]=byte(p.g);dst[2]=byte(p.b);dst[3]=byte(p.a);
                }
            }
        }
    }
    const auto format=color?DXGI_FORMAT_BC7_UNORM_SRGB:(role=="height"||role=="roughness"?DXGI_FORMAT_BC4_UNORM:DXGI_FORMAT_BC5_UNORM);
    // Values are already encoded as sRGB: tag the input to avoid a second conversion.
    if(color) array.OverrideFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    DirectX::ScratchImage compressed;
    if (color&&device)
        Check(DirectX::Compress(device,array.GetImages(),array.GetImageCount(),array.GetMetadata(),format,DirectX::TEX_COMPRESS_DEFAULT,1.f,compressed),"GPU compress environment "+group.name+" "+role);
    else
        Check(DirectX::Compress(array.GetImages(),array.GetImageCount(),array.GetMetadata(),format,DirectX::TEX_COMPRESS_PARALLEL,.5f,compressed),"CPU compress environment "+group.name+" "+role);
    const auto path=OutputPath(output,group,role);
    Check(DirectX::SaveToDDSFile(compressed.GetImages(),compressed.GetImageCount(),compressed.GetMetadata(),DirectX::DDS_FLAGS_FORCE_DX10_EXT,path.c_str()),"Save "+path.string());
}
}
void ValidateEnvironmentTextures(const std::filesystem::path& source)
{
    const auto manifest = Read(source);
    for(const auto& group:Groups(manifest)) for(const auto& page:group.pages) for(const auto& role:Roles(group))
        (void)Load(TexturePath(source,page,role),role);
    for (const auto *group : {"grass", "leaf"})
        for (const auto &page : manifest.at("foliage").at(group).at("pages"))
            (void)Load(TexturePath(source, page, "normal"), "normal");
    for (const auto &material : manifest.at("opaque_mesh").at("materials"))
        for (const auto *role : {"base_color", "normal", "detail_normal", "surface_ra"})
            (void)Load(TexturePath(source, material.at("textures"), role), role);
}
void CookEnvironmentTextures(const std::filesystem::path& source,const std::filesystem::path& output)
{
    const auto manifest=Read(source);
    const auto groups=Groups(manifest);
    std::filesystem::create_directories(output);
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
    const HRESULT deviceResult=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
        levels,1,D3D11_SDK_VERSION,device.GetAddressOf(),nullptr,nullptr);
    if (FAILED(deviceResult)) {
        // Only device availability selects the CPU path; GPU compression errors remain fatal.
        std::cout<<"Environment BC7: D3D11 hardware unavailable (HRESULT "<<deviceResult<<"); using CPU compressor"<<std::endl;
    } else {
        std::cout<<"Environment BC7: D3D11 GPU compressor"<<std::endl;
    }
    CookEnvironmentHistogram(source, output, device.Get());
    for(const auto& group:groups) for(const auto& role:Roles(group)) {
        const auto path=OutputPath(output,group,role);
        const auto inputHash=InputHash(source,manifest,group,role,device.Get()!=nullptr);
        if (CacheHit(path,inputHash)) {
            std::cout<<"Environment "<<group.name<<" "<<role<<": cache hit"<<std::endl;
            continue;
        }
        std::cout<<"Environment "<<group.name<<" "<<role<<": cooking"<<std::endl;
        CookRole(source,output,group,role,device.Get());
        std::ofstream cache(path.string()+".cache.json",std::ios::trunc);
        cache<<Json{{"input",inputHash},{"output",HashFile(path)}}.dump()<<'\n';
        cache.close();
        if (!cache) throw std::runtime_error("Cannot save texture cache "+path.string());
        std::cout<<"Environment "<<group.name<<" "<<role<<": complete"<<std::endl;
    }
}
}