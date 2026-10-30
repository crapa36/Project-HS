#include <hs/runtime/application.hpp>

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

    const auto config = hs::ParseProjectHsArguments(arguments);
    const auto run = hs::RunApplication(config);
    if (!run.result)
    {
        std::cerr << run.result.Subsystem() << ": " << run.result.Message() << '\n';
        return 1;
    }
#ifdef HS_DEVELOPMENT_TOOLS
    if (!run.playtest_directory.empty())
        std::cout << "playtest=" << run.playtest_directory.string() << '\n';
#endif
    return 0;
}
