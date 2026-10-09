#include <hs/asset_manifest/generation_lock.hpp>
#include <hs/asset_manifest/selected_generation.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

int main(int argc, char **argv)
{
    try
    {
        if (argc < 5) throw std::runtime_error("hold|mutation path ready release [bad-pointer]");
        hs::GenerationFileLock mutation;
        hs::SelectedGeneration retained;
        if (std::string_view(argv[1]) == "mutation")
            mutation = hs::GenerationFileLock(fs::path(argv[2]) / ".generation-mutation.lock",
                hs::GenerationFileLock::Mode::Exclusive, true);
        else
        {
            hs::SelectedGeneration selected;
            const auto loaded = hs::LoadSelectedGeneration(argv[2], selected);
            if (!loaded) throw std::runtime_error(std::string(loaded.Message()));
            if (argc == 6)
            {
                const auto id = selected.id;
                const auto paths = selected.product_paths;
                if (hs::LoadSelectedGeneration(argv[5], selected) ||
                    selected.id != id || selected.product_paths != paths)
                    throw std::runtime_error("failed selection changed previous selection");
            }
            retained = selected;
            selected = {}; // Copy must keep the shared lease alive.
        }
        std::ofstream(argv[3]) << "ready\n";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!fs::exists(argv[4]))
        {
            if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("probe timeout");
            for (const auto &[id, path] : retained.product_paths)
                if (!fs::is_regular_file(path)) throw std::runtime_error("active reader product deleted: " + id);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
