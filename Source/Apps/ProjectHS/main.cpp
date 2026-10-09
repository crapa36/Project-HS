#include <hs/runtime/application.hpp>
#include <hs/runtime/save_store.hpp>
#include <hs/runtime/settings_projection.hpp>

#include "command_line.hpp"

#include <Windows.h>

#include <cstddef>
#include <iostream>
#include <string_view>
#include <vector>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

int main(int argc, char **argv)
{
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index)
        arguments.emplace_back(argv[index]);

    const auto initial_config = hs::ParseProjectHsArguments(arguments);
    auto config = initial_config;
    if (!initial_config.smoke)
    {
        hs::SettingsDocument requested_settings;
        bool loaded_from_file{};
        if (hs::SaveStore store;
            store.LoadSettingsV2(requested_settings, &loaded_from_file))
        {
            if (!loaded_from_file)
                requested_settings = hs::LegacyDefaultSettingsDocument();
            config = hs::ParseProjectHsArguments(arguments, &requested_settings);
        }
    }

    const auto run = hs::RunApplication(config);
    if (!run.result)
    {
        std::cerr << run.result.Subsystem() << ": " << run.result.Message() << '\n';
        return 1;
    }
    std::cout << "runtime tick=" << run.final_tick << " checksum=" << run.checksum
              << " frames=" << run.rendered_frames << '\n';
    if (!run.playtest_directory.empty())
        std::cout << "playtest=" << run.playtest_directory.string() << '\n';
    return 0;
}
