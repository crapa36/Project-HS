#include "release_presentation_codegen.hpp"

#include <bit>
#include <cstdint>

namespace hs::content
{
std::string EmitReleasePresentationCpp(const PresentationCatalog &catalog)
{
    std::string source =
        "#include <hs/presentation/presentation_catalog.hpp>\n"
        "#include <array>\n#include <bit>\n#include <cstdint>\n\n"
        "namespace hs::shipping\n{\nnamespace\n{\n"
        "constinit const PresentationCatalog kPresentation{\n    {";
    const auto append_float = [&](float value) {
        source += "std::bit_cast<float>(std::uint32_t{";
        source += std::to_string(std::bit_cast<std::uint32_t>(value));
        source += "}), ";
    };
    append_float(catalog.camera.yaw_degrees);
    append_float(catalog.camera.pitch_degrees);
    append_float(catalog.camera.vertical_fov_degrees);
    append_float(catalog.camera.distance_m);
    source += std::to_string(catalog.camera.minimum_distance_percent) + "u, ";
    source += std::to_string(catalog.camera.maximum_distance_percent) + "u},\n";
    // Numeric byte arrays avoid source encoding and signed-char conversions, including trailing zeros.
    const auto append_text_rows = [&](const auto &rows) {
        source += "    {{\n";
        for (const auto &row : rows)
        {
            const auto width = std::to_string(row.size());
            source += "        std::bit_cast<std::array<char, " + width + ">>(std::array<std::uint8_t, " + width + ">{";
            for (std::size_t index = 0; index < row.size(); ++index)
            {
                if (index) source += ',';
                source += std::to_string(static_cast<unsigned char>(row[index]));
            }
            source += "}),\n";
        }
        source += "    }},\n";
    };
    append_text_rows(catalog.relic_names);
    append_text_rows(catalog.relic_rules);
    source +=
        "};\n}\n"
        "const PresentationCatalog &Presentation() noexcept\n{\n"
        "    return kPresentation;\n}\n}\n";
    return source;
}
}
