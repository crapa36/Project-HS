#include "character_interchange.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv)
{
    try
    {
        if (argc != 3) throw std::runtime_error("usage: hs_character_interchange <character.interchange.json> <output-directory>");
        const auto imported = hs::character_interchange::Load(argv[1]);
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        for (const auto *name : {"archer.rpkg", "source_metadata.json", "import_report.json"})
            if (std::filesystem::exists(directory / name))
                throw std::runtime_error("refusing to overwrite existing output: " + (directory / name).string());
        const auto package = hs::render_data::BuildEmbeddedCharacterPackage(
            hs::MakeAssetId("render/character/archer.rpkg"), {}, imported.runtime);
        const auto write = [&](const std::filesystem::path &name, const char *data, std::size_t size) {
            const auto destination = directory / name;
            if (std::filesystem::exists(destination))
                throw std::runtime_error("refusing to overwrite existing output: " + destination.string());
            std::ofstream stream(destination, std::ios::binary);
            if (!stream.write(data, static_cast<std::streamsize>(size)))
                throw std::runtime_error("cannot write output: " + destination.string());
        };
        const auto source = imported.source.dump(2);
        const nlohmann::json report{{"schema_version",1},{"status","compatibility_projection_only"},
            {"source_clip_count",imported.source.at("clips").size()},
            {"runtime_clip_count",imported.runtime.clips.size()},
            {"bone_count",imported.runtime.bone_count},{"vertex_count",imported.runtime.vertices.size()},
            {"limitations",imported.limitations}};
        const auto report_text = report.dump(2);
        write("archer.rpkg",reinterpret_cast<const char *>(package.data()),package.size());
        write("source_metadata.json",source.data(),source.size());
        write("import_report.json",report_text.data(),report_text.size());
        std::cout << report_text << '\n';
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n'; return 1;
    }
}
