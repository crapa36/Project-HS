#include "../AssetPipeline/action.hpp"
#include "../Content/bake_sampling.hpp"
#include <hs/core/cooked_format.hpp>
#include <hs/render_data/ltc_lut.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numbers>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace
{
namespace ap=hs::asset_pipeline;
namespace rd=hs::render_data;
namespace fs=std::filesystem;
constexpr std::string_view kProduct="render/lighting/default_ltc.rpkg";
constexpr std::string_view kNamespace="render/lighting/default_ltc";
constexpr double kPi=std::numbers::pi, kAlphaFloor=0.002;
struct Vec3 { double x{},y{},z{}; };
Vec3 operator+(Vec3 a,Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 operator*(Vec3 a,double b) { return {a.x*b,a.y*b,a.z*b}; }
double Dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 Cross(Vec3 a,Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Vec3 Normalize(Vec3 a) { return a*(1/std::sqrt(Dot(a,a))); }
void Require(bool condition,std::string_view message)
{
    if(!condition) throw std::runtime_error("LTC baker: "+std::string(message));
}
void Keys(const ap::Json &object,std::initializer_list<std::string_view> keys)
{
    Require(object.is_object() && object.size()==keys.size(),"invalid object keys");
    for(auto key:keys) Require(object.contains(std::string(key)),"missing required key");
}
rd::LtcLutDescription ParseSource(std::string_view text)
{
    std::vector<std::unordered_set<std::string>> objectKeys;
    const auto root=ap::Json::parse(text,[&](int,ap::Json::parse_event_t event,ap::Json &value) {
        if(event==ap::Json::parse_event_t::object_start) objectKeys.emplace_back();
        else if(event==ap::Json::parse_event_t::key)
            Require(!objectKeys.empty() && objectKeys.back().insert(value.get<std::string>()).second,"duplicate object key");
        else if(event==ap::Json::parse_event_t::object_end) objectKeys.pop_back();
        return true;
    });
    Keys(root,{"version","id","kind","brdf","parameterization","normalization","bake","author_notes"});
    Require(root.at("version").is_number_integer() && root.at("version")==1 && root.at("id")=="default_ltc" &&
        root.at("kind")=="ltc_ggx" && root.at("brdf")=="hs_isotropic_ggx_uncorrelated_smith_schlick" &&
        root.at("parameterization")=="perceptual_roughness_sqrt_one_minus_ndotv" &&
        root.at("normalization")=="cosine_over_pi_solid_angle_density","unsupported source conventions");
    const auto &notes=root.at("author_notes");
    Require(notes.is_string() && !notes.get_ref<const std::string &>().empty() &&
        notes.get_ref<const std::string &>().size()<=2048,"invalid author notes");
    const auto &bake=root.at("bake"); Keys(bake,{"edge","samples","iterations"});
    Require(bake.at("edge").is_number_unsigned() && bake.at("edge")==rd::kLtcLutEdge &&
        bake.at("samples").is_number_unsigned() && bake.at("iterations").is_number_unsigned(),"invalid integer fit budget");
    const auto samples=bake.at("samples").get<std::uint64_t>(),iterations=bake.at("iterations").get<std::uint64_t>();
    Require(samples>=256 && samples<=4096 && std::has_single_bit(samples) && iterations>=32 && iterations<=256,
        "fit budget outside bounded contract");
    return {std::uint32_t(samples),std::uint32_t(iterations)};
}
// Heitz, JCGT 2018 visible GGX normal sampler. This matches the project's
// HsBsdfSampleVisibleNormal, including null reflected directions below N.
struct VndfFrame { Vec3 stretched,t1,t2; double s{}; };
VndfFrame PrepareVndfFrame(Vec3 view,double alpha)
{
    const auto stretched=Normalize({alpha*view.x,alpha*view.y,view.z});
    const double lensq=stretched.x*stretched.x+stretched.y*stretched.y;
    const auto t1=lensq>0?Vec3{-stretched.y/std::sqrt(lensq),stretched.x/std::sqrt(lensq),0}:Vec3{1,0,0};
    return {stretched,t1,Cross(stretched,t1),0.5*(1+stretched.z)};
}
struct VndfSample { double u{},disk_x{},disk_y{}; };
std::vector<VndfSample> PrepareVndfSamples(std::uint32_t count)
{
    std::vector<VndfSample> result;
    result.reserve(count);
    for(const auto sample:hs::content::MakeHammersleySamples(count)) {
        const double radius=std::sqrt(sample.u),phi=2*kPi*sample.v;
        result.push_back({sample.u,radius*std::cos(phi),radius*std::sin(phi)});
    }
    return result;
}
struct SphereSample { Vec3 direction; };
std::vector<SphereSample> PrepareSphereSamples(std::uint32_t count)
{
    std::vector<SphereSample> result;
    result.reserve(count);
    for(const auto sample:hs::content::MakeHammersleySamples(count)) {
        const double z=1-2*sample.u,r=std::sqrt(std::max(0.0,1-z*z)),phi=2*kPi*sample.v;
        result.push_back({{r*std::cos(phi),r*std::sin(phi),z}});
    }
    return result;
}
Vec3 VisibleNormal(const VndfFrame& frame,const VndfSample& sample,double alpha)
{
    const double one=sample.disk_x;
    const double two=(1-frame.s)*std::sqrt(std::max(0.0,1-one*one))+frame.s*sample.disk_y;
    const auto n=frame.t1*one+frame.t2*two+frame.stretched*std::sqrt(std::max(0.0,1-one*one-two*two));
    return Normalize({alpha*n.x,alpha*n.y,std::max(0.0,n.z)});
}
double SmithDenominator(double cosine,double alpha)
{
    return cosine+std::sqrt(alpha*alpha+(1-alpha*alpha)*cosine*cosine);
}
double Distribution(double noH,double alpha)
{
    const double d=noH*noH*(alpha*alpha-1)+1;
    return alpha*alpha/(kPi*d*d);
}
double ReflectionPdf(Vec3 view,Vec3 light,double alpha)
{
    const auto sum=view+light; const double square=Dot(sum,sum);
    if(square<=0) return 0;
    const auto h=sum*(1/std::sqrt(square));
    if(h.z<=0 || Dot(view,h)<=0) return 0;
    return Distribution(h.z,alpha)/(2*SmithDenominator(view.z,alpha));
}
double UnitFresnelCosine(Vec3 view,Vec3 light,double alpha)
{
    if(light.z<=0) return 0;
    const auto sum=view+light; const double square=Dot(sum,sum);
    if(square<=0) return 0;
    const auto h=sum*(1/std::sqrt(square));
    return Distribution(std::clamp(h.z,0.0,1.0),alpha)*light.z/
        (SmithDenominator(view.z,alpha)*SmithDenominator(light.z,alpha));
}
using Parameters=std::array<double,4>;
struct Matrix { double a{},b{},c{},d{}; };
Matrix Decode(Parameters parameters)
{
    // Positive determinant by construction. m11=1 fixes the immaterial common
    // scale; remaining four coefficients cover scale, stretch and view skew.
    const double a=std::exp(parameters[0]),scale=std::exp(parameters[1]);
    return {a,parameters[2],parameters[3],scale+parameters[2]*parameters[3]/a};
}
bool Bounded(Parameters parameters)
{
    return std::abs(parameters[0])<=12 && std::abs(parameters[1])<=12 &&
        std::abs(parameters[2])<=1e5 && std::abs(parameters[3])<=1e5;
}
double Density(Matrix matrix,Vec3 direction)
{
    const Vec3 q{matrix.a*direction.x+matrix.b*direction.z,direction.y,
        matrix.c*direction.x+matrix.d*direction.z};
    const double square=Dot(q,q);
    return q.z>0 && square>0 ? q.z*(matrix.a*matrix.d-matrix.b*matrix.c)/(kPi*square*square):0;
}
struct FitPoint { Vec3 direction; double target{},weight{}; };
struct FitContext
{
    std::vector<FitPoint> points;
    double energy{},schlick{};
};
FitContext Quadrature(Vec3 view,double alpha,const std::vector<VndfSample>& full,
                      const std::vector<VndfSample>& half,const std::vector<SphereSample>& sphere)
{
    FitContext result;
    const auto count=static_cast<std::uint32_t>(full.size());
    const auto frame=PrepareVndfFrame(view,alpha);
    for(std::uint32_t i=0;i<count;++i)
    {
        const auto h=VisibleNormal(frame,full[i],alpha);
        const auto light=h*(2*Dot(view,h))+view*(-1);
        if(light.z<=0) continue; // Null samples retain the original denominator.
        const double weight=2*light.z/SmithDenominator(light.z,alpha);
        result.energy+=weight/count;
        result.schlick+=weight*std::pow(1-std::clamp(Dot(view,h),0.0,1.0),5)/count;
    }
    Require(result.energy>0 && std::isfinite(result.energy) && std::isfinite(result.schlick),"invalid GGX energy");
    // Half uniform-sphere, half GGX VNDF samples form one deterministic MIS
    // integral. Sphere samples measure LTC leakage below the receiver horizon.
    result.points.reserve(count);
    for(std::uint32_t i=0;i<count;++i)
    {
        const auto index=i%static_cast<std::uint32_t>(half.size());
        Vec3 light;
        if(i<half.size())
        {
            light=sphere[index].direction;
        }
        else
        {
            const auto h=VisibleNormal(frame,half[index],alpha); light=h*(2*Dot(view,h))+view*(-1);
        }
        const double proposal=0.5/(4*kPi)+0.5*ReflectionPdf(view,light,alpha);
        result.points.push_back({light,UnitFresnelCosine(view,light,alpha)/result.energy,1/(count*proposal)});
    }
    return result;
}
Parameters Fit(const FitContext &context,Parameters initial,std::uint32_t iterations)
{
    struct Vertex { Parameters p; double error{}; };
    auto objective=[&](Parameters p) {
        if(!Bounded(p)) return std::numeric_limits<double>::infinity();
        const auto matrix=Decode(p); double error=0;
        for(const auto &point:context.points) error+=std::abs(Density(matrix,point.direction)-point.target)*point.weight;
        return std::isfinite(error)?error:std::numeric_limits<double>::infinity();
    };
    auto evaluate=[&](Parameters p) { return Vertex{p,objective(p)}; };
    std::array<Vertex,5> simplex; simplex[0]=evaluate(initial);
    for(std::size_t i=0;i<4;++i)
    {
        auto p=initial; p[i]+=i<2?0.2:std::max(0.1,std::abs(initial[i])*0.15); simplex[i+1]=evaluate(p);
    }
    // Fixed-count Nelder-Mead, no random seeds, retries or unbounded convergence.
    for(std::uint32_t iteration=0;iteration<iterations;++iteration)
    {
        std::ranges::stable_sort(simplex,{},&Vertex::error);
        Parameters center{};
        for(std::size_t i=0;i<4;++i) for(std::size_t j=0;j<4;++j) center[j]+=simplex[i].p[j]*0.25;
        auto point=[&](double factor) {
            Parameters p; for(std::size_t j=0;j<4;++j) p[j]=center[j]+factor*(center[j]-simplex[4].p[j]);
            return evaluate(p);
        };
        const auto reflected=point(1);
        if(reflected.error<simplex[0].error)
        {
            const auto expanded=point(2); simplex[4]=expanded.error<reflected.error?expanded:reflected;
        }
        else if(reflected.error<simplex[3].error) simplex[4]=reflected;
        else
        {
            const bool outside=reflected.error<simplex[4].error;
            const auto contracted=point(outside?0.5:-0.5);
            if(contracted.error<(outside?reflected.error:simplex[4].error)) simplex[4]=contracted;
            else for(std::size_t i=1;i<5;++i)
            {
                auto p=simplex[i].p;
                for(std::size_t j=0;j<4;++j) p[j]=(p[j]+simplex[0].p[j])*0.5;
                simplex[i]=evaluate(p);
            }
        }
    }
    std::ranges::stable_sort(simplex,{},&Vertex::error);
    Require(std::isfinite(simplex[0].error),"fit did not produce a finite transform"); return simplex[0].p;
}
std::vector<std::byte> Bake(rd::LtcLutDescription source)
{
    const auto edge=rd::kLtcLutEdge; std::vector<hs::Float4> matrices(edge*edge),amplitudes(edge*edge);
    const auto full_samples=PrepareVndfSamples(source.fit_samples);
    const auto half_samples=PrepareVndfSamples(source.fit_samples/2);
    const auto sphere_samples=PrepareSphereSamples(source.fit_samples/2);
    // Heitz et al., SIGGRAPH 2016: normalized LTC fit then inverse-transform
    // emitter geometry. This independently implemented fit uses the project's
    // uncorrelated Smith model, not downloaded reference fit coefficients.
    for(std::uint32_t y=0;y<edge;++y)
    {
        const double coordinate=double(y)/(edge-1),noV=std::max(double(rd::kLtcNdotVEndpoint),1-coordinate*coordinate);
        const Vec3 view{std::sqrt(std::max(0.0,1-noV*noV)),0,noV}; Parameters previous{};
        for(std::uint32_t reverse=0;reverse<edge;++reverse)
        {
            const auto x=edge-1-reverse; const double roughness=double(x)/(edge-1),alpha=std::max(kAlphaFloor,roughness*roughness);
            const auto quadrature=Quadrature(view,alpha,full_samples,half_samples,sphere_samples);
            // Warm start from the neighboring larger roughness. At normal
            // incidence the symmetry is also represented by this general fit.
            previous=Fit(quadrature,previous,source.fit_iterations); const auto matrix=Decode(previous);
            const auto offset=std::size_t(y)*edge+x;
            matrices[offset]={float(matrix.a),float(matrix.b),float(matrix.c),float(matrix.d)};
            const double energy=std::min(quadrature.energy,1.0),schlick=std::min(quadrature.schlick,energy);
            amplitudes[offset]={float(energy),float(schlick),0,0};
        }
    }
    return rd::BuildLtcLutPackage(hs::MakeAssetId(kProduct),{},source,matrices,amplitudes);
}
void Produce(const fs::path &sourceRoot,const fs::path &buildRoot)
{
    Require(sourceRoot.is_absolute() && buildRoot.is_absolute(),"roots must be absolute");
    ap::RequireDirectory(sourceRoot); ap::MakeDirectory(buildRoot);
    ap::ActionSpec spec; spec.action_id="render.lighting.default_ltc";
    spec.recipe_id="project-hs.render.ltc-lut"; spec.recipe_version=1; spec.namespace_path=kNamespace;
    const auto sourcePath=sourceRoot/"ContentSource/Lighting/default_ltc.json";
    const auto sourceBytes=ap::ReadBytes(sourcePath);
    spec.inputs.push_back({"ContentSource/Lighting/default_ltc.json",sourcePath,
        ap::Sha256Bytes(std::as_bytes(std::span(sourceBytes.data(),sourceBytes.size())))});
    for(const auto *relative:{"Schemas/ltc_lut.schema.json","Content/Shaders/bsdf_metallic_roughness.hlsli"})
        spec.inputs.push_back(ap::HashInput(relative,sourceRoot/relative));
    std::ranges::sort(spec.inputs,{},&ap::Input::logical_id); const auto source=ParseSource(sourceBytes);
    const auto resultPath=buildRoot/"Actions"/kNamespace/"result.json";
    if(fs::exists(resultPath) && fs::exists(resultPath.parent_path()/"result.d"))
    {
        try
        {
            const auto current=ap::Json::parse(ap::ReadBytes(resultPath));
            if(current.at("action_id")==spec.action_id && current.at("action_key")==ap::ActionKey(spec) && current.at("products").size()==1)
            {
                ap::UpstreamProduct ignored; fs::path path;
                (void)ap::ReadActionResult(buildRoot,kNamespace,kProduct,ignored,path); const auto existing=ap::ReadBytes(path);
                (void)rd::ReadLtcLutPackage(std::as_bytes(std::span(existing.data(),existing.size())),hs::MakeAssetId(kProduct));
                ap::TouchActionResult(resultPath); return;
            }
        }
        catch(const std::exception &) { /* Invalid current product is rebuilt by its normal action. */ }
    }
    const auto stage=ap::CreateStage(buildRoot/"Actions"/kNamespace);
    try
    {
        const auto bytes=Bake(source); (void)rd::ReadLtcLutPackage(bytes,hs::MakeAssetId(kProduct));
        const auto path=stage/"default_ltc.rpkg"; ap::WriteBytes(path,bytes);
        ap::Publish(buildRoot,spec,stage,{ap::HashProduct(std::string(kProduct),"default_ltc.rpkg",path)});
    }
    catch(...) { ap::RemoveStageAfterFailure(stage); throw; }
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
            else throw std::runtime_error("usage: hs_ltc_lut --produce --source-root <absolute> --build-root <absolute>");
        }
        Require(produce && !sourceRoot.empty() && !buildRoot.empty(),
            "usage: hs_ltc_lut --produce --source-root <absolute> --build-root <absolute>");
        Produce(sourceRoot.lexically_normal(),buildRoot.lexically_normal()); return 0;
    }
    catch(const std::exception &error)
    {
        ap::ReportFailure("hs_ltc_lut", error.what(),
                          {{"action_id", "render.lighting.default_ltc"}, {"produce", produce},
                           {"source_root", ap::DiagnosticPath(sourceRoot)}, {"build_root", ap::DiagnosticPath(buildRoot)}});
        return 1;
    }
}
