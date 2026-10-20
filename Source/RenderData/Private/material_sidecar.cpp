#include <hs/render_data/material_sidecar.hpp>

#include <cstddef>
#include <stdexcept>

namespace hs::render_data
{
MaterialSidecarLayout ComputeMaterialSidecarLayout(std::uint32_t mask)
{
    if ((mask & ~31u) != 0)
        throw std::invalid_argument("material sidecar layout: unknown family mask bits");
    constexpr std::array<std::uint32_t, 5> words{2, 1, 2, 2, 1};
    MaterialSidecarLayout result;
    result.mask = mask;
    for (std::size_t field = 0; field < words.size(); ++field)
        if ((mask & (1u << field)) != 0)
        {
            result.offsets[field] = result.word_count;
            result.word_count += words[field];
        }
    result.plane_count = (result.word_count + 3u) / 4u;
    return result;
}
} // namespace hs::render_data
