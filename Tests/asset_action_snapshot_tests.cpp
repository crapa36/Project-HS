#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "../Source/Apps/AssetPipeline/action.hpp"
#ifdef _WIN32
#include "../Source/Apps/AssetPipeline/dxc_process.hpp"
#endif
#include "../Source/Apps/AssetPipeline/shader_source_snapshot.hpp"
#include "../Source/Apps/Content/game_data_numbers.hpp"

#include <stdexcept>
#include <sstream>
#include <array>
#include <cmath>
#include <limits>
#include <exception>
#include <utility>
#ifdef _WIN32
#include <Windows.h>
#endif

void TestAssetActionSnapshot()
{
    namespace ap = hs::asset_pipeline;
    namespace fs = std::filesystem;
    const auto root = ap::CreateStage(fs::temp_directory_path() / "hs-action-snapshot-tests");
    try
    {
        const auto cleanup_stage = ap::CreateStage(root / "cleanup-success");
        constexpr std::string_view partial_bytes = "partial";
        ap::WriteBytes(cleanup_stage / "partial.bin",
            std::as_bytes(std::span(partial_bytes.data(), partial_bytes.size())));
        ap::RemoveStageAfterFailure(cleanup_stage);
        if (fs::exists(cleanup_stage))
            throw std::runtime_error("best-effort failure cleanup did not remove an unlocked stage");

#ifdef _WIN32
        const auto locked_stage = ap::CreateStage(root / "cleanup-locked");
        const auto locked_file = locked_stage / "locked.bin";
        constexpr std::string_view locked_bytes = "locked";
        ap::WriteBytes(locked_file,
            std::as_bytes(std::span(locked_bytes.data(), locked_bytes.size())));
        const HANDLE lock = CreateFileW(locked_file.c_str(), GENERIC_READ, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock == INVALID_HANDLE_VALUE)
            throw std::runtime_error("could not open cleanup fixture without delete sharing");
        struct HandleGuard
        {
            HANDLE value{};
            ~HandleGuard() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
            void Close() noexcept
            {
                if (value != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(value, INVALID_HANDLE_VALUE));
            }
        } lock_guard{lock};
        std::string propagated_error;
        try
        {
            try
            {
                throw std::runtime_error("primary producer failure");
            }
            catch (...)
            {
                ap::RemoveStageAfterFailure(locked_stage);
                throw;
            }
        }
        catch (const std::exception &error) { propagated_error = error.what(); }
        if (propagated_error != "primary producer failure" || !fs::exists(locked_stage))
            throw std::runtime_error("locked-stage cleanup replaced the primary error or removed the live stage");
        const auto unreadable_diagnostics = hs::shader_content::ReadDxcDiagnostics(locked_stage);
        if (!unreadable_diagnostics.starts_with("diagnostic output unavailable:"))
            throw std::runtime_error("DXC diagnostic read failure was not converted to a secondary message");
        lock_guard.Close();
        ap::RemoveStage(locked_stage);
#endif

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

void TestGameDataFloatConversion()
{
    using hs::content::detail::CheckedFloat;
    using hs::content::detail::CheckedPositiveFloat;
    using hs::content::detail::CheckedSpawnRate;
    using hs::content::detail::CheckedPositiveTickInterval;
    using hs::content::detail::MakesFiniteProjectileRangeProgress;
    const auto rejects = [](auto &&convert) {
        try
        {
            static_cast<void>(convert());
        }
        catch (const std::exception &)
        {
            return;
        }
        throw std::runtime_error("game-data float conversion accepted an invalid value");
    };

    const auto maximum = static_cast<double>(std::numeric_limits<float>::max());
    if (CheckedFloat(0.0) != 0.0f || CheckedFloat(1e-50) != 0.0f ||
        CheckedFloat(1.25) != 1.25f || CheckedFloat(maximum) != std::numeric_limits<float>::max() ||
        CheckedFloat(-maximum) != std::numeric_limits<float>::lowest() ||
        CheckedPositiveFloat(1.25) != 1.25f)
        throw std::runtime_error("game-data float conversion changed valid values or allowed zero");
    if (CheckedFloat(maximum * 2.0 * 0.5) != std::numeric_limits<float>::max())
        throw std::runtime_error("arena half-width conversion changed a representable extent");
    rejects([] { return CheckedFloat(std::numeric_limits<double>::infinity()); });
    rejects([] { return CheckedFloat(std::numeric_limits<double>::quiet_NaN()); });
    rejects([&] { return CheckedFloat(std::nextafter(maximum, std::numeric_limits<double>::infinity())); });
    rejects([&] { return CheckedFloat(std::nextafter(-maximum, -std::numeric_limits<double>::infinity())); });
    rejects([&] { return CheckedFloat(maximum * 4.0 * 0.5); });
    rejects([] { return CheckedPositiveFloat(0.0); });
    rejects([] { return CheckedPositiveFloat(1e-50); });
    if (!MakesFiniteProjectileRangeProgress(32.0f, 18.0f))
        throw std::runtime_error("normal projectile speed/range stopped making 60 Hz progress");
    if (MakesFiniteProjectileRangeProgress(1e-20f, 18.0f) ||
        MakesFiniteProjectileRangeProgress(1e-22f, 18.0f) ||
        MakesFiniteProjectileRangeProgress(1e30f, 18.0f) ||
        MakesFiniteProjectileRangeProgress(32.0f, 1e20f))
        throw std::runtime_error("non-progressing projectile speed/range was accepted");
    if (!MakesFiniteProjectileRangeProgress(1e30f, 0.0f) ||
        !MakesFiniteProjectileRangeProgress(1e30f, -1.0f))
        throw std::runtime_error("nonpositive projectile leg no longer keeps immediate-retirement semantics");
    if (CheckedPositiveTickInterval(1) != 1 ||
        CheckedPositiveTickInterval(std::uint64_t{1} << 33) != (std::uint64_t{1} << 33))
        throw std::runtime_error("positive recurring interval changed a valid 60 Hz tick");
    const auto positive_subtick_rounded = static_cast<std::uint64_t>(std::llround(1e-4 * 60.0));
    if (positive_subtick_rounded != 0)
        throw std::runtime_error("sub-tick interval fixture no longer rounds to zero");
    rejects([&] { return CheckedPositiveTickInterval(positive_subtick_rounded); });
    rejects([] { return CheckedPositiveTickInterval(0); });
    if (CheckedSpawnRate(4.8) != static_cast<float>(4.8) || CheckedSpawnRate(120) != 120.0f)
        throw std::runtime_error("spawn rate conversion changed normal or multi-spawn ticks");
    rejects([] { return CheckedSpawnRate(1e20); });
    rejects([] { return CheckedSpawnRate(1e-50); });
    rejects([] { return CheckedSpawnRate(0); });
}
