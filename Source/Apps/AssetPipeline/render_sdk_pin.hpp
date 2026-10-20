#pragma once
#include "action.hpp"
#include <stdexcept>

namespace hs::asset_pipeline
{
// Reading the authoritative lock does not add it to the Action depfile. The
// complete selected projection is the hash input and is checked byte-for-byte
// semantically against the current authoritative entry before work begins.
inline Json ReadRenderSdkPin(const fs::path &source, const fs::path &projection,
                             std::string_view sdk)
{
    if (!source.is_absolute() || !projection.is_absolute())
        throw std::runtime_error("SDK pin paths must be absolute");
    const auto lock = Json::parse(ReadBytes(source / "RenderSDKs.lock.json"));
    const auto pin = Json::parse(ReadBytes(projection));
    if (lock.at("schema_version") != 1 || pin.size() != 3 ||
        pin.at("schema_version") != lock.at("schema_version") || pin.at("sdk").get<std::string>() != sdk ||
        pin.at("entry") != lock.at("sdks").at(std::string(sdk)))
        throw std::runtime_error("Selected SDK pin projection differs from authoritative lock");
    return pin.at("entry");
}
}
