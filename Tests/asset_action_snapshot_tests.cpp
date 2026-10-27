#include "../Source/Apps/AssetPipeline/action.hpp"
#include "../Source/Apps/AssetPipeline/shader_source_snapshot.hpp"

#include <stdexcept>
#include <sstream>
#include <array>

void TestAssetActionSnapshot()
{
    namespace ap = hs::asset_pipeline;
    namespace fs = std::filesystem;
    const auto root = ap::CreateStage(fs::temp_directory_path() / "hs-action-snapshot-tests");
    try
    {
        ap::ActionSpec spec;
        spec.action_id = "snapshot.fixture";
        spec.recipe_id = "snapshot.fixture";
        spec.recipe_version = 1;
        spec.namespace_path = "snapshot/fixture";
        const auto input_path = root / "fixture.input";
        constexpr std::string_view input_bytes = "receipt input";
        const auto bytes = [](std::string_view value) { return std::as_bytes(std::span(value.data(), value.size())); };
        ap::WriteBytes(input_path, bytes(input_bytes));
        spec.inputs.push_back(ap::HashInput("fixture/input", input_path));
        const auto stage = ap::CreateStage(root / "Actions" / spec.namespace_path);
        constexpr std::string_view original = "verified bytes";
        ap::WriteBytes(stage / "fixture.bin", bytes(original));
        ap::Publish(root, spec, stage,
                    {ap::HashProduct("fixture/payload", "fixture.bin", stage / "fixture.bin")});
        const ap::ActionResultSnapshot snapshot(root, spec.namespace_path);
        const auto &input_metadata = snapshot.InputMetadata();
        if (!input_metadata.is_array() || input_metadata.size() != 1 ||
            input_metadata.at(0).at("logical_id") != "fixture/input" ||
            input_metadata.at(0).at("digest") != spec.inputs.front().digest)
            throw std::runtime_error("snapshot input receipt metadata differs");
        ap::WriteBytes(input_path, bytes("changed input"));
        if (snapshot.InputMetadata().at(0).at("digest") != spec.inputs.front().digest)
            throw std::runtime_error("snapshot input receipt changed with source file");
        if (!snapshot.HasProduct("fixture/payload") || snapshot.HasProduct("fixture/other"))
            throw std::runtime_error("snapshot product membership differs");
        ap::UpstreamProduct dependency;
        const auto owned = snapshot.ReadProductBytes("fixture/payload", dependency);
        std::ostringstream copied;
        snapshot.CopyProductBytesTo("fixture/payload", copied, dependency);
        if(copied.str()!=original || snapshot.ProductIds()!=std::vector<std::string_view>{"fixture/payload"})
            throw std::runtime_error("streamed product or snapshot IDs differ");
        if (owned != original || dependency.size != original.size() ||
            dependency.digest != ap::Sha256Bytes(bytes(original)))
            throw std::runtime_error("verified snapshot bytes or identity differ");
        fs::path path;
        snapshot.ReadProduct("fixture/payload", dependency, path);
        std::array<char, 3> scratch{};
        std::ostringstream chunked, input_copy;
        snapshot.CopyProductBytesTo("fixture/payload", chunked, dependency, scratch);
        ap::CopyFileBytesTo(path, input_copy, dependency.size, dependency.digest, scratch);
        if (chunked.str() != original || input_copy.str() != original)
            throw std::runtime_error("reused partial-chunk scratch changed streamed bytes");
        ap::WriteBytes(path, bytes("corrupt! bytes")); // Equal size must not count as a hit.
        if (owned != original) throw std::runtime_error("snapshot owner changed with file");
        const auto rejects = [](auto operation) {
            try { operation(); } catch (const std::exception &) { return; }
            throw std::runtime_error("snapshot accepted corrupt or unbound product");
        };
        rejects([&] { (void)snapshot.ReadProductBytes("fixture/payload", dependency); });
        rejects([&] { std::ostringstream out; snapshot.CopyProductBytesTo("fixture/payload",out,dependency); });
        rejects([&] { std::ostringstream out; ap::CopyFileBytesTo(path,out,original.size(),ap::Sha256Bytes(bytes(original)),scratch); });
        rejects([&] { std::ostringstream out; snapshot.CopyProductBytesTo("fixture/other",out,dependency); });
        rejects([&] { snapshot.ReadProduct("fixture/payload", dependency, path); });
        rejects([&] { (void)snapshot.ReadProductBytes("fixture/other", dependency); });
        ap::WriteBytes(path, bytes(original));
        rejects([&] { std::ostringstream out; ap::CopyFileBytesTo(path,out,original.size()+1,ap::Sha256Bytes(bytes(original)),scratch); });
        rejects([&] { std::ostringstream out; ap::CopyFileBytesTo(path,out,original.size(),ap::Sha256Bytes(bytes(original)),{}); });
        rejects([&] { std::ostringstream out; out.setstate(std::ios::badbit); snapshot.CopyProductBytesTo("fixture/payload",out,dependency); });
        if (snapshot.ReadProductBytes("fixture/payload", dependency) != original)
            throw std::runtime_error("repaired product did not reopen");

        const auto source = root / "sources" / "Content/Shaders";
        ap::MakeDirectory(source);
        ap::WriteBytes(source / "fixture.hlsl", bytes("#include \"fixture.hlsli\"\n"));
        ap::WriteBytes(source / "fixture.hlsli", bytes("original include"));
        const std::vector shader_inputs{
            ap::HashInput("Content/Shaders/fixture.hlsl", source / "fixture.hlsl"),
            ap::HashInput("Content/Shaders/fixture.hlsli", source / "fixture.hlsli")};
        const auto shader_stage = ap::CreateStage(root / "shader-actions");
        const auto frozen = hs::shader_content::CreateShaderSourceSnapshot(shader_inputs, shader_stage);
        ap::WriteBytes(source / "fixture.hlsl", bytes("changed source"));
        ap::WriteBytes(source / "fixture.hlsli", bytes("changed include"));
        if (ap::ReadBytes(frozen / "Content/Shaders/fixture.hlsl") != "#include \"fixture.hlsli\"\n" ||
            ap::ReadBytes(frozen / "Content/Shaders/fixture.hlsli") != "original include")
            throw std::runtime_error("source edit changed the compiler snapshot");
        const auto changed_stage = ap::CreateStage(root / "shader-actions");
        rejects([&] { (void)hs::shader_content::CreateShaderSourceSnapshot(shader_inputs, changed_stage); });
    }
    catch (...)
    {
        ap::RemoveStage(root);
        throw;
    }
    ap::RemoveStage(root);
}
