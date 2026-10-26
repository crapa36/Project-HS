#pragma once

#include <cstdint>
#include <vector>

namespace hs::content
{
struct HammersleySample
{
    double u{};
    double v{};
};

constexpr double RadicalInverse(std::uint32_t bits)
{
    bits=(bits<<16)|(bits>>16);
    bits=((bits&0x55555555u)<<1)|((bits&0xaaaaaaaau)>>1);
    bits=((bits&0x33333333u)<<2)|((bits&0xccccccccu)>>2);
    bits=((bits&0x0f0f0f0fu)<<4)|((bits&0xf0f0f0f0u)>>4);
    bits=((bits&0x00ff00ffu)<<8)|((bits&0xff00ff00u)>>8);
    return double(bits)*2.3283064365386963e-10;
}
static_assert(RadicalInverse(0)==0.0 && RadicalInverse(1)==0.5);

inline std::vector<HammersleySample> MakeHammersleySamples(std::uint32_t count)
{
    std::vector<HammersleySample> samples;
    samples.reserve(count);
    for(std::uint32_t i=0;i<count;++i)
        samples.push_back({(double(i)+0.5)/count,RadicalInverse(i)});
    return samples;
}
}
