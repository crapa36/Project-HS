#include <hs/runtime/experiment_spec.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

namespace hs
{
namespace
{

using Json = nlohmann::ordered_json;

[[noreturn]] void Invalid(std::string_view path, std::string_view message)
{
    throw std::runtime_error(std::format("{}: {}", path, message));
}

template <typename Allowed, typename Required>
void CheckKeys(const Json &value, std::string_view path, const Allowed &allowed,
               const Required &required)
{
    if (!value.is_object())
    {
        Invalid(path, "must be an object");
    }
    for (const auto &[key, unused] : value.items())
    {
        (void)unused;
        if (std::ranges::find(allowed, key) == allowed.end())
        {
            Invalid(path, std::format("unknown field '{}'", key));
        }
    }
    for (const auto key : required)
    {
        if (!value.contains(key))
        {
            Invalid(path, std::format("missing field '{}'", key));
        }
    }
}

[[nodiscard]] std::string String(const Json &value, std::string_view path)
{
    if (!value.is_string())
    {
        Invalid(path, "must be a string");
    }
    return value.get<std::string>();
}

[[nodiscard]] std::uint64_t Unsigned(const Json &value, std::string_view path)
{
    if (!value.is_number_unsigned())
    {
        Invalid(path, "must be an unsigned integer");
    }
    return value.get<std::uint64_t>();
}

[[nodiscard]] bool Boolean(const Json &value, std::string_view path)
{
    if (!value.is_boolean())
    {
        Invalid(path, "must be a boolean");
    }
    return value.get<bool>();
}

[[nodiscard]] bool IsHex64(std::string_view value) noexcept
{
    return value.size() == 16 &&
           std::ranges::all_of(value, [](unsigned char character) {
               return std::isxdigit(character) != 0;
           });
}

[[nodiscard]] bool IsSafeKey(std::string_view value) noexcept
{
    return !value.empty() && value.size() <= 64 &&
           std::ranges::all_of(value, [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' || character == '_' ||
                      character == '.';
           });
}

[[nodiscard]] ExperimentMode ParseMode(std::string_view value)
{
    if (value == "simulation_only")
    {
        return ExperimentMode::SimulationOnly;
    }
    if (value == "offscreen_render")
    {
        return ExperimentMode::OffscreenRender;
    }
    if (value == "present_performance")
    {
        return ExperimentMode::PresentPerformance;
    }
    Invalid("mode", "unsupported value");
}

void ValidateConfigOverrides(const Json &value)
{
    constexpr std::array allowed{
        "warp",          "validation",          "gpu_validation", "vsync",
        "bloom",         "outline",             "resize_test",    "borderless",
        "width",         "height",              "frame_cap",      "render_scale_percent",
        "shadow_resolution", "particle_percentage", "barrier_mode"};
    CheckKeys(value, "config_overrides", allowed, std::array<std::string_view, 0>{});

    constexpr std::array boolean_fields{
        "warp", "validation", "gpu_validation", "vsync", "bloom", "outline",
        "resize_test", "borderless"};
    for (const auto field : boolean_fields)
    {
        if (value.contains(field))
        {
            (void)Boolean(value.at(field), std::format("config_overrides.{}", field));
        }
    }
    const auto ranged = [&](std::string_view field, std::uint64_t minimum,
                            std::uint64_t maximum) {
        if (!value.contains(field))
        {
            return;
        }
        const auto parsed = Unsigned(value.at(field), std::format("config_overrides.{}", field));
        if (parsed < minimum || parsed > maximum)
        {
            Invalid(std::format("config_overrides.{}", field), "value is out of range");
        }
    };
    ranged("width", 640, 7680);
    ranged("height", 360, 4320);

    if (value.contains("frame_cap"))
    {
        const auto parsed = Unsigned(value.at("frame_cap"), "config_overrides.frame_cap");
        if (parsed != 0 && parsed != 30 && parsed != 60 && parsed != 120)
        {
            Invalid("config_overrides.frame_cap", "must be 0, 30, 60, or 120");
        }
    }
    const auto one_of = [&](std::string_view field,
                            std::initializer_list<std::uint64_t> values) {
        if (!value.contains(field))
        {
            return;
        }
        const auto parsed = Unsigned(value.at(field), std::format("config_overrides.{}", field));
        if (std::ranges::find(values, parsed) == values.end())
        {
            Invalid(std::format("config_overrides.{}", field), "unsupported value");
        }
    };
    one_of("render_scale_percent", {75, 100});
    one_of("shadow_resolution", {1024, 2048});
    one_of("particle_percentage", {50, 100});

    if (value.contains("barrier_mode"))
    {
        const auto mode = String(value.at("barrier_mode"), "config_overrides.barrier_mode");
        if (mode != "automatic" && mode != "enhanced" && mode != "legacy")
        {
            Invalid("config_overrides.barrier_mode", "unsupported value");
        }
    }
}

[[nodiscard]] ExperimentValue ParseValue(const Json &value, std::string_view path)
{
    if (value.is_boolean())
    {
        return value.get<bool>();
    }
    if (value.is_number_unsigned())
    {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer())
    {
        return value.get<std::int64_t>();
    }
    if (value.is_number_float())
    {
        const auto parsed = value.get<double>();
        if (!std::isfinite(parsed))
        {
            Invalid(path, "must be finite");
        }
        return parsed;
    }
    if (value.is_string())
    {
        return value.get<std::string>();
    }
    Invalid(path, "must be a scalar");
}

[[nodiscard]] Json ToJson(const ExperimentValue &value)
{
    return std::visit([](const auto &item) { return Json(item); }, value);
}

[[nodiscard]] std::uint64_t FileHash(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return 0;
    }
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;
    std::array<char, 16 * 1024> bytes{};
    while (stream)
    {
        stream.read(bytes.data(), bytes.size());
        const auto count = stream.gcount();
        for (std::streamsize index = 0; index < count; ++index)
        {
            hash ^= static_cast<unsigned char>(bytes[static_cast<std::size_t>(index)]);
            hash *= prime;
        }
    }
    return hash;
}

[[nodiscard]] bool Compare(const Json &actual, const ExperimentAssertion &assertion)
{
    const auto expected = ToJson(assertion.expected);
    const auto comparison = assertion.comparison;
    if (actual.is_number() && expected.is_number())
    {
        if (assertion.tolerance == 0.0 && actual.is_number_integer() &&
            expected.is_number_integer() && (comparison == "eq" || comparison == "ne"))
        {
            return comparison == "eq" ? actual == expected : actual != expected;
        }
        const auto left = actual.get<double>();
        const auto right = expected.get<double>();
        const auto difference = std::abs(left - right);
        if (comparison == "eq")
        {
            return difference <= assertion.tolerance;
        }
        if (comparison == "ne")
        {
            return difference > assertion.tolerance;
        }
        if (comparison == "lt")
        {
            return left < right + assertion.tolerance;
        }
        if (comparison == "le")
        {
            return left <= right + assertion.tolerance;
        }
        if (comparison == "gt")
        {
            return left > right - assertion.tolerance;
        }
        return left >= right - assertion.tolerance;
    }
    if ((actual.is_boolean() && expected.is_boolean()) ||
        (actual.is_string() && expected.is_string()))
    {
        return comparison == "eq" ? actual == expected : actual != expected;
    }
    return false;
}

[[nodiscard]] Result Failure(std::string message)
{
    return Result::Failure(ErrorCode::InvalidArgument, "hs_experiment", std::move(message));
}

} // namespace

std::string_view ToString(ExperimentMode mode) noexcept
{
    switch (mode)
    {
    case ExperimentMode::SimulationOnly:
        return "simulation_only";
    case ExperimentMode::OffscreenRender:
        return "offscreen_render";
    case ExperimentMode::PresentPerformance:
        return "present_performance";
    }
    return "offscreen_render";
}

Result LoadExperimentSpec(const std::filesystem::path &path, ExperimentSpec &spec)
{
    try
    {
        std::ifstream stream(path);
        if (!stream)
        {
            return Failure(std::format("Cannot open spec: {}", path.string()));
        }
        Json root;
        stream >> root;
        constexpr std::array fields{
            "schema_version", "scenario_id",      "seed",       "mode",
            "build_hash",     "content_hash",     "config_overrides",
            "timeline_actions", "probes",         "captures",   "assertions",
            "termination"};
        CheckKeys(root, "$", fields, fields);

        ExperimentSpec parsed;
        const auto schema = Unsigned(root.at("schema_version"), "schema_version");
        if (schema != ExperimentSpec::kSchemaVersion)
        {
            Invalid("schema_version", "must be 1");
        }
        parsed.schema_version = static_cast<std::uint32_t>(schema);
        parsed.scenario_id = String(root.at("scenario_id"), "scenario_id");
        if (!IsSafeKey(parsed.scenario_id))
        {
            Invalid("scenario_id", "must be 1-64 ASCII key characters");
        }
        parsed.seed = Unsigned(root.at("seed"), "seed");
        parsed.mode = ParseMode(String(root.at("mode"), "mode"));
        parsed.build_hash = String(root.at("build_hash"), "build_hash");
        parsed.content_hash = String(root.at("content_hash"), "content_hash");
        if (!IsHex64(parsed.build_hash) || !IsHex64(parsed.content_hash))
        {
            Invalid("build_hash/content_hash", "must be exactly 16 hexadecimal characters");
        }

        const auto &overrides = root.at("config_overrides");
        ValidateConfigOverrides(overrides);
        parsed.config_overrides_json = overrides.dump();

        const auto &termination = root.at("termination");
        constexpr std::array termination_fields{
            "maximum_tick", "on_victory", "on_defeat", "on_error"};
        CheckKeys(termination, "termination", termination_fields, termination_fields);
        parsed.termination.maximum_tick =
            Unsigned(termination.at("maximum_tick"), "termination.maximum_tick");
        if (parsed.termination.maximum_tick == 0)
        {
            Invalid("termination.maximum_tick", "must be greater than zero");
        }
        parsed.termination.on_victory =
            Boolean(termination.at("on_victory"), "termination.on_victory");
        parsed.termination.on_defeat =
            Boolean(termination.at("on_defeat"), "termination.on_defeat");
        parsed.termination.on_error =
            Boolean(termination.at("on_error"), "termination.on_error");

        const auto &actions = root.at("timeline_actions");
        if (!actions.is_array())
        {
            Invalid("timeline_actions", "must be an array");
        }
        std::unordered_set<Sequence> action_sequences;
        Tick previous_tick{};
        constexpr std::array commands{
            "start_session", "grant_skill", "grant_upgrade", "grant_relic",
            "set_growth_tick", "damage_player", "heal_player", "spawn_enemy",
            "spawn_boss"};
        for (std::size_t index = 0; index < actions.size(); ++index)
        {
            const auto path_prefix = std::format("timeline_actions[{}]", index);
            const auto &action = actions[index];
            constexpr std::array action_fields{"sequence", "target_tick", "command", "payload"};
            CheckKeys(action, path_prefix, action_fields, action_fields);
            ExperimentTimelineAction item;
            item.sequence = Unsigned(action.at("sequence"), path_prefix + ".sequence");
            item.target_tick = Unsigned(action.at("target_tick"), path_prefix + ".target_tick");
            item.command = String(action.at("command"), path_prefix + ".command");
            if (item.sequence == 0 || item.target_tick == 0)
            {
                Invalid(path_prefix, "sequence and target_tick must be greater than zero");
            }
            if (!action_sequences.insert(item.sequence).second)
            {
                Invalid(path_prefix + ".sequence", "duplicate sequence");
            }
            if (index != 0 && item.target_tick < previous_tick)
            {
                Invalid(path_prefix + ".target_tick", "target ticks must not decrease");
            }
            if (item.target_tick > parsed.termination.maximum_tick)
            {
                Invalid(path_prefix + ".target_tick", "exceeds termination maximum_tick");
            }
            if (std::ranges::find(commands, item.command) == commands.end())
            {
                Invalid(path_prefix + ".command", "unsupported command");
            }
            if (!action.at("payload").is_object())
            {
                Invalid(path_prefix + ".payload", "must be an object");
            }
            item.payload_json = action.at("payload").dump();
            previous_tick = item.target_tick;
            parsed.timeline_actions.push_back(std::move(item));
        }

        const auto &probes = root.at("probes");
        if (!probes.is_array())
        {
            Invalid("probes", "must be an array");
        }
        std::unordered_set<std::string> probe_names;
        constexpr std::array supported_probes{
            "cpu.thread", "cpu.phase", "gpu.pass", "memory", "ecs.count",
            "queue.depth", "gameplay.checksum"};
        for (std::size_t index = 0; index < probes.size(); ++index)
        {
            auto probe = String(probes[index], std::format("probes[{}]", index));
            if (std::ranges::find(supported_probes, probe) == supported_probes.end())
            {
                Invalid(std::format("probes[{}]", index), "unsupported probe");
            }
            if (!probe_names.insert(probe).second)
            {
                Invalid(std::format("probes[{}]", index), "duplicate probe");
            }
            parsed.probes.push_back(std::move(probe));
        }

        const auto &captures = root.at("captures");
        if (!captures.is_array())
        {
            Invalid("captures", "must be an array");
        }
        std::unordered_set<std::string> capture_paths;
        for (std::size_t index = 0; index < captures.size(); ++index)
        {
            const auto path_prefix = std::format("captures[{}]", index);
            const auto &capture = captures[index];
            constexpr std::array capture_fields{"tick", "path"};
            CheckKeys(capture, path_prefix, capture_fields, capture_fields);
            ExperimentCapture item;
            item.tick = Unsigned(capture.at("tick"), path_prefix + ".tick");
            item.path = String(capture.at("path"), path_prefix + ".path");
            const std::filesystem::path relative(item.path);
            if (item.tick > parsed.termination.maximum_tick || relative.empty() ||
                relative.is_absolute() || relative.has_parent_path() ||
                relative.extension() != ".png")
            {
                Invalid(path_prefix, "capture must be a basename PNG within maximum_tick");
            }
            if (!capture_paths.insert(item.path).second)
            {
                Invalid(path_prefix + ".path", "duplicate capture path");
            }
            parsed.captures.push_back(std::move(item));
        }

        const auto &assertions = root.at("assertions");
        if (!assertions.is_array())
        {
            Invalid("assertions", "must be an array");
        }
        std::unordered_set<std::string> assertion_ids;
        for (std::size_t index = 0; index < assertions.size(); ++index)
        {
            const auto path_prefix = std::format("assertions[{}]", index);
            const auto &assertion = assertions[index];
            constexpr std::array assertion_fields{
                "id", "probe", "comparison", "expected", "tolerance"};
            CheckKeys(assertion, path_prefix, assertion_fields, assertion_fields);
            ExperimentAssertion item;
            item.id = String(assertion.at("id"), path_prefix + ".id");
            item.probe = String(assertion.at("probe"), path_prefix + ".probe");
            item.comparison = String(assertion.at("comparison"), path_prefix + ".comparison");
            item.expected = ParseValue(assertion.at("expected"), path_prefix + ".expected");
            if (!assertion.at("tolerance").is_number())
            {
                Invalid(path_prefix + ".tolerance", "must be numeric");
            }
            item.tolerance = assertion.at("tolerance").get<double>();
            if (!IsSafeKey(item.id) || item.probe.empty() || item.probe.front() != '/' ||
                !std::isfinite(item.tolerance) || item.tolerance < 0.0)
            {
                Invalid(path_prefix, "invalid id, JSON pointer probe, or tolerance");
            }
            try
            {
                (void)Json::json_pointer(item.probe);
            }
            catch (const std::exception &)
            {
                Invalid(path_prefix + ".probe", "invalid JSON pointer");
            }
            constexpr std::array comparisons{"eq", "ne", "lt", "le", "gt", "ge"};
            if (std::ranges::find(comparisons, item.comparison) == comparisons.end())
            {
                Invalid(path_prefix + ".comparison", "unsupported comparison");
            }
            if (!std::holds_alternative<std::int64_t>(item.expected) &&
                !std::holds_alternative<std::uint64_t>(item.expected) &&
                !std::holds_alternative<double>(item.expected) && item.tolerance != 0.0)
            {
                Invalid(path_prefix + ".tolerance", "non-numeric assertions require zero");
            }
            if ((!std::holds_alternative<std::int64_t>(item.expected) &&
                 !std::holds_alternative<std::uint64_t>(item.expected) &&
                 !std::holds_alternative<double>(item.expected)) &&
                item.comparison != "eq" && item.comparison != "ne")
            {
                Invalid(path_prefix + ".comparison", "non-numeric values support eq/ne only");
            }
            if (!assertion_ids.insert(item.id).second)
            {
                Invalid(path_prefix + ".id", "duplicate assertion id");
            }
            parsed.assertions.push_back(std::move(item));
        }

        spec = std::move(parsed);
        return Result::Success();
    }
    catch (const std::exception &exception)
    {
        return Failure(exception.what());
    }
}

Result SaveExperimentSpec(const std::filesystem::path &path, const ExperimentSpec &spec)
{
    try
    {
        Json root;
        root["schema_version"] = spec.schema_version;
        root["scenario_id"] = spec.scenario_id;
        root["seed"] = spec.seed;
        root["mode"] = ToString(spec.mode);
        root["build_hash"] = spec.build_hash;
        root["content_hash"] = spec.content_hash;
        root["config_overrides"] = Json::parse(spec.config_overrides_json);
        root["timeline_actions"] = Json::array();
        for (const auto &action : spec.timeline_actions)
        {
            root["timeline_actions"].push_back(
                {{"sequence", action.sequence},
                 {"target_tick", action.target_tick},
                 {"command", action.command},
                 {"payload", Json::parse(action.payload_json)}});
        }
        root["probes"] = spec.probes;
        root["captures"] = Json::array();
        for (const auto &capture : spec.captures)
        {
            root["captures"].push_back({{"tick", capture.tick}, {"path", capture.path}});
        }
        root["assertions"] = Json::array();
        for (const auto &assertion : spec.assertions)
        {
            root["assertions"].push_back(
                {{"id", assertion.id},
                 {"probe", assertion.probe},
                 {"comparison", assertion.comparison},
                 {"expected", ToJson(assertion.expected)},
                 {"tolerance", assertion.tolerance}});
        }
        root["termination"] = {{"maximum_tick", spec.termination.maximum_tick},
                               {"on_victory", spec.termination.on_victory},
                               {"on_defeat", spec.termination.on_defeat},
                               {"on_error", spec.termination.on_error}};

        std::error_code error;
        if (const auto parent = path.parent_path(); !parent.empty())
        {
            std::filesystem::create_directories(parent, error);
            if (error)
            {
                return Failure(std::format("Cannot create spec directory: {}", error.message()));
            }
        }
        std::ofstream stream(path, std::ios::trunc);
        if (!stream)
        {
            return Failure(std::format("Cannot write spec: {}", path.string()));
        }
        stream << root.dump(2) << '\n';
        if (!stream)
        {
            return Failure(std::format("Cannot finish spec: {}", path.string()));
        }
        return Result::Success();
    }
    catch (const std::exception &exception)
    {
        return Failure(exception.what());
    }
}

Result ApplyExperimentOverrides(const ExperimentSpec &spec, ApplicationConfig &config)
{
    try
    {
        const auto overrides = Json::parse(spec.config_overrides_json);
        const auto boolean = [&](std::string_view field, bool &target) {
            if (overrides.contains(field))
            {
                target = overrides.at(field).get<bool>();
            }
        };
        const auto number = [&](std::string_view field, std::uint32_t &target) {
            if (overrides.contains(field))
            {
                target = overrides.at(field).get<std::uint32_t>();
            }
        };
        boolean("warp", config.warp);
        boolean("validation", config.validation);
        boolean("gpu_validation", config.gpu_validation);
        boolean("vsync", config.vsync);
        boolean("bloom", config.bloom);
        boolean("outline", config.outline);
        boolean("resize_test", config.resize_test);
        boolean("borderless", config.borderless);
        number("width", config.width);
        number("height", config.height);
        number("frame_cap", config.frame_cap);
        number("render_scale_percent", config.render_scale_percent);
        number("shadow_resolution", config.shadow_resolution);
        number("particle_percentage", config.particle_percentage);
        if (overrides.contains("barrier_mode"))
        {
            const auto mode = overrides.at("barrier_mode").get<std::string>();
            config.barrier_mode = mode == "legacy"    ? BarrierMode::Legacy
                                  : mode == "enhanced" ? BarrierMode::Enhanced
                                                       : BarrierMode::Automatic;
        }
        return Result::Success();
    }
    catch (const std::exception &exception)
    {
        return Failure(exception.what());
    }
}

Result FinalizeExperimentArtifacts(const ExperimentSpec &spec,
                                   const std::filesystem::path &directory,
                                   std::string_view start_utc,
                                   std::string_view end_utc,
                                   int process_exit)
{
    try
    {
        constexpr std::array required{"result.json", "timeline.csv", "events.ndjson"};
        for (const auto name : required)
        {
            if (!std::filesystem::is_regular_file(directory / name))
            {
                return Failure(std::format("Missing experiment artifact: {}", name));
            }
        }
        const auto generated_capture =
            directory / std::format("capture_tick_{}.png", spec.termination.maximum_tick);
        for (const auto &capture : spec.captures)
        {
            if (capture.tick != spec.termination.maximum_tick)
            {
                return Failure("Current render runner captures termination.maximum_tick only.");
            }
            const auto destination = directory / capture.path;
            if (destination != generated_capture)
            {
                std::error_code error;
                std::filesystem::copy_file(generated_capture, destination,
                                           std::filesystem::copy_options::overwrite_existing,
                                           error);
                if (error)
                {
                    return Failure(std::format("Cannot copy capture '{}': {}", capture.path,
                                               error.message()));
                }
            }
            if (!std::filesystem::is_regular_file(destination))
            {
                return Failure(std::format("Missing requested capture: {}", capture.path));
            }
        }

        if (auto saved = SaveExperimentSpec(directory / "spec.json", spec); !saved)
        {
            return saved;
        }

        std::ifstream input(directory / "result.json");
        Json result;
        input >> result;
        if (!result.is_object())
        {
            return Failure("result.json root must be an object.");
        }
        Json assertion_results = Json::array();
        bool assertions_passed = result.value("execution_valid", false) &&
                                 result.value("assertions_passed", false);
        for (const auto &assertion : spec.assertions)
        {
            Json actual;
            bool passed{};
            try
            {
                const Json::json_pointer pointer(assertion.probe);
                actual = result.at(pointer);
                passed = Compare(actual, assertion);
            }
            catch (const std::exception &)
            {
                actual = nullptr;
            }
            assertions_passed = assertions_passed && passed;
            assertion_results.push_back(
                {{"id", assertion.id},
                 {"probe", assertion.probe},
                 {"comparison", assertion.comparison},
                 {"expected", ToJson(assertion.expected)},
                 {"actual", actual},
                 {"tolerance", assertion.tolerance},
                 {"passed", passed}});
        }

        Json artifacts = Json::array();
        std::vector<std::string> paths;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
        {
            if (entry.is_regular_file() && entry.path().filename() != "result.json")
                paths.push_back(entry.path().filename().generic_string());
        }
        std::ranges::sort(paths);
        for (const auto &path : paths)
        {
            const auto hash = FileHash(directory / path);
            artifacts.push_back({{"path", path}, {"fnv1a64", std::format("{:016x}", hash)}});
        }

        result["schema_version"] = 1;
        result["scenario_id"] = spec.scenario_id;
        result["mode"] = ToString(spec.mode);
        result["build_hash"] = spec.build_hash;
        result["content_hash"] = spec.content_hash;
        result["start_utc"] = start_utc;
        result["end_utc"] = end_utc;
        result["process_exit"] = process_exit;
        result["crash"] = false;
        result["device_removed"] = false;
        const auto adapter_path = directory / "adapter.txt";
        if (std::ifstream adapter_file(adapter_path); adapter_file)
        {
            std::string adapter;
            std::string driver;
            std::getline(adapter_file, adapter);
            std::getline(adapter_file, driver);
            result["adapter"] = adapter;
            result["driver"] = driver;
        }
        else
        {
            result["adapter"] = "none";
            result["driver"] = "none";
        }
        result["assertions"] = std::move(assertion_results);
        result["artifacts"] = std::move(artifacts);
        result["execution_valid"] = assertions_passed;
        result["assertions_passed"] = assertions_passed;
        result["user_review"] = "awaiting";

        std::ofstream output(directory / "result.json", std::ios::trunc);
        output << result.dump(2) << '\n';
        if (!output)
        {
            return Failure("Cannot update result.json.");
        }
        if (!assertions_passed)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_experiment",
                                   "Experiment assertions failed.");
        }
        return Result::Success();
    }
    catch (const std::exception &exception)
    {
        return Failure(exception.what());
    }
}

} // namespace hs
