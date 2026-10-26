#include "../Source/Apps/AssetPipeline/action.hpp"

#include <stdexcept>
#include <sstream>

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
        const auto stage = ap::CreateStage(root / "Actions" / spec.namespace_path);
        constexpr std::string_view original = "verified bytes";
        const auto bytes = [](std::string_view value) { return std::as_bytes(std::span(value.data(), value.size())); };
        ap::WriteBytes(stage / "fixture.bin", bytes(original));
        ap::Publish(root, spec, stage,
                    {ap::HashProduct("fixture/payload", "fixture.bin", stage / "fixture.bin")});
        const ap::ActionResultSnapshot snapshot(root, spec.namespace_path);
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
        ap::WriteBytes(path, bytes("corrupt! bytes")); // Equal size must not count as a hit.
        if (owned != original) throw std::runtime_error("snapshot owner changed with file");
        const auto rejects = [](auto operation) {
            try { operation(); } catch (const std::exception &) { return; }
            throw std::runtime_error("snapshot accepted corrupt or unbound product");
        };
        rejects([&] { (void)snapshot.ReadProductBytes("fixture/payload", dependency); });
        rejects([&] { std::ostringstream out; snapshot.CopyProductBytesTo("fixture/payload",out,dependency); });
        rejects([&] { std::ostringstream out; snapshot.CopyProductBytesTo("fixture/other",out,dependency); });
        rejects([&] { snapshot.ReadProduct("fixture/payload", dependency, path); });
        rejects([&] { (void)snapshot.ReadProductBytes("fixture/other", dependency); });
        ap::WriteBytes(path, bytes(original));
        rejects([&] { std::ostringstream out; out.setstate(std::ios::badbit); snapshot.CopyProductBytesTo("fixture/payload",out,dependency); });
        if (snapshot.ReadProductBytes("fixture/payload", dependency) != original)
            throw std::runtime_error("repaired product did not reopen");
    }
    catch (...)
    {
        ap::RemoveStage(root);
        throw;
    }
    ap::RemoveStage(root);
}
