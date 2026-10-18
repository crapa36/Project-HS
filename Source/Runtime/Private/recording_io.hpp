#pragma once

#include <fstream>

namespace hs::runtime_detail
{
// Check both buffered writes and close, and still release the file after flush failure.
inline bool FinishRecordingFile(std::ofstream &file) noexcept
{
    bool complete = true;
    try { file.flush(); complete = static_cast<bool>(file); }
    catch (...) { complete = false; }
    try { file.close(); complete = complete && !file.fail(); }
    catch (...) { complete = false; }
    return complete;
}
} // namespace hs::runtime_detail
