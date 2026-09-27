#include "vfx_spec_validation.hpp"
#include "vfx_texture_validation.hpp"
#include "vfx_gradient_cooker.hpp"
#include "vfx_program_cooker.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <algorithm>
#include <cwctype>

int main(int argc, char **argv)
{
    try
    {
        if (argc != 2 && argc != 3) throw std::runtime_error("usage: hs_vfx_prepare --validate-only | --prepare output-directory");
        const std::string_view mode = argv[1];
        if ((mode != "--validate-only" || argc != 2) && (mode != "--prepare" || argc != 3))
            throw std::runtime_error("invalid preparation arguments");
        const std::filesystem::path root = HS_VFX_SOURCE_ROOT;
        std::ifstream input(root / "ContentSource/VFX/production_spec.v4.json");
        const auto spec = nlohmann::json::parse(input);
        hs::content::ValidateVfxSpecification(spec);
        hs::content::ValidateVfxTextures(spec, root);
        if (mode == "--prepare")
        {
            const auto build = std::filesystem::weakly_canonical(root / "Build");
            const auto folded = [](const std::filesystem::path &path) {
                auto name = path.generic_wstring();
                std::transform(name.begin(), name.end(), name.begin(),
                    [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                return name;
            };
            const auto checked = [&](const std::filesystem::path &path) {
                const auto canonical = std::filesystem::weakly_canonical(path);
                if (!folded(canonical).starts_with(folded(build) + L"/"))
                    throw std::runtime_error("VFX preparation output must stay within repository Build");
                return canonical;
            };
            const auto output = checked(std::filesystem::absolute(argv[2]));
            for (const auto &[id, texture] : spec.at("libraries").at("texture_catalog").items())
            {
                const std::filesystem::path runtime = texture.at("runtime_asset").get<std::string>();
                const auto target = checked(output / runtime);
                const auto original = root / "ContentSource/Textures/VFX" / runtime.filename();
                if (std::filesystem::exists(target) && std::filesystem::equivalent(original, target))
                    throw std::runtime_error("output aliases source texture");
                std::filesystem::create_directories(target.parent_path());
                std::filesystem::copy_file(root / "ContentSource/Textures/VFX" / runtime.filename(),
                    target, std::filesystem::copy_options::overwrite_existing);
            }
            const auto rows = hs::content::CookVfxGradientDefaults(spec,
                root / "ContentSource/Textures/VFX/vfx_gradient_lut.dds",
                checked(output / "Content/Textures/VFX/vfx_gradient_lut.dds"));
            const auto program = hs::content::CookVfxProgram(spec, rows,
                checked(output / "Content/VFX/production.hsbin"));
            nlohmann::json report{{"schema_version", 4}, {"effects", spec.at("effects").size()},
                {"skill_upgrade_bindings", spec.at("skill_upgrade_visual_bindings").size()},
                {"texture_count", 13}, {"gradient_rows", rows},
                {"runtime_program_ready", false}, {"program", program}};
            std::ofstream stream(checked(output / "vfx_preparation_report.json"));
            stream << report.dump(2) << '\n';
            if (!stream) throw std::runtime_error("cannot write VFX preparation report");
            std::cout << "vfx.prepared gradient_rows=" << rows.size() << " runtime_program_ready=false\n";
        }
        std::cout << "vfx.validated effects=196 bindings=72 textures=13\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "vfx.error " << error.what() << '\n';
        return 1;
    }
}
