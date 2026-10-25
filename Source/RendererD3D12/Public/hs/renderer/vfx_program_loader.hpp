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
    // Rebuild spans after fixture mutation; never cache a view across vector growth.
    VfxProgramView View() const && = delete;
    [[nodiscard]] VfxProgramView View() const & noexcept
    {
        return {effects, sources, outputs, parameters, texture_bindings, slice_indices, curves, curve_keys, effect_lookup, texture_resources, strings, upgrade_bindings, upgrade_sequences};
    }
    operator VfxProgramView() const & noexcept { return View(); }
    operator VfxProgramView() const && = delete;

};

bool LoadVfxProgram(std::span<const std::byte> bytes, VfxProgramData& output, std::string& error);
}
