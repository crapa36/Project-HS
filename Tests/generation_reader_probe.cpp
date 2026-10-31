#include <hs/asset_manifest/generation_lock.hpp>
#include <hs/asset_manifest/selected_generation.hpp>
#include "../Source/Apps/AssetPipeline/action.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;
namespace ap = hs::asset_pipeline;

int main(int argc, char **argv)
{
    try
    {
        if (argc >= 2 && std::string_view(argv[1]) == "publish-action")
        {
            if (argc != 9)
                throw std::runtime_error("publish-action build-root namespace action-id version product-id filename payload");
            const fs::path root(argv[2]);
            ap::ActionSpec spec;
            spec.action_id = argv[4];
            spec.recipe_id = "fixture.product-set-lifetime";
            spec.recipe_version = static_cast<std::uint32_t>(std::stoul(argv[5]));
            spec.namespace_path = argv[3];
            const auto stage = ap::CreateStage(root / "Actions" / spec.namespace_path);
            try
            {
                const std::string_view payload(argv[8]);
                ap::WriteBytes(stage / argv[7],
                    std::as_bytes(std::span(payload.data(), payload.size())));
                ap::Publish(root, spec, stage,
                    {ap::HashProduct(argv[6], argv[7], stage / argv[7])});
            }
            catch (...)
            {
                ap::RemoveStageAfterFailure(stage);
                throw;
            }
            ap::RemoveStage(stage);
            return 0;
        }

        if (argc >= 2 && std::string_view(argv[1]) == "action-hold")
        {
            if (argc != 7)
                throw std::runtime_error("action-hold build-root namespace product-id ready release");
            const fs::path root(argv[2]);
            ap::UpstreamProduct dependency;
            fs::path product_path;
            const ap::ActionResultSnapshot snapshot(root, argv[3]);
            snapshot.ReadProduct(argv[4], dependency, product_path);

            ap::ActionSpec consumer;
            consumer.action_id = "fixture.consumer";
            consumer.recipe_id = "fixture.consume-product";
            consumer.recipe_version = 1;
            consumer.namespace_path = "consumer";
            consumer.upstream.push_back(dependency);
            const auto consumed = ap::ReadBytes(product_path);
            const auto stage = ap::CreateStage(root / "Actions" / consumer.namespace_path);
            try
            {
                ap::WriteBytes(stage / "consumed.bin",
                    std::as_bytes(std::span(consumed.data(), consumed.size())));
                ap::Publish(root, consumer, stage,
                    {ap::HashProduct("consumer/output", "consumed.bin", stage / "consumed.bin")});
            }
            catch (...)
            {
                ap::RemoveStageAfterFailure(stage);
                throw;
            }
            ap::RemoveStage(stage);
            // Only the original snapshot remains pinned across the downstream publish.
            consumer.upstream.clear();
            dependency = {};

            std::ofstream(argv[5]) << "ready\n";
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!fs::exists(argv[6]))
            {
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("action snapshot probe timeout");
                if (!fs::is_regular_file(product_path))
                    throw std::runtime_error("active ActionResult product was deleted");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return 0;
        }

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
