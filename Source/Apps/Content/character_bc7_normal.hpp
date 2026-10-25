#pragma once
#include "character_texture_mips.hpp"

namespace hs::content
{
using CharacterNormalBlock = std::array<CharacterTexel, 16>;
struct CharacterNormalBlockScore
{
    bool valid{true};
    std::uint32_t invalid_texels{};
    double maximum_angle{}, squared_angles{}, rgba_squared{}, maximum_alpha_error{};
};
inline bool CharacterNormalDirectionsCompatible(const CharacterTexel &reference,const CharacterTexel &decoded)
{
    double dot{},reference_length{},decoded_length{};
    for(unsigned channel=0;channel<4;++channel)
        if(!std::isfinite(reference[channel])||!std::isfinite(decoded[channel])||reference[channel]<0||reference[channel]>1||decoded[channel]<0||decoded[channel]>1)
            return false;
    for(unsigned channel=0;channel<3;++channel)
    {
        const double a=double(reference[channel])*2-1,b=double(decoded[channel])*2-1;
        dot+=a*b;reference_length+=a*a;decoded_length+=b*b;
    }
    return reference_length>0&&decoded_length>0&&dot>0;
}
inline CharacterNormalBlockScore ScoreCharacterNormalBlock(const CharacterNormalBlock &reference,const CharacterNormalBlock &decoded)
{
    CharacterNormalBlockScore result;
    for(unsigned pixel=0;pixel<16;++pixel)
    {
        const bool valid=CharacterNormalDirectionsCompatible(reference[pixel],decoded[pixel]);
        if(!valid) { result.valid=false;++result.invalid_texels; }
        double dot{},reference_length{},decoded_length{};
        for(unsigned channel=0;channel<4;++channel)
        {
            const double delta=double(decoded[pixel][channel])-reference[pixel][channel];
            result.rgba_squared+=delta*delta;
            if(channel==3) result.maximum_alpha_error=std::max(result.maximum_alpha_error,std::abs(delta));
            else
            {
                const double a=double(reference[pixel][channel])*2-1,b=double(decoded[pixel][channel])*2-1;
                dot+=a*b;reference_length+=a*a;decoded_length+=b*b;
            }
        }
        const double angle=reference_length>0&&decoded_length>0&&std::isfinite(dot)
            ? std::acos(std::clamp(dot/std::sqrt(reference_length*decoded_length),-1.0,1.0))
            : 3.14159265358979323846;
        result.maximum_angle=std::max(result.maximum_angle,angle);result.squared_angles+=angle*angle;
    }
    return result;
}
inline bool CharacterNormalAlphaErrorNotWorse(const CharacterNormalBlock &reference,
    const CharacterNormalBlock &original,const CharacterNormalBlock &candidate)
{
    for(unsigned pixel=0;pixel<16;++pixel)
        if(!std::isfinite(candidate[pixel][3]) || std::abs(double(candidate[pixel][3])-reference[pixel][3])>
            std::abs(double(original[pixel][3])-reference[pixel][3])) return false;
    return true;
}
inline bool PreferCharacterNormalBlock(const CharacterNormalBlockScore &candidate,const CharacterNormalBlockScore &current)
{
    if(candidate.valid!=current.valid) return candidate.valid;
    if(!candidate.valid) return false;
    if(candidate.maximum_angle!=current.maximum_angle) return candidate.maximum_angle<current.maximum_angle;
    if(candidate.squared_angles!=current.squared_angles) return candidate.squared_angles<current.squared_angles;
    return candidate.rgba_squared<current.rgba_squared;
}
} // namespace hs::content
