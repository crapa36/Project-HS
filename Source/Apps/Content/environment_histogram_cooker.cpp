#include "environment_histogram_cooker.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <DirectXTex.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace hs::content
{
namespace
{
using Json = nlohmann::json;
using V3 = std::array<double, 3>;
using F3 = std::array<float, 3>;
using Matrix = std::array<V3, 3>;
constexpr size_t kSize = 2048, kMips = 12, kLayers = 4, kLutWidth = 256;
constexpr double kSigma = 1.0 / 6.0;
// PCA marginal Gaussianization and inverse-LUT prefiltering follow the Unity Labs
// procedural-stochastic-texturing reference (ProceduralTexture2DEditor.cs).
// This implementation uses deterministic Gaussian-quantile quadrature.
constexpr const char* kRecipe = "terrain-pca-midrank-gaussian-v1-linear-bc7-lut256x16-q512";
struct Basis { V3 origin{}; Matrix axes{}; V3 eigenvalues{}; };
using Lut = std::array<std::array<F3, kLutWidth>, kMips>;
struct LayerData { Basis basis; Lut lut{}; Json qa; };

void Check(HRESULT hr, const std::string& action)
{
    if (FAILED(hr)) throw std::runtime_error(action + " (HRESULT " + std::to_string(hr) + ")");
}
std::string HashFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot hash " + path.string());
    uint64_t hash = 14695981039346656037ull;
    std::array<char, 65536> bytes{};
    while (stream) {
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        for (std::streamsize i = 0; i < stream.gcount(); ++i) {
            hash ^= static_cast<unsigned char>(bytes[static_cast<size_t>(i)]);
            hash *= 1099511628211ull;
        }
    }
    if (!stream.eof()) throw std::runtime_error("Failed reading " + path.string());
    return std::to_string(hash);
}
double Linear(double value)
{
    return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
}
// Acklam's inverse normal CDF approximation. Mid-ranks never request an endpoint.
double NormalQuantile(double p)
{
    p = std::clamp(p, 1e-12, 1.0 - 1e-12);
    constexpr double a[] = {-39.69683028665376,220.9460984245205,-275.9285104469687,138.3577518672690,-30.66479806614716,2.506628277459239};
    constexpr double b[] = {-54.47609879822406,161.5858368580409,-155.6989798598866,66.80131188771972,-13.28068155288572};
    constexpr double c[] = {-.007784894002430293,-.3223964580411365,-2.400758277161838,-2.549732539343734,4.374664141464968,2.938163982698783};
    constexpr double d[] = {.007784695709041462,.3224671290700398,2.445134137142996,3.754408661907416};
    if (p < .02425 || p > .97575) {
        const double q = std::sqrt(-2 * std::log(p < .02425 ? p : 1 - p));
        const double x = (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1);
        return p < .02425 ? x : -x;
    }
    const double q = p - .5, r = q*q;
    return (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5])*q / (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1);
}
std::vector<F3> LoadLinear(const std::filesystem::path& path)
{
    DirectX::ScratchImage source, converted;
    DirectX::TexMetadata metadata{};
    Check(DirectX::LoadFromWICFile(path.c_str(), DirectX::WIC_FLAGS_IGNORE_SRGB, &metadata, source), "Load histogram source");
    if (metadata.width != kSize || metadata.height != kSize || metadata.arraySize != 1)
        throw std::runtime_error("Histogram source must be 2048x2048: " + path.string());
    Check(DirectX::Convert(source.GetImages(), source.GetImageCount(), metadata,
        DXGI_FORMAT_R32G32B32A32_FLOAT, DirectX::TEX_FILTER_DEFAULT, 0, converted), "Convert histogram source");
    std::vector<F3> pixels(kSize*kSize);
    const auto* image = converted.GetImage(0,0,0);
    for (size_t y = 0; y < kSize; ++y) {
        const auto* row = reinterpret_cast<const float*>(image->pixels + y*image->rowPitch);
        for (size_t x = 0; x < kSize; ++x)
            for (size_t c = 0; c < 3; ++c) {
                const double value = Linear(row[x*4+c]);
                if (!std::isfinite(value) || value < 0 || value > 1)
                    throw std::runtime_error("Invalid histogram source color");
                pixels[y*kSize+x][c] = static_cast<float>(value);
            }
    }
    return pixels;
}
Basis PrincipalComponents(const std::vector<F3>& pixels)
{
    Basis basis;
    const double count = static_cast<double>(pixels.size());
    for (const auto& pixel : pixels) for (size_t c=0;c<3;++c) basis.origin[c] += pixel[c] / count;
    Matrix covariance{};
    for (const auto& pixel : pixels)
        for (size_t i=0;i<3;++i) for (size_t j=0;j<3;++j)
            covariance[i][j] += (pixel[i]-basis.origin[i])*(pixel[j]-basis.origin[j]) / count;
    Matrix vectors{{{1,0,0},{0,1,0},{0,0,1}}};
    for (size_t iteration=0;iteration<48;++iteration) {
        size_t p=0,q=1;
        for (size_t i=0;i<3;++i) for (size_t j=i+1;j<3;++j)
            if (std::abs(covariance[i][j])>std::abs(covariance[p][q])) { p=i; q=j; }
        if (std::abs(covariance[p][q])<1e-15) break;
        const double tau=(covariance[q][q]-covariance[p][p])/(2*covariance[p][q]);
        const double t=std::copysign(1.0,tau)/(std::abs(tau)+std::sqrt(1+tau*tau));
        const double cosine=1/std::sqrt(1+t*t), sine=t*cosine;
        const double app=covariance[p][p], aqq=covariance[q][q], apq=covariance[p][q];
        covariance[p][p]=app-t*apq; covariance[q][q]=aqq+t*apq;
        covariance[p][q]=covariance[q][p]=0;
        for (size_t k=0;k<3;++k) {
            if (k!=p && k!=q) {
                const double akp=covariance[k][p], akq=covariance[k][q];
                covariance[k][p]=covariance[p][k]=cosine*akp-sine*akq;
                covariance[k][q]=covariance[q][k]=sine*akp+cosine*akq;
            }
            const double vkp=vectors[k][p], vkq=vectors[k][q];
            vectors[k][p]=cosine*vkp-sine*vkq;
            vectors[k][q]=sine*vkp+cosine*vkq;
        }
    }
    std::array<size_t,3> order{0,1,2};
    std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return covariance[a][a]>covariance[b][b];});
    for (size_t c=0;c<3;++c) {
        basis.eigenvalues[c]=covariance[order[c]][order[c]];
        for (size_t j=0;j<3;++j) basis.axes[c][j]=vectors[j][order[c]];
        const auto largest=std::max_element(basis.axes[c].begin(),basis.axes[c].end(),[](double a,double b){return std::abs(a)<std::abs(b);});
        if (*largest<0) for (double& value:basis.axes[c]) value=-value;
    }
    for (size_t i=0;i<3;++i) for (size_t j=0;j<3;++j) {
        const double dot=std::inner_product(basis.axes[i].begin(),basis.axes[i].end(),basis.axes[j].begin(),0.0);
        if (!std::isfinite(dot) || std::abs(dot-(i==j?1.0:0.0))>1e-8)
            throw std::runtime_error("Nonorthonormal histogram PCA basis");
    }
    return basis;
}
float Lookup(const std::array<F3,kLutWidth>& row, double coordinate, size_t channel)
{
    // Match normalized texture sampling: inverse-quantile values live at texel centers.
    const double index=std::clamp(coordinate*static_cast<double>(kLutWidth)-.5,0.0,static_cast<double>(kLutWidth-1));
    const size_t low=static_cast<size_t>(index), high=std::min(low+1,kLutWidth-1);
    return static_cast<float>(row[low][channel]+(row[high][channel]-row[low][channel])*(index-static_cast<double>(low)));
}
void PutGaussian(DirectX::Image const& image, const std::vector<F3>& values)
{
    for (size_t y=0;y<image.height;++y) for (size_t x=0;x<image.width;++x) {
        auto* dst=image.pixels+y*image.rowPitch+x*4;
        for (size_t c=0;c<3;++c) dst[c]=static_cast<uint8_t>(std::lround(std::clamp(values[y*image.width+x][c],0.f,1.f)*255.f));
        dst[3]=255;
    }
}
std::vector<F3> Downsample(const std::vector<F3>& pixels, size_t width)
{
    const size_t next=std::max(width/2,size_t{1});
    std::vector<F3> result(next*next);
    for (size_t y=0;y<next;++y) for (size_t x=0;x<next;++x)
        for (size_t c=0;c<3;++c)
            result[y*next+x][c]=(pixels[(y*2)*width+x*2][c]+pixels[(y*2)*width+x*2+1][c]+
                pixels[(y*2+1)*width+x*2][c]+pixels[(y*2+1)*width+x*2+1][c])*.25f;
    return result;
}
LayerData CookLayer(const std::filesystem::path& path, size_t layer, DirectX::ScratchImage& gaussian, DirectX::ScratchImage& histogram)
{
    auto pixels=LoadLinear(path);
    LayerData data; data.basis=PrincipalComponents(pixels);
    const size_t count=pixels.size();
    std::vector<F3> transformed(count);
    std::vector<std::pair<float,uint32_t>> sorted(count);
    std::array<size_t,3> clippedLow{},clippedHigh{};
    for (size_t c=0;c<3;++c) {
        for (size_t i=0;i<count;++i) {
            double projected=0;
            for (size_t j=0;j<3;++j) projected+=(pixels[i][j]-data.basis.origin[j])*data.basis.axes[c][j];
            sorted[i]={static_cast<float>(projected),static_cast<uint32_t>(i)};
        }
        std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.first<b.first || (a.first==b.first && a.second<b.second);});
        // Equal projected values share their middle rank; constant channels map to exactly .5.
        for (size_t begin=0;begin<count;) {
            size_t end=begin+1;
            while (end<count && sorted[end].first==sorted[begin].first) ++end;
            const float value=static_cast<float>(.5+kSigma*NormalQuantile((static_cast<double>(begin)+static_cast<double>(end))*.5/static_cast<double>(count)));
            for (size_t i=begin;i<end;++i) transformed[sorted[i].second][c]=value;
            if (value<0) clippedLow[c]+=end-begin;
            if (value>1) clippedHigh[c]+=end-begin;
            begin=end;
        }
        for (size_t x=0;x<kLutWidth;++x) {
            const double gaussianValue=(static_cast<double>(x)+.5)/static_cast<double>(kLutWidth);
            const double probability=.5*std::erfc(-(gaussianValue-.5)/(kSigma*std::sqrt(2.0)));
            const double index=std::clamp(probability*static_cast<double>(count)-.5,0.0,static_cast<double>(count-1));
            const size_t low=static_cast<size_t>(index),high=std::min(low+1,count-1);
            data.lut[0][x][c]=static_cast<float>(sorted[low].first+(sorted[high].first-sorted[low].first)*(index-static_cast<double>(low)));
        }
    }
    pixels.clear(); pixels.shrink_to_fit(); sorted.clear(); sorted.shrink_to_fit();
    V3 baseSecondMoment{};
    for (const auto& pixel:transformed) for (size_t c=0;c<3;++c) baseSecondMoment[c]+=static_cast<double>(pixel[c])*pixel[c]/static_cast<double>(count);
    std::array<double,512> quadrature{};
    for (size_t i=0;i<quadrature.size();++i) quadrature[i]=NormalQuantile((static_cast<double>(i)+.5)/static_cast<double>(quadrature.size()));
    Json variances=Json::array();
    size_t width=kSize;
    for (size_t mip=0;mip<kMips;++mip) {
        V3 secondMoment{},variance{};
        for (const auto& pixel:transformed) for (size_t c=0;c<3;++c) secondMoment[c]+=static_cast<double>(pixel[c])*pixel[c]/static_cast<double>(transformed.size());
        for (size_t c=0;c<3;++c) variance[c]=std::max(0.0,baseSecondMoment[c]-secondMoment[c]);
        variances.push_back(variance);
        if (mip>0) for (size_t x=0;x<kLutWidth;++x) for (size_t c=0;c<3;++c) {
            const double center=(static_cast<double>(x)+.5)/static_cast<double>(kLutWidth), sigma=std::sqrt(variance[c]);
            double sum=0;
            for (double sample:quadrature) sum+=Lookup(data.lut[0],center+sigma*sample,c);
            data.lut[mip][x][c]=static_cast<float>(sum/static_cast<double>(quadrature.size()));
        }
        PutGaussian(*gaussian.GetImage(mip,layer,0),transformed);
        if (mip+1<kMips) { transformed=Downsample(transformed,width); width/=2; }
    }
    const auto* lutImage=histogram.GetImage(0,layer,0);
    for (size_t row=0;row<16;++row) for (size_t x=0;x<kLutWidth;++x) {
        auto* dst=reinterpret_cast<float*>(lutImage->pixels+row*lutImage->rowPitch)+x*4;
        for (size_t c=0;c<3;++c) dst[c]=row<kMips?data.lut[row][x][c]:static_cast<float>(row==12?data.basis.origin[c]:data.basis.axes[row-13][c]);
        dst[3]=1;
        for (size_t c=0;c<4;++c) if (!std::isfinite(dst[c])) throw std::runtime_error("Nonfinite histogram LUT");
    }
    data.qa={{"source",path.filename().string()},{"source_linear_mean",data.basis.origin},{"pca_axes",data.basis.axes},
        {"pca_eigenvalues",data.basis.eigenvalues},{"gaussian_storage_clipped_below_zero",clippedLow},
        {"gaussian_storage_clipped_above_one",clippedHigh},{"mip_prefilter_variance",variances}};
    return data;
}
void MeasureDecoded(const DirectX::ScratchImage& compressed, size_t layer, LayerData& data)
{
    Json mips=Json::array();
    for (size_t mip=0;mip<kMips;++mip) {
        DirectX::ScratchImage decoded;
        Check(DirectX::Decompress(*compressed.GetImage(mip,layer,0),DXGI_FORMAT_R32G32B32A32_FLOAT,decoded),"Measure histogram BC7 decode");
        const auto* image=decoded.GetImage(0,0,0);
        const double count=static_cast<double>(image->width*image->height);
        V3 mean{}, minimum{1e30,1e30,1e30},maximum{-1e30,-1e30,-1e30};
        size_t outside=0;
        for (size_t y=0;y<image->height;++y) {
            const auto* row=reinterpret_cast<const float*>(image->pixels+y*image->rowPitch);
            for (size_t x=0;x<image->width;++x) {
                V3 rgb=data.basis.origin;
                for (size_t c=0;c<3;++c) {
                    const double component=Lookup(data.lut[mip],row[x*4+c],c);
                    for (size_t j=0;j<3;++j) rgb[j]+=component*data.basis.axes[c][j];
                }
                bool out=false;
                for (size_t c=0;c<3;++c) {
                    if (!std::isfinite(rgb[c])) throw std::runtime_error("Nonfinite reconstructed histogram color");
                    mean[c]+=rgb[c]/count;
                    minimum[c]=std::min(minimum[c],rgb[c]); maximum[c]=std::max(maximum[c],rgb[c]);
                    out=out || rgb[c]<0 || rgb[c]>1;
                }
                if (out) ++outside;
            }
        }
        V3 error{};
        for (size_t c=0;c<3;++c) error[c]=mean[c]-data.basis.origin[c];
        mips.push_back({{"mip",mip},{"decoded_linear_mean",mean},{"mean_error",error},{"decoded_min",minimum},{"decoded_max",maximum},{"out_of_gamut_pixel_count",outside},{"finite",true}});
    }
    data.qa["bc7_decoded_mips"]=std::move(mips);
}
} // namespace

void CookEnvironmentHistogram(const std::filesystem::path& source,
    const std::filesystem::path& output, ID3D11Device* device)
{
    std::ifstream input(source/"project_hs_environment_materials_v9_LEAN.json");
    if (!input) throw std::runtime_error("Missing histogram terrain manifest");
    Json manifest; input>>manifest;
    const auto& layers=manifest.at("terrain").at("layers");
    if (layers.size()!=kLayers) throw std::runtime_error("Expected four histogram terrain layers");
    std::array<std::filesystem::path,kLayers> paths;
    Json inputs=Json::array();
    for (size_t layer=0;layer<kLayers;++layer) {
        if (layers[layer].at("slice").get<size_t>()!=layer) throw std::runtime_error("Histogram terrain slices must be ordered");
        const std::filesystem::path relative(layers[layer].at("textures").at("base_color").get<std::string>());
        if (relative.is_absolute()) throw std::runtime_error("Absolute histogram source path");
        for (const auto& part:relative) if (part=="..") throw std::runtime_error("Histogram source escapes pack");
        paths[layer]=source/relative;
        inputs.push_back({{"path",relative.generic_string()},{"hash",HashFile(paths[layer])}});
    }
    std::filesystem::create_directories(output);
    const auto gaussianPath=output/"environment_terrain_gaussian.dds";
    const auto histogramPath=output/"environment_terrain_histogram.dds";
    const auto qaPath=output/"environment_terrain_histogram.qa.json";
    const auto cachePath=output/"environment_terrain_histogram.cache.json";
    const Json identity={{"recipe",kRecipe},{"encoder",device?"DirectXTex-D3D11-BC7":"DirectXTex-CPU-BC7"},{"inputs",inputs}};
    try {
        std::ifstream cached(cachePath); Json cache; cached>>cache;
        if (cache.at("identity")==identity && cache.at("gaussian_hash")==HashFile(gaussianPath) &&
            cache.at("histogram_hash")==HashFile(histogramPath) && cache.at("qa_hash")==HashFile(qaPath)) {
            std::cout<<"Environment histogram cache hit\n"; return;
        }
    } catch (const std::exception&) { /* Missing, stale or damaged cache is rebuilt. */ }
    DirectX::ScratchImage gaussian,histogram,compressed;
    Check(gaussian.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM,kSize,kSize,kLayers,kMips),"Allocate Gaussian terrain array");
    Check(histogram.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT,kLutWidth,16,kLayers,1),"Allocate histogram LUT array");
    std::array<LayerData,kLayers> data;
    for (size_t layer=0;layer<kLayers;++layer) {
        std::cout<<"Cook terrain histogram slice "<<layer<<"\n";
        data[layer]=CookLayer(paths[layer],layer,gaussian,histogram);
    }
    if (device)
        Check(DirectX::Compress(device,gaussian.GetImages(),gaussian.GetImageCount(),gaussian.GetMetadata(),DXGI_FORMAT_BC7_UNORM,DirectX::TEX_COMPRESS_DEFAULT,1.f,compressed),"GPU compress Gaussian terrain");
    else
        Check(DirectX::Compress(gaussian.GetImages(),gaussian.GetImageCount(),gaussian.GetMetadata(),DXGI_FORMAT_BC7_UNORM,DirectX::TEX_COMPRESS_PARALLEL,.5f,compressed),"CPU compress Gaussian terrain");
    gaussian.Release();
    Json qa={{"recipe",kRecipe},{"description","Per-channel PCA marginal histogram reconstruction; finite LUT, BC7 and Gaussian tails are approximate. Joint RGB distribution is not guaranteed."},{"layers",Json::array()}};
    for (size_t layer=0;layer<kLayers;++layer) { MeasureDecoded(compressed,layer,data[layer]); qa["layers"].push_back(data[layer].qa); }
    Check(DirectX::SaveToDDSFile(compressed.GetImages(),compressed.GetImageCount(),compressed.GetMetadata(),DirectX::DDS_FLAGS_FORCE_DX10_EXT,gaussianPath.c_str()),"Save Gaussian terrain");
    Check(DirectX::SaveToDDSFile(histogram.GetImages(),histogram.GetImageCount(),histogram.GetMetadata(),DirectX::DDS_FLAGS_FORCE_DX10_EXT,histogramPath.c_str()),"Save histogram terrain LUT");
    { std::ofstream stream(qaPath); stream<<qa.dump(2)<<'\n'; if (!stream) throw std::runtime_error("Failed writing histogram QA"); }
    const Json cache={{"identity",identity},{"gaussian_hash",HashFile(gaussianPath)},{"histogram_hash",HashFile(histogramPath)},{"qa_hash",HashFile(qaPath)}};
    { std::ofstream stream(cachePath); stream<<cache.dump(2)<<'\n'; if (!stream) throw std::runtime_error("Failed writing histogram cache"); }
}
} // namespace hs::content
