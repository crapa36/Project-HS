#include <hs/runtime/audio_engine.hpp>

#include <hs/asset_manifest/selected_generation.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;

int main(int argc, char **argv)
{
    try
    {
        if (argc == 6 && std::string_view(argv[1]) == "--hold")
        {
            hs::AudioEngine audio;
            const hs::SettingsData settings;
            const auto initialization = audio.Initialize(settings);
            if (!initialization || audio.Status().loaded_cues != 107)
                throw std::runtime_error("default AudioEngine failed to load selected Generation: " +
                    std::string(initialization.Message()) + " cues=" +
                    std::to_string(audio.Status().loaded_cues));
            std::ofstream(argv[2]) << "ready\n";
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            bool retried = false;
            while (!fs::exists(argv[3]))
            {
                if (!retried && fs::exists(argv[5]))
                {
                    if (audio.Initialize(settings) || audio.Status().loaded_cues != 107)
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

        hs::AudioEngine audio;
        const hs::SettingsData settings;
        if (const auto result = audio.Initialize(settings, generation.product_paths); !result)
            throw std::runtime_error("AudioEngine rejected selected Generation catalog");
        if (audio.Status().loaded_cues != 107)
            throw std::runtime_error("AudioEngine did not load all 107 authored cues");
        std::cout << "audio generation consumer loaded_cues=107\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "audio generation consumer: " << error.what() << '\n';
        return 1;
    }
}
