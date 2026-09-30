#pragma once

#include "vfx_program.hpp"
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace hs
{
struct VfxProgramData
{
    std::vector<VfxEffectRecord> effects;
    std::vector<VfxSourceRecord> sources;
    std::vector<VfxOutputRecord> outputs;
    std::vector<VfxParameterRecord> parameters;
    std::vector<VfxTextureBindingRecord> texture_bindings;
    std::vector<std::uint32_t> slice_indices;
    std::vector<VfxCurveRecord> curves;
    std::vector<VfxCurveKey> curve_keys;
    std::vector<VfxEffectLookupRecord> effect_lookup;
    std::vector<VfxTextureResourceRecord> texture_resources;
    std::vector<std::byte> strings;
    std::vector<VfxUpgradeBindingRecord> upgrade_bindings;
    std::vector<std::uint32_t> upgrade_sequences;
};

bool LoadVfxProgram(std::span<const std::byte> bytes, VfxProgramData& output, std::string& error);
}
