#include <hs/runtime/vfx_image_io.hpp>

#include <stdexcept>

namespace
{
void Check(bool value)
{
    if (!value) throw std::runtime_error("canonical VFX image identity regression");
}
}

void TestVfxImageIdentity()
{
    using namespace hs;
    VfxImageData data;
    data.definitions.push_back({});
    data.definitions[0].reserved = 1;
    data.definitions[0].line_width = -0.0f;
    data.payload_hash = 11;
    data.program.strings = {std::byte{0xc3}, std::byte{0xa9}};
    data.effect_output_profiles = {1};
    data.ground_effect_ranges = {{0,1}};
    VfxGroundRecipe ground{};
    ground.tertiary = .125f;
    ground.mask_variants[1] = {{3,2}, .5f, false, true};
    data.ground_recipes = {ground};

    const auto baseline = CanonicalVfxImageBytes(data.View());
    data.payload_hash = 999;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.payload_hash = 11;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    data.definitions[0].line_width = 0.0f;
    Check(CanonicalVfxImageBytes(data.View()) != baseline); // preserve exact IEEE-754 bits.
    data.definitions[0].line_width = -0.0f;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    std::swap(data.definitions[0].reserved, data.ground_recipes[0].gradient_row);
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    std::swap(data.definitions[0].reserved, data.ground_recipes[0].gradient_row);
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    data.ground_recipes[0].mask_variants[1].present = false;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.ground_recipes[0].mask_variants[1].present = true;

    data.program.strings[1] = std::byte{0xa8};
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.program.strings[1] = std::byte{0xa9};
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    data.effect_output_profiles[0] = 2;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.effect_output_profiles[0] = 1;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    data.ground_effect_ranges[0].count = 0;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.ground_effect_ranges[0].count = 1;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    data.ground_recipes.push_back(ground);
    data.ground_recipes[1].tertiary = .25f;
    const auto ordered = CanonicalVfxImageBytes(data.View());
    std::swap(data.ground_recipes[0], data.ground_recipes[1]);
    Check(CanonicalVfxImageBytes(data.View()) != ordered); // authored row order is identity.
}
