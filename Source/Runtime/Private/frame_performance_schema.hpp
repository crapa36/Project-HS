#pragma once

#include <string>

namespace hs::runtime_detail
{
inline std::string FramePerformanceCsvHeader()
{
    std::string result;
    const auto append = [&](std::string_view name) {
        if (!result.empty()) result.push_back(',');
        result.append(name);
    };
#define HS_FRAME_PERFORMANCE_FIELD(name, expression) append(#name);
#include "frame_performance_fields.inc"
#undef HS_FRAME_PERFORMANCE_FIELD
    return result;
}
}
