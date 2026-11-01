#include "generation_reload_watch.hpp"

#ifdef HS_DEVELOPMENT_TOOLS
#include <nlohmann/json.hpp>

#include <exception>

namespace hs::runtime_detail
{
PreparedContentGeneration PrepareContentGeneration(std::filesystem::path pointer_path,
    std::string pointer_text, std::string active_id, std::string active_scope)
{
    PreparedContentGeneration prepared;
    try
    {
        std::string current;
        prepared.result = ReadGenerationPointer(pointer_path, current);
        if (!prepared.result) return prepared;
        if (current != pointer_text)
        {
            prepared.superseded = true;
            return prepared;
        }
        const auto expected = nlohmann::json::parse(pointer_text);
        prepared.result = LoadSelectedGeneration(pointer_path, prepared.candidate);
        if (prepared.result)
        {
            // Load reads the pathname again; identity checks also reject an A -> B -> A race.
            const auto &candidate = prepared.candidate;
            if (expected.at("schema_version") != 1 ||
                expected.at("generation_id") != candidate.id ||
                expected.at("scope") != candidate.scope ||
                expected.at("manifest_locator") != "Generations/" + candidate.id + "/generation.json")
                prepared.superseded = true;
            else if (candidate.scope != active_scope)
                prepared.result = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    "Content Generation changes the selected scope.");
            else if (candidate.id != active_id)
            {
                prepared.stage = "integrity";
                prepared.result = VerifySelectedGenerationProducts(candidate);
            }
        }
        if (const auto read = ReadGenerationPointer(pointer_path, current); !read)
        {
            if (prepared.result) prepared.result = read;
        }
        else
            prepared.superseded |= current != pointer_text;
    }
    catch (const std::exception &error)
    {
        prepared.result = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
            "Content Generation preparation failed: " + std::string(error.what()));
    }
    return prepared;
}
} // namespace hs::runtime_detail
#endif
