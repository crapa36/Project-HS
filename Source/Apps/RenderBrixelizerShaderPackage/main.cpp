#include "../AssetPipeline/action.hpp"
#include "../AssetPipeline/render_sdk_pin.hpp"

#include <hs/renderer/brixelizer_shaders.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/render_data/texture_package.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Implemented by the unmodified, pinned authoritative ContentSource copy.
float samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_8spp(int, int, int, int);

namespace
{
namespace ap = hs::asset_pipeline;
namespace fs = std::filesystem;
namespace rd = hs::render_data;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kShaderCount = 31 * 4 + 19 * 32;
constexpr std::string_view kProductPrefix = "render/native/brixelizer/";

void Require(bool condition, std::string_view reason)
{
    if (!condition) throw std::runtime_error("hs_render_brixelizer_shader: " + std::string(reason));
}

bool HexDigest(std::string_view value)
{
    return value.size() == 64 && std::ranges::all_of(value, [](char c)
        { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::uint32_t U32(std::span<const std::byte> bytes, std::size_t offset)
{
    Require(offset <= bytes.size() && bytes.size() - offset >= 4, "truncated DXIL container");
    std::uint32_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

void RequireRawDxil(std::span<const std::byte> bytes)
{
    Require(bytes.size() >= 32 && std::memcmp(bytes.data(), "DXBC", 4) == 0,
            "native table did not contain a raw DXIL container");
    Require(U32(bytes, 24) == bytes.size(), "native DXIL container length mismatch");
    const auto count = U32(bytes, 28);
    Require(count <= (bytes.size() - 32) / 4, "invalid DXIL part table");
    bool found = false;
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const auto offset = U32(bytes, 32 + static_cast<std::size_t>(index) * 4);
        Require(offset <= bytes.size() && bytes.size() - offset >= 8, "invalid DXIL part offset");
        const auto size = U32(bytes, offset + 4);
        Require(size <= bytes.size() - offset - 8, "invalid DXIL part size");
        found |= size != 0 && std::memcmp(bytes.data() + offset, "DXIL", 4) == 0;
    }
    Require(found, "native table contained no DXIL program part");
}

void RequireShaderId(std::string_view id)
{
    Require(id.starts_with(kProductPrefix) && id.ends_with(".dxil"), "unexpected native shader product ID");
    auto keys = id.substr(kProductPrefix.size(), id.size() - kProductPrefix.size() - 5);
    for (unsigned index = 0; index < 3; ++index)
    {
        const auto slash = keys.find('/');
        const auto key = keys.substr(0, slash);
        Require(!key.empty() && key.size() <= 10 && std::ranges::all_of(key,
            [](char c) { return c >= '0' && c <= '9'; }), "invalid native shader permutation key");
        Require(index == 2 ? slash == std::string_view::npos : slash != std::string_view::npos,
                "native shader product requires effect/pass/permutation keys");
        if (slash != std::string_view::npos) keys.remove_prefix(slash + 1);
    }
}

std::uint16_t NoiseHalf(float value)
{
    // All native sampler values are odd integers / 512, so binary16 stores
    // them exactly. Reject a changed sampler rather than round its result.
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto exponent = (bits >> 23) & 255u;
    Require(value > 0.0f && value < 1.0f && exponent > 112u && exponent < 143u &&
            (bits & 0x80001fffu) == 0, "native noise sample is not an exact positive binary16 value");
    return static_cast<std::uint16_t>(((exponent - 112u) << 10) | ((bits & 0x007fffffu) >> 13));
}

void ProduceNoise(const fs::path &stage, std::vector<ap::Product> &products, ap::Json &provenance)
{
    const std::array dependencies{std::string("render/native/brixelizer/LICENSE.txt")};
    provenance["noise"] = ap::Json::array();
    for (int sample = 0; sample < 16; ++sample)
    {
        Bytes payload(128 * 128 * 8);
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x)
            {
                const std::array texel{
                    NoiseHalf(samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_8spp(x, y, sample, 0)),
                    NoiseHalf(samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_8spp(x, y, sample, 1)),
                    std::uint16_t{0}, std::uint16_t{0x3c00}};
                const auto offset = static_cast<std::size_t>(y * 128 + x) * 8;
                for (std::size_t channel = 0; channel < texel.size(); ++channel)
                {
                    payload[offset + channel * 2] = static_cast<std::byte>(texel[channel] & 255u);
                    payload[offset + channel * 2 + 1] = static_cast<std::byte>(texel[channel] >> 8);
                }
            }
        const auto filename = "brixelizer_noise_" + std::to_string(sample) + ".rpkg";
        const auto id = "render/texture/" + filename;
        const auto package = rd::BuildTexturePackage(hs::MakeAssetId(id), dependencies,
            {128, 128, 1, 1, rd::kTextureFormatRgba16Float}, payload);
        ap::WriteBytes(stage / filename, package);
        auto product = ap::HashProduct(id, filename, stage / filename);
        product.dependencies.assign(dependencies.begin(), dependencies.end());
        provenance["noise"].push_back({{"logical_id", id}, {"digest", product.digest},
            {"size", product.size}, {"sample_index", sample}, {"sample_dimensions", {0, 1}},
            {"width", 128}, {"height", 128}, {"format", rd::kTextureFormatRgba16Float}});
        products.push_back(std::move(product));
    }
}

void Produce(const fs::path &source, const fs::path &build, const fs::path &sdk, const fs::path &pin)
{
    Require(source.is_absolute() && build.is_absolute() && sdk.is_absolute(), "all producer paths must be absolute");
    const auto pinned = ap::ReadRenderSdkPin(source, pin, "brixelizer");
    Require(pinned.at("release") == "1.1.4" &&
            pinned.at("commit") == "c6efa6bf7f2027b3ec94f28578bb5965eabb9e55" &&
            pinned.at("archive_sha256") == "b5bba71656e18f4c813fda4eb364dcb5e9b8491efd6abb66fdc8a2b530bfcedd" &&
            pinned.at("source_manifest_sha256") == "1f5bf0cb5a2541626c66546b0a22ac410c747fb6273c65bf3f8c6d54e9bb2761",
            "SDK lock differs from the verified native table source pin");
    const auto &configuration = pinned.at("native_shader_configuration");
    Require(configuration.at("raw_passes") == 31 && configuration.at("raw_permutation_keys") == 4 &&
            configuration.at("gi_passes") == 19 && configuration.at("gi_permutation_keys") == 32 &&
            configuration.at("compiler_threads") == 1 && configuration.at("compiler_job_pool") == 2,
            "SDK lock differs from the native shader table recipe");
    const auto cache = source / "Build/tooling/render-sdks/brixelizer-1.1.4";
    Require(fs::equivalent(sdk, cache / "FidelityFX-SDK-c6efa6bf7f2027b3ec94f28578bb5965eabb9e55"),
            "SDK path must identify the pinned bootstrap installation");

    ap::ActionSpec spec;
    spec.action_id = "render.shader.brixelizer";
    spec.recipe_id = "project-hs.render.brixelizer-native-dxil";
    spec.recipe_version = 2;
    spec.namespace_path = "render/shader/brixelizer";
    const auto addInput = [&](std::string id, const fs::path &path, std::string expected = {})
    {
        auto input = ap::HashInput(std::move(id), path);
        if (!expected.empty())
            Require(HexDigest(expected) && input.digest == "sha256:" + expected, "pinned SDK input digest mismatch");
        spec.inputs.push_back(std::move(input));
    };
    addInput("sdk/brixelizer/pin", pin);
    addInput("sdk/brixelizer/archive", cache / "sdk-source.zip", pinned.at("archive_sha256"));
    addInput("sdk/brixelizer/source-manifest", cache / "source-files.sha256", pinned.at("source_manifest_sha256"));
    const auto &noise = pinned.at("noise_source");
    Require(noise.at("path") == "ContentSource/Textures/BrixelizerNoise/samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_8spp.cpp" &&
            noise.at("sdk_path") == "sdk/src/components/sssr/samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_8spp.cpp" &&
            noise.at("sha256") == "cd464cc9c931b0b28fde5d42e83a31275ff9d5025482af73bb629f67ab373170" &&
            noise.at("license_path") == "ContentSource/Textures/BrixelizerNoise/FidelityFX-LICENSE.txt" &&
            noise.at("license_sha256") == "82cf74fc23885107c7f69249ac3f1dac55898ff68320eb2c753984cae6b2d78a",
            "authored noise source differs from its exact SDK copy");
    addInput("content/brixelizer/noise-source", source / noise.at("path").get<std::string>(), noise.at("sha256"));
    addInput("content/brixelizer/noise-license", source / noise.at("license_path").get<std::string>(), noise.at("license_sha256"));
    addInput("content/brixelizer/noise-provenance", source / "ContentSource/Textures/BrixelizerNoise/README.md");
    for (const auto *relative : {"Tools/bootstrap-brixelizer-sdk.ps1", "Tools/bootstrap_render_sdks.ps1",
                               "cmake/BrixelizerSdk.cmake", "cmake/BrixelizerNative/CMakeLists.txt",
                               "cmake/BrixelizerNative/NormalizeShaderDepfile.cmake",
                               "Source/RendererD3D12/Public/hs/renderer/brixelizer_shaders.hpp",
                               "Source/RendererD3D12/Private/renderer_brixelizer_shaders.cpp"})
        addInput(relative, source / relative);

    // The fixed manifest authenticates every SDK file, including the compiler
    // executable and its DXC/DXIL DLLs. An installed mutable receipt is not used.
    const auto manifest = ap::ReadBytes(cache / "source-files.sha256");
    std::set<std::string> sourceFiles;
    std::string_view remaining(manifest);
    while (!remaining.empty())
    {
        const auto end = remaining.find('\n');
        Require(end != std::string_view::npos, "unterminated pinned source manifest");
        const auto line = remaining.substr(0, end);
        remaining.remove_prefix(end + 1);
        Require(line.size() > 66 && HexDigest(line.substr(0, 64)) && line.substr(64, 2) == "  ",
                "invalid pinned source manifest entry");
        const std::string relative(line.substr(66));
        const fs::path path(relative);
        Require((relative.starts_with("sdk/") || relative == "LICENSE.txt") && !path.is_absolute() &&
                relative.find(':') == std::string::npos && relative.find('\\') == std::string::npos &&
                relative.find('\r') == std::string::npos && relative.find("..") == std::string::npos &&
                sourceFiles.insert(relative).second, "unsafe or duplicate SDK source manifest entry");
        addInput("sdk/brixelizer/" + relative, sdk / path, std::string(line.substr(0, 64)));
    }
    Require(sourceFiles.contains("LICENSE.txt") &&
            sourceFiles.contains("sdk/tools/binary_store/FidelityFX_SC.exe"), "incomplete pinned SDK manifest");
    const auto physicalInputs = spec.inputs;

    // Copy the linked, official CPU blob tables before creating any output.
    // Every permutation's actual digest participates in the Action recipe.
    std::map<std::string, Bytes> shaders;
    const auto enumerationSource = source / "Source/RendererD3D12/Private/renderer_brixelizer_shaders.cpp";
    const bool enumerated = hs::brixelizer_native::EnumerateShaders(
        [&](std::string_view id, std::span<const std::byte> bytes)
        {
            RequireShaderId(id);
            RequireRawDxil(bytes);
            Require(shaders.size() < kShaderCount, "native table exceeded the pinned permutation count");
            Require(shaders.emplace(std::string(id), Bytes(bytes.begin(), bytes.end())).second,
                    "native table returned a duplicate shader product");
            // Semantic input: its file dependency is the enumerator source,
            // while its digest identifies the actual linked immutable blob.
            spec.inputs.push_back({"native/blob/" + std::string(id), enumerationSource, ap::Sha256Bytes(bytes)});
            return true;
        });
    Require(enumerated && shaders.size() == kShaderCount, "native shader table enumeration was incomplete");
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);

    const auto stage = ap::CreateStage(build / "Actions" / spec.namespace_path);
    try
    {
        std::vector<ap::Product> products;
        products.reserve(shaders.size() + 18);
        ap::Json provenance{{"schema_version", 1}, {"kind", "native_pipeline_raw_dxil"},
            {"sdk", "FidelityFX Brixelizer"}, {"sdk_lock", pinned},
            {"shader_count", shaders.size()}, {"inputs", ap::Json::array()}, {"pipelines", ap::Json::array()}};
        for (const auto &input : spec.inputs)
            provenance["inputs"].push_back({{"logical_id", input.logical_id}, {"digest", input.digest}});
        for (const auto &[id, bytes] : shaders)
        {
            const auto digest = ap::Sha256Bytes(std::as_bytes(std::span(id)));
            Require(digest.starts_with("sha256:") && digest.size() == 71, "invalid product filename digest");
            const auto filename = digest.substr(7) + ".dxil";
            ap::WriteBytes(stage / filename, bytes);
            auto product = ap::HashProduct(id, filename, stage / filename);
            product.dependencies.push_back("render/native/brixelizer/LICENSE.txt");
            provenance["pipelines"].push_back({{"logical_id", id}, {"digest", product.digest}, {"size", product.size}});
            products.push_back(std::move(product));
        }
        const auto license = std::find_if(physicalInputs.begin(), physicalInputs.end(),
            [](const auto &input) { return input.logical_id == "sdk/brixelizer/LICENSE.txt"; });
        Require(license != physicalInputs.end(), "SDK license provenance absent");
        const auto copied = ap::CopyAndHashInput(license->logical_id, license->path, stage / "LICENSE.txt");
        Require(copied.digest == license->digest, "SDK license changed during publication");
        products.push_back(ap::HashProduct("render/native/brixelizer/LICENSE.txt", "LICENSE.txt", stage / "LICENSE.txt"));
        ProduceNoise(stage, products, provenance);
        const auto text = provenance.dump(2) + "\n";
        ap::WriteBytes(stage / "provenance.json", std::as_bytes(std::span(text)));
        products.push_back(ap::HashProduct("render/native/brixelizer/provenance.json", "provenance.json", stage / "provenance.json"));
        for (const auto &input : physicalInputs)
            Require(ap::Sha256File(input.path) == input.digest, "producer input changed before publication");
        ap::Publish(build, spec, stage, std::move(products));
        ap::RemoveStage(stage);
        std::cout << spec.action_id << " published native DXIL pipelines=" << shaders.size() << " noise_textures=16\n";
    }
    catch (...) { ap::RemoveStageAfterFailure(stage); throw; }
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    fs::path source, build, sdk, pin;
    try
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::wstring_view argument(argv[index]);
            const auto read = [&](std::wstring_view prefix, fs::path &path)
            {
                if (!argument.starts_with(prefix) || !path.empty()) return false;
                path = fs::path(argument.substr(prefix.size()));
                return !path.empty();
            };
            Require(read(L"--source=", source) || read(L"--build=", build) || read(L"--sdk=", sdk) || read(L"--pin=", pin),
                    "unknown or duplicate producer argument");
        }
        Produce(source.lexically_normal(), build.lexically_normal(), sdk.lexically_normal(), pin.lexically_normal());
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_render_brixelizer_shader_package", error.what(),
                          {{"action_id", "render.shader.brixelizer"},
                           {"source_root", ap::DiagnosticPath(source)}, {"build_root", ap::DiagnosticPath(build)},
                           {"dependency", {{"sdk", ap::DiagnosticPath(sdk)}, {"pin", ap::DiagnosticPath(pin)}}}});
        return 1;
    }
}
