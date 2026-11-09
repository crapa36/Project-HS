#include <hs/runtime/audio_engine.hpp>

#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/core/cooked_format.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace
{
void CheckCatalogCues(hs::AudioEngine &audio, const fs::path &catalog_path)
{
    std::ifstream input(catalog_path);
    const auto catalog = nlohmann::json::parse(input);
    const auto &entries = catalog.at("entries");
    if (!input || !entries.is_array() || entries.empty() ||
        audio.Status().loaded_cues != entries.size())
        throw std::runtime_error("AudioEngine cue count does not match selected catalog");
    for (const auto &entry : entries)
    {
        const auto cue = hs::MakeAssetId(entry.at("asset_id").get<std::string>());
        if (audio.CueReadiness(cue) == hs::AudioCueReadiness::Missing)
            throw std::runtime_error("AudioEngine is missing a selected catalog cue");
    }
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc == 6 && std::string_view(argv[1]) == "--hold")
        {
            hs::AudioEngine audio;
            hs::SettingsData settings;
            settings.master_volume = 0;
            const auto initialization = audio.Initialize(settings);
            if (!initialization)
                throw std::runtime_error("default AudioEngine failed to load selected Generation: " +
                    std::string(initialization.Message()) + " cues=" +
                    std::to_string(audio.Status().loaded_cues));
            const auto expected_cues = audio.Status().loaded_cues;
            {
                const fs::path root = fs::absolute(argv[0]).parent_path();
                hs::SelectedGeneration generation;
                const auto selected = hs::LoadSelectedGeneration(
                    root / "GenerationState/runtime-full.json", generation);
                if (!selected)
                    throw std::runtime_error("invalid selected runtime Generation for audio check");
                CheckCatalogCues(audio, generation.product_paths.at("audio/audio_cues.json"));
            }
            std::ofstream(argv[2]) << "ready\n";
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            bool retried = false;
            while (!fs::exists(argv[3]))
            {
                if (!retried && fs::exists(argv[5]))
                {
                    if (audio.Initialize(settings) || audio.Status().loaded_cues != expected_cues)
                        throw std::runtime_error("failed reselect lost previous audio");
                    std::ofstream(std::string(argv[5]) + ".checked") << "ready\n";
                    retried = true;
                }
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("audio hold timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            audio.Shutdown();
            std::ofstream(std::string(argv[2]) + ".shutdown") << "ready\n";
            while (!fs::exists(argv[4]))
            {
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("audio exit timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return 0;
        }
        if (argc != 2)
            throw std::runtime_error("usage: hs_audio_generation_consumer_tests <build-root>");
        const fs::path root = fs::absolute(argv[1]);
        hs::SelectedGeneration generation;
        const auto selected = hs::LoadSelectedGeneration(
            root / "GenerationState/test-audio.json", generation);
        if (!selected || generation.scope != "test-audio")
            throw std::runtime_error("invalid selected audio Generation");
        const auto catalog_path = generation.product_paths.at("audio/audio_cues.json");

        hs::AudioEngine audio;
        hs::SettingsData settings;
        settings.master_volume = 0;
        std::shared_ptr<hs::audio_detail::PreparedAudio> prepared;
        if (const auto result = hs::PrepareAudio(generation.product_paths, prepared); !result)
            throw std::runtime_error("AudioEngine rejected selected Generation preparation");
        auto generation_owner = std::make_shared<hs::SelectedGeneration>(std::move(generation));
        if (const auto result = audio.Initialize(settings, prepared, generation_owner); !result)
            throw std::runtime_error("AudioEngine rejected selected Generation catalog");
        CheckCatalogCues(audio, catalog_path);
        std::cout << "audio generation consumer loaded_cues=" << audio.Status().loaded_cues << '\n';
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "audio generation consumer: " << error.what() << '\n';
        return 1;
    }
}
