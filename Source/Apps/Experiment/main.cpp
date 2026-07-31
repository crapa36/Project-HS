#include <hs/runtime/application.hpp>

#include <Windows.h>

#include <iostream>
#include <string_view>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

int main(int argc, char **argv)
{
    hs::ApplicationConfig config;
    config.smoke = true;
    config.visible = false;
    config.vsync = false;
    config.validation = true;
    config.artifact_directory = "Artifacts/experiment";
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument.starts_with("--artifacts="))
        {
            config.artifact_directory = argument.substr(12);
        }
        else if (argument == "--warp")
        {
            config.warp = true;
        }
    }
    const auto run = hs::RunApplication(config);
    if (!run.result)
    {
        std::cerr << run.result.Message() << '\n';
        return 1;
    }
    std::cout << "experiment tick=" << run.final_tick << " checksum=" << run.checksum << '\n';
    return 0;
}
