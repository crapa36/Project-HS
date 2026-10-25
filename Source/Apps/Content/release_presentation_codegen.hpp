#pragma once

#include <hs/presentation/presentation_catalog.hpp>

#include <string>

namespace hs::content
{
// Input is the checked BuildPresentationCatalog result; output belongs to the release link transaction.
[[nodiscard]] std::string EmitReleasePresentationCpp(const PresentationCatalog &catalog);
}
