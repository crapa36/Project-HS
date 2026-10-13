#ifndef HS_HZB_REGION_INCLUDED
#define HS_HZB_REGION_INCLUDED

// Scalar integer address calculation shared verbatim with the portable oracle test.
#ifdef __cplusplus
#define HS_HZB_OUT(type) type &
#else
#define HS_HZB_OUT(type) out type
#endif
struct HsHzbRegion
{
    uint width, height, offset, level;
    uint min_x, min_y, max_x, max_y;
};

bool HsFindHzbRegion(uint width, uint height, uint levels, uint capacity,
    uint min_x, uint min_y, uint max_x, uint max_y, HS_HZB_OUT(HsHzbRegion) region)
{
    if (width == 0u || height == 0u || levels == 0u || levels > 16u ||
        min_x > max_x || min_y > max_y || max_x >= width || max_y >= height) return false;
    uint offset = 0u, level = 0u;
    while (true)
    {
        // Validate every level before subtraction, multiplication and addition.
        if (width > capacity / height) return false;
        uint elements = width * height;
        if (offset > capacity - elements) return false;
        if (max_x - min_x <= 1u && max_y - min_y <= 1u) break;
        if (level + 1u >= levels) return false;
        offset += elements;
        width = width / 2u + width % 2u;
        height = height / 2u + height % 2u;
        min_x /= 2u; min_y /= 2u; max_x /= 2u; max_y /= 2u;
        ++level;
    }
    region.width = width; region.height = height;
    region.offset = offset; region.level = level;
    region.min_x = min_x; region.min_y = min_y;
    region.max_x = max_x; region.max_y = max_y;
    return true;
}
#undef HS_HZB_OUT
#endif
