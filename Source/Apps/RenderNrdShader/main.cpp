#include "../AssetPipeline/action.hpp"
#include "../AssetPipeline/render_sdk_pin.hpp"

#include <NRD.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace ap = hs::asset_pipeline;
namespace fs = std::filesystem;
using Bytes = std::vector<std::byte>;

void Require(bool condition, std::string_view reason)
{
    if (!condition) throw std::runtime_error("hs_render_nrd_shader: " + std::string(reason));
}

// These CPU-only SDK variants are independent of renderer implementation types.
// SIGMA is packaged for a future native shadow contract; packaging selects no
// runtime feature and allocates no D3D12 resources.
struct Variant { nrd::Denoiser native; const char *name; };
constexpr std::array kVariants{
    Variant{nrd::Denoiser::REBLUR_DIFFUSE, "REBLUR_DIFFUSE"},
    Variant{nrd::Denoiser::REBLUR_SPECULAR, "REBLUR_SPECULAR"},
    Variant{nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR, "REBLUR_DIFFUSE_SPECULAR"},
    Variant{nrd::Denoiser::RELAX_DIFFUSE, "RELAX_DIFFUSE"},
    Variant{nrd::Denoiser::RELAX_SPECULAR, "RELAX_SPECULAR"},
    Variant{nrd::Denoiser::RELAX_DIFFUSE_SPECULAR, "RELAX_DIFFUSE_SPECULAR"},
    Variant{nrd::Denoiser::REBLUR_DIFFUSE_OCCLUSION, "REBLUR_DIFFUSE_OCCLUSION"},
    Variant{nrd::Denoiser::SIGMA_SHADOW, "SIGMA_SHADOW"}};

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
            "SDK pipeline did not contain a raw DXIL container");
    Require(U32(bytes, 24) == bytes.size(), "SDK DXIL container length mismatch");
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
    Require(found, "SDK field contained no DXIL program part");
}

struct NativeLibrary
{
    HMODULE handle{};
    decltype(&nrd::CreateInstance) create{};
    decltype(&nrd::DestroyInstance) destroy{};
    decltype(&nrd::GetLibraryDesc) library{};
    decltype(&nrd::GetInstanceDesc) instance{};
    explicit NativeLibrary(const fs::path &path)
    {
        handle = LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Require(handle != nullptr, "cannot load the verified NRD DLL snapshot");
        create = reinterpret_cast<decltype(create)>(GetProcAddress(handle, "CreateInstance"));
        destroy = reinterpret_cast<decltype(destroy)>(GetProcAddress(handle, "DestroyInstance"));
        library = reinterpret_cast<decltype(library)>(GetProcAddress(handle, "GetLibraryDesc"));
        instance = reinterpret_cast<decltype(instance)>(GetProcAddress(handle, "GetInstanceDesc"));
        if (!create || !destroy || !library || !instance)
        {
            FreeLibrary(handle);
            handle = nullptr;
            Require(false, "pinned NRD exports absent");
        }
    }
    ~NativeLibrary() { if (handle) FreeLibrary(handle); }
    NativeLibrary(const NativeLibrary &) = delete;
    NativeLibrary &operator=(const NativeLibrary &) = delete;
};

struct NativeInstance
{
    NativeLibrary &api;
    nrd::Instance *value{};
    NativeInstance(NativeLibrary &library, nrd::Denoiser denoiser) : api(library)
    {
        const nrd::DenoiserDesc selected{1, denoiser};
        nrd::InstanceCreationDesc description{};
        description.denoisers = &selected;
        description.denoisersNum = 1;
        const auto result = api.create(description, value);
        if (result != nrd::Result::SUCCESS || !value)
        {
            if (value) api.destroy(*value);
            value = nullptr;
            Require(false, "native NRD selected instance creation failed");
        }
    }
    ~NativeInstance() { if (value) api.destroy(*value); }
};

std::string ShortDigest(const std::string &digest)
{
    Require(digest.starts_with("sha256:") && digest.size() == 71, "unexpected SHA-256 encoding");
    return digest.substr(7);
}

void Produce(const fs::path &source, const fs::path &build, const fs::path &sdk, const fs::path &dxc, const fs::path &pin)
{
    Require(source.is_absolute() && build.is_absolute() && sdk.is_absolute() && dxc.is_absolute(),
            "all producer paths must be absolute");
    Require(fs::equivalent(sdk, source / "Build/tooling/render-sdks/nrd-4.17.3"),
            "SDK path must identify the pinned bootstrap installation");
    const auto pinned = ap::ReadRenderSdkPin(source, pin, "nrd");
    Require(pinned.at("commit") == "792eff196afdd350fd9c3f862119017ccb438a0e" &&
            pinned.at("release") == "4.17.3", "unexpected NRD source pin");
    const auto receiptPath = sdk / "native-build-receipt.json";
    const auto receipt = ap::Json::parse(ap::ReadBytes(receiptPath));
    Require(receipt.at("schema_version") == 1 && receipt.at("kind") == "derived_native_build" &&
            receipt.at("recipe").at("commit") == pinned.at("commit") &&
            receipt.at("recipe").at("archive_sha256") == pinned.at("archive_sha256") &&
            receipt.at("recipe").at("configuration") == pinned.at("native_configuration"),
            "native build provenance does not match the pinned source/configuration");

    ap::ActionSpec spec;
    spec.action_id = "render.shader.nrd";
    spec.recipe_id = "project-hs.render.nrd-native-dxil";
    spec.recipe_version = 1;
    spec.namespace_path = "render/shader/nrd";
    const auto addInput = [&](std::string id, const fs::path &path, std::string expected = {})
    {
        auto input = ap::HashInput(std::move(id), path);
        if (!expected.empty()) Require(input.digest == "sha256:" + expected, "NRD provenance input digest mismatch");
        spec.inputs.push_back(std::move(input));
    };
    addInput("sdk/nrd/pin", pin);
    addInput("sdk/nrd/native-build-receipt", receiptPath);
    addInput("sdk/nrd/source", sdk.parent_path() / "nrd-4.17.3-source.zip", pinned.at("archive_sha256"));
    Require(receipt.at("recipe").at("dependencies").size() == pinned.at("dependencies").size(),
            "native source dependencies differ");
    for (std::size_t index = 0; index < pinned.at("dependencies").size(); ++index)
    {
        const auto &dependency = pinned.at("dependencies").at(index);
        const auto &built = receipt.at("recipe").at("dependencies").at(index);
        Require(built.at("name") == dependency.at("name") && built.at("commit") == dependency.at("commit") &&
                built.at("archive_sha256") == dependency.at("archive_sha256"), "native dependency pin mismatch");
        const auto name = dependency.at("name").get<std::string>();
        addInput("sdk/nrd/source/" + name, sdk.parent_path() /
            (name + "-" + dependency.at("commit").get<std::string>() + ".zip"), dependency.at("archive_sha256"));
    }
    addInput("sdk/nrd/build-wrapper", source / "cmake/NRDPrerequisite/CMakeLists.txt", receipt.at("recipe").at("wrapper_sha256"));
    addInput("sdk/nrd/build-bootstrap", source / "tools/bootstrap_nrd.ps1", receipt.at("recipe").at("bootstrap_sha256"));
    for (const auto &artifact : receipt.at("artifacts"))
    {
        const auto relative = artifact.at("path").get<std::string>();
        Require(!fs::path(relative).is_absolute() && relative.find("..") == std::string::npos && relative.find(':') == std::string::npos,
                "unsafe native build artifact path");
        addInput("sdk/nrd/" + relative, sdk / relative, artifact.at("sha256"));
    }
    for (const auto &relative : {"Include/NRD.h", "Include/NRDDescs.h", "Include/NRDSettings.h", "Shaders/NRD.hlsli"})
        addInput("sdk/nrd/" + std::string(relative), sdk / relative);
    Require(!receipt.at("tools").empty(), "native compiler provenance absent");
    std::size_t toolIndex = 0;
    bool builtWithDxc = false;
    for (const auto &tool : receipt.at("tools"))
    {
        const fs::path path(tool.at("path").get<std::string>());
        addInput("sdk/nrd/compiler/" + std::to_string(toolIndex++), path, tool.at("sha256"));
        builtWithDxc |= fs::equivalent(path, dxc);
    }
    Require(builtWithDxc, "producer DXC differs from the compiler used for the native package");
    const auto stage = ap::CreateStage(build / "Actions" / spec.namespace_path);
    try
    {
        const auto snapshot = stage / "native-snapshot";
        ap::MakeDirectory(snapshot);
        const auto dllInput = std::find_if(spec.inputs.begin(), spec.inputs.end(),
            [](const auto &input) { return input.logical_id == "sdk/nrd/bin/NRD.dll"; });
        Require(dllInput != spec.inputs.end(), "native DLL provenance absent");
        const auto copied = ap::CopyAndHashInput(dllInput->logical_id, dllInput->path, snapshot / "NRD.dll");
        Require(copied.digest == dllInput->digest, "native DLL changed while snapshotting");
        std::map<std::string, Bytes> shaders;
        ap::Json variants = ap::Json::array();
        {
            NativeLibrary api(snapshot / "NRD.dll");
            const auto *library = api.library();
            Require(library && library->versionMajor == NRD_VERSION_MAJOR && library->versionMinor == NRD_VERSION_MINOR &&
                    library->versionBuild == NRD_VERSION_BUILD && static_cast<unsigned>(library->normalEncoding) == 2 &&
                    static_cast<unsigned>(library->roughnessEncoding) == 1,
                    "native SDK version or packing configuration differs");
            Require(library->supportedDenoisers && library->supportedDenoisersNum, "native supported denoisers absent");
            for (const auto &variant : kVariants)
            {
                Require(std::find(library->supportedDenoisers, library->supportedDenoisers + library->supportedDenoisersNum,
                                  variant.native) != library->supportedDenoisers + library->supportedDenoisersNum,
                        "native SDK does not support a requested producer variant");
                NativeInstance instance(api, variant.native);
                const auto *description = api.instance(*instance.value);
                Require(description && description->pipelines && description->pipelinesNum,
                        "native instance returned no pipelines");
                ap::Json identifiers = ap::Json::array();
                for (std::uint32_t index = 0; index < description->pipelinesNum; ++index)
                {
                    const auto &pipeline = description->pipelines[index];
                    const auto end = std::find(std::begin(pipeline.shaderIdentifier), std::end(pipeline.shaderIdentifier), '\0');
                    Require(end != std::end(pipeline.shaderIdentifier) && end != std::begin(pipeline.shaderIdentifier),
                            "native pipeline identifier invalid");
                    const std::string identifier(std::begin(pipeline.shaderIdentifier), end);
                    const auto &native = pipeline.computeShaderDXIL;
                    Require(native.bytecode && native.size && native.size <= std::numeric_limits<std::size_t>::max(),
                            "native pipeline embeds no DXIL");
                    const auto bytes = std::span(static_cast<const std::byte *>(native.bytecode), static_cast<std::size_t>(native.size));
                    RequireRawDxil(bytes);
                    const auto [found, inserted] = shaders.try_emplace(identifier, bytes.begin(), bytes.end());
                    Require(inserted || std::equal(bytes.begin(), bytes.end(), found->second.begin(), found->second.end()),
                            "same native shader identifier produced different bytecode");
                    identifiers.push_back("nrd/" + identifier);
                }
                variants.push_back({{"denoiser", variant.name}, {"native_enum", static_cast<unsigned>(variant.native)},
                                    {"pipeline_products", std::move(identifiers)}});
            }
        } // All native metadata is copied before instances/DLL are destroyed.
        std::vector<ap::Product> products;
        ap::Json provenance{{"schema_version", 1}, {"kind", "native_pipeline_raw_dxil"},
            {"sdk", "NRD"}, {"sdk_version", "4.17.3"}, {"source_commit", pinned.at("commit")},
            {"configuration", pinned.at("native_configuration")}, {"variants", std::move(variants)},
            {"native_build", receipt}, {"inputs", ap::Json::array()}, {"pipelines", ap::Json::array()}};
        for (const auto &input : spec.inputs)
            provenance["inputs"].push_back({{"logical_id", input.logical_id}, {"digest", input.digest}});
        for (const auto &[identifier, bytes] : shaders)
        {
            const auto filename = ShortDigest(ap::Sha256Bytes(std::as_bytes(std::span(identifier)))) + ".dxil";
            ap::WriteBytes(stage / filename, bytes);
            auto product = ap::HashProduct("nrd/" + identifier, filename, stage / filename);
            provenance["pipelines"].push_back({{"logical_id", product.logical_id}, {"digest", product.digest}, {"size", product.size}});
            products.push_back(std::move(product));
        }
        for (const auto &[relative, filename] : std::array{
            std::pair{"generated/NRDConfig.hlsli", "NRDConfig.hlsli"}, std::pair{"Shaders/NRD.hlsli", "NRD.hlsli"}})
        {
            const auto input = std::find_if(spec.inputs.begin(), spec.inputs.end(), [&](const auto &value)
                { return value.logical_id == "sdk/nrd/" + std::string(relative); });
            Require(input != spec.inputs.end(), "packing helper provenance absent");
            const auto copy = ap::CopyAndHashInput(input->logical_id, input->path, stage / filename);
            Require(copy.digest == input->digest, "packing helper changed while snapshotting");
            products.push_back(ap::HashProduct("nrd/" + std::string(filename), filename, stage / filename));
        }
        const auto provenanceText = provenance.dump(2) + "\n";
        ap::WriteBytes(stage / "provenance.json", std::as_bytes(std::span(provenanceText)));
        products.push_back(ap::HashProduct("nrd/provenance.json", "provenance.json", stage / "provenance.json"));
        for (const auto &input : spec.inputs)
            Require(ap::Sha256File(input.path) == input.digest, "producer input changed before publication");
        ap::RemoveStage(snapshot);
        ap::Publish(build, spec, stage, std::move(products));
        ap::RemoveStage(stage);
        std::cout << spec.action_id << " published native DXIL pipelines=" << shaders.size() << '\n';
    }
    catch (...) { ap::RemoveStage(stage); throw; }
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    fs::path source, build, sdk, dxc, pin;
    try
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::wstring_view argument(argv[index]);
            const auto read = [&](std::wstring_view prefix, fs::path &path)
            {
                if (!argument.starts_with(prefix)) return false;
                path = fs::path(argument.substr(prefix.size()));
                return true;
            };
            Require(read(L"--source=", source) || read(L"--build=", build) || read(L"--sdk=", sdk) || read(L"--dxc=", dxc) || read(L"--pin=", pin),
                    "unknown producer argument");
        }
        Produce(source, build, sdk, dxc, pin);
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_render_nrd_shader", error.what(),
                          {{"action_id", "render.shader.nrd"},
                           {"source_root", ap::DiagnosticPath(source)}, {"build_root", ap::DiagnosticPath(build)},
                           {"dependency", {{"sdk", ap::DiagnosticPath(sdk)}, {"dxc", ap::DiagnosticPath(dxc)},
                                           {"pin", ap::DiagnosticPath(pin)}}}});
        return 1;
    }
}
