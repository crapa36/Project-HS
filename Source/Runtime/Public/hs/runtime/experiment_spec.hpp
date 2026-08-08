#pragma once

#include <hs/core/result.hpp>
#include <hs/core/types.hpp>
#include <hs/runtime/application.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace hs
{

enum class ExperimentMode : std::uint8_t
{
    SimulationOnly,
    OffscreenRender,
    PresentPerformance,
};

struct ExperimentTimelineAction
{
    Sequence sequence{};
    Tick target_tick{};
    std::string command;
    std::string payload_json;
};

struct ExperimentCapture
{
    Tick tick{};
    std::string path;
};

using ExperimentValue = std::variant<bool, std::int64_t, std::uint64_t, double, std::string>;

struct ExperimentAssertion
{
    std::string id;
    std::string probe;
    std::string comparison;
    ExperimentValue expected;
    double tolerance{};
};

struct ExperimentTermination
{
    Tick maximum_tick{};
    bool on_victory{};
    bool on_defeat{};
    bool on_error{true};
};

struct ExperimentSpec
{
    static constexpr std::uint32_t kSchemaVersion = 1;

    std::uint32_t schema_version{kSchemaVersion};
    std::string scenario_id;
    std::uint64_t seed{};
    ExperimentMode mode{ExperimentMode::OffscreenRender};
    std::string build_hash;
    std::string content_hash;
    std::string config_overrides_json{"{}"};
    std::vector<ExperimentTimelineAction> timeline_actions;
    std::vector<std::string> probes;
    std::vector<ExperimentCapture> captures;
    std::vector<ExperimentAssertion> assertions;
    ExperimentTermination termination;
};

[[nodiscard]] std::string_view ToString(ExperimentMode mode) noexcept;
[[nodiscard]] Result LoadExperimentSpec(const std::filesystem::path &path,
                                        ExperimentSpec &spec);
[[nodiscard]] Result SaveExperimentSpec(const std::filesystem::path &path,
                                        const ExperimentSpec &spec);
[[nodiscard]] Result ApplyExperimentOverrides(const ExperimentSpec &spec,
                                              ApplicationConfig &config);
[[nodiscard]] Result FinalizeExperimentArtifacts(const ExperimentSpec &spec,
                                                 const std::filesystem::path &directory,
                                                 std::string_view start_utc,
                                                 std::string_view end_utc,
                                                 int process_exit);

} // namespace hs
