#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "content_cooker.hpp"

#include <Windows.h>

#include <iostream>
#include <string_view>

namespace
{

struct ScopedComInitialization
{
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~ScopedComInitialization()
    {
        if (SUCCEEDED(result))
        {
            CoUninitialize();
        }
    }
};

} // namespace

int main(int argc, char **argv)
{
    const ScopedComInitialization com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
    {
        std::cerr << "content.error COM initialization failed\\n";
        return 1;
    }
    const auto mode = argc == 2 ? std::string_view(argv[1]) : std::string_view{};
    if (mode != "--validate-only" && mode != "--cook")
    {
        std::cerr << "usage: hs_content --validate-only | --cook\\n";
        return 2;
    }

    try
    {
        return hs::content::RunContent(mode);
    }
    catch (const std::exception &exception)
    {
        std::cerr << "content.error " << exception.what() << '\\n';
        return 1;
    }
}
