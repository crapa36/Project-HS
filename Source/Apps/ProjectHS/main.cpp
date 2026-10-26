#include <hs/runtime/application.hpp>
#ifndef HS_DEVELOPMENT_TOOLS
#include <hs/core/process_info.hpp>
#include <hs/core/shipped_content_owner.hpp>
#include <hs/runtime/shipping_content.hpp>
#endif

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
    if (argc == 2 && std::string_view(argv[1]) == "--verify-shipping-content")
    {
#ifdef HS_DEVELOPMENT_TOOLS
        std::cerr << "Shipping content verification requires a Release executable.\n";
        return 1;
#else
        const auto directory = hs::CurrentExecutableDirectory();
        if (directory.empty())
        {
            std::cerr << "Could not resolve the installed executable directory.\n";
            return 1;
        }
        hs::ShippedContentOwner owner;
        const auto verified = hs::shipping::OpenImage(directory, hs::shipping::ImageInfo(), owner);
        if (!verified)
        {
            std::cerr << verified.Subsystem() << ": " << verified.Message() << '\n';
            return 1;
        }
        std::cout << "Shipping executable and content image match.\n";
        return 0;
#endif
    }

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
    std::cout << "runtime tick=" << run.final_tick << " checksum=" << run.checksum
              << " frames=" << run.rendered_frames << '\n';
    if (!run.playtest_directory.empty())
        std::cout << "playtest=" << run.playtest_directory.string() << '\n';
    return 0;
}
