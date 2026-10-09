#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "content_cooker.hpp"

#include <Windows.h>

#include <filesystem>
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
    std::string_view mode;
    std::filesystem::path output_root;
    if (argc == 2 && std::string_view(argv[1]) == "--validate-only")
    {
        mode = "--validate-only";
    }
    else if (argc == 4 && std::string_view(argv[1]) == "--legacy-oracle" &&
             std::string_view(argv[2]) == "--output-root")
    {
        mode = "--legacy-oracle";
        output_root = std::filesystem::u8path(argv[3]);
        if (output_root.empty() || !output_root.is_absolute())
        {
            std::cerr << "usage: hs_legacy_content_oracle --validate-only | "
                         "--legacy-oracle --output-root <absolute>\n";
            return 2;
        }
    }
    else
    {
        std::cerr << "usage: hs_legacy_content_oracle --validate-only | "
                     "--legacy-oracle --output-root <absolute>\n";
        return 2;
    }

    try
    {
        return hs::content::RunContent(mode, output_root);
    }
    catch (const std::exception &exception)
    {
        std::cerr << "content.error " << exception.what() << '\\n';
        return 1;
    }
}
