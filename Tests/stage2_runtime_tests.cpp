#include <hs/runtime/experiment_spec.hpp>
#include <hs/runtime/experiment_pipe.hpp>
#include <hs/runtime/save_store.hpp>

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace
{

void Check(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

std::filesystem::path TestDirectory()
{
    return std::filesystem::temp_directory_path() /
           (L"ProjectHS_stage2_runtime_" + std::to_wstring(GetCurrentProcessId()));
}

void WriteCorrupt(const std::filesystem::path &path)
{
    std::ofstream(path, std::ios::trunc) << "{broken";
}

void TestSaveRecovery(const std::filesystem::path &root)
{
    hs::SaveStore store(root);
    hs::ProfileData first;
    first.best_level = 7;
    Check(store.SaveProfile(first).Succeeded(), "first profile save");

    auto second = first;
    second.best_level = 11;
    Check(store.SaveProfile(second).Succeeded(), "second profile save");
    Check(std::filesystem::is_regular_file(root / "profile.json.bak"),
          "atomic replacement retains backup");

    WriteCorrupt(root / "profile.json");
    hs::ProfileData recovered;
    Check(store.LoadProfile(recovered).Succeeded() && recovered.best_level == 7,
          "corrupt primary restores valid backup");

    WriteCorrupt(root / "profile.json");
    WriteCorrupt(root / "profile.json.bak");
    hs::ProfileData reset;
    Check(store.LoadProfile(reset).Succeeded() && reset.best_level == 1,
          "two corrupt copies reset profile");
    std::size_t corrupt_count{};
    for (const auto &entry : std::filesystem::directory_iterator(root))
    {
        if (entry.path().extension() == ".corrupt") ++corrupt_count;
    }
    Check(corrupt_count >= 2, "corrupt profile copies are preserved");
}

void TestSettingsPersistence(const std::filesystem::path &root)
{
    hs::SaveStore store(root);
    hs::SettingsData first;
    first.vsync = false;
    first.frame_cap = 120;
    first.master_volume = 0.7f;
    first.skill_virtual_keys = {'W', 'Q', 'E', 'R'};
    Check(store.SaveSettings(first).Succeeded(), "settings save");

    hs::SettingsData loaded;
    Check(store.LoadSettings(loaded).Succeeded() && !loaded.vsync &&
              loaded.frame_cap == 120 && loaded.master_volume == 0.7f &&
              loaded.skill_virtual_keys == first.skill_virtual_keys,
          "screen, audio, and exchanged key settings round trip");

    auto invalid = first;
    invalid.skill_virtual_keys = {'Q', 'Q', 'E', 'R'};
    Check(!store.SaveSettings(invalid), "duplicate action keys are rejected");
}

void TestExperimentSpecRoundTrip(const std::filesystem::path &root)
{
    hs::ExperimentSpec source;
    source.scenario_id = "stage2-runtime";
    source.seed = 42;
    source.mode = hs::ExperimentMode::SimulationOnly;
    source.build_hash = "0123456789abcdef";
    source.content_hash = "fedcba9876543210";
    source.timeline_actions.push_back({1, 1, "start_session", "{}"});
    source.probes = {"gameplay.checksum", "ecs.count"};
    source.assertions.push_back({"tick", "/tick", "eq", std::uint64_t{60}, 0.0});
    source.termination = {60, true, true, true};

    const auto path = root / "experiment.json";
    Check(hs::SaveExperimentSpec(path, source).Succeeded(), "save experiment spec");
    hs::ExperimentSpec loaded;
    Check(hs::LoadExperimentSpec(path, loaded).Succeeded(), "load experiment spec");
    Check(loaded.scenario_id == source.scenario_id && loaded.seed == 42 &&
              loaded.timeline_actions.size() == 1 && loaded.termination.maximum_tick == 60,
          "experiment spec round trip");
}

void TestNamedPipeCommandRoundTrip()
{
    const auto name = L"test-" + std::to_wstring(GetCurrentProcessId());
    hs::Result server_result = hs::Result::Success();
    std::jthread server([&] {
        server_result = hs::ServeExperimentPipeOnce(
            name, [](std::string_view request, std::string &reply) {
                hs::ExperimentPipeCommand command;
                if (auto parsed = hs::ParseExperimentPipeCommand(request, command); !parsed)
                    return parsed;
                reply = hs::BuildExperimentPipeReply(command.sequence,
                                                     command.target_tick, "accepted", "applied");
                return hs::Result::Success();
            });
    });
    const std::string request =
        R"({"sequence":7,"target_tick":30,"command":"damage_player","payload":{"amount":5}})";
    std::string reply;
    hs::Result transaction = hs::Result::Failure(hs::ErrorCode::InvalidState,
                                                 "test", "not attempted");
    for (unsigned attempt = 0; attempt < 20 && !transaction; ++attempt)
    {
        transaction = hs::TransactExperimentPipe(name, request, reply,
                                                 std::chrono::milliseconds(100));
        if (!transaction) Sleep(5);
    }
    server.join();
    Check(transaction.Succeeded() && server_result.Succeeded(), "named pipe transaction");
    Check(reply.find("\"sequence\":7") != std::string::npos &&
              reply.find("\"status\":\"accepted\"") != std::string::npos,
          "named pipe ack preserves sequence");

    hs::ExperimentPipeCommand invalid;
    Check(!hs::ParseExperimentPipeCommand(R"({"sequence":1})", invalid),
          "named pipe rejects incomplete JSON command");
}

void TestNamedPipeIdempotencyAndTargetTick()
{
    hs::ExperimentPipeCommandQueue queue;
    const std::string accepted =
        R"({"sequence":11,"target_tick":30,"command":"damage_player","payload":{"amount":5}})";
    std::string first;
    std::string duplicate;
    Check(queue.Handle(accepted, 10, first).Succeeded() &&
              queue.Handle(accepted, 20, duplicate).Succeeded() && first == duplicate &&
              queue.AcceptedCount() == 1,
          "duplicate sequence replays response without queueing twice");

    const std::string missed =
        R"({"sequence":12,"target_tick":20,"command":"heal_player","payload":{"amount":5}})";
    std::string rejected;
    Check(queue.Handle(missed, 20, rejected).Succeeded() &&
              rejected.find("missed_target_tick") != std::string::npos &&
              queue.AcceptedCount() == 1,
          "elapsed target tick is rejected");
    const auto commands = queue.TakeAccepted();
    Check(commands.size() == 1 && commands.front().sequence == 11,
          "accepted command executes once");
}

} // namespace

int main()
{
    const auto root = TestDirectory();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    try
    {
        TestSaveRecovery(root);
        TestSettingsPersistence(root);
        TestExperimentSpecRoundTrip(root);
        TestNamedPipeCommandRoundTrip();
        TestNamedPipeIdempotencyAndTargetTick();
        std::filesystem::remove_all(root, error);
        std::cout << "stage2_runtime_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "stage2_runtime_tests failed: " << exception.what() << '\n';
        return 1;
    }
}
