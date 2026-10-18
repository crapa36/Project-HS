#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <ostream>
#include <string>
#include <string_view>

namespace hs::runtime_detail
{
// RFC 4180 field quoting also keeps provider/display names and native reasons parseable.
inline std::string CsvField(std::string_view value)
{
    std::string field;
    field.reserve(value.size() + 2);
    field += '"';
    for (const auto ch : value)
    {
        if (ch == '"') field += '"';
        field += ch;
    }
    field += '"';
    return field;
}

// Runtime captures one main view; completed GPU frame IDs increase for its lifetime.
class PerformanceLogState
{
  public:
    static constexpr std::uint64_t ElapsedTicks(std::uint64_t begin, std::uint64_t end) noexcept
    {
        return end >= begin ? end - begin : 0;
    }
    std::uint64_t FrameInterval(std::uint64_t qpc) noexcept
    {
        const auto interval = previous_qpc_ ? ElapsedTicks(previous_qpc_, qpc) : 0;
        previous_qpc_ = qpc;
        return interval;
    }
    bool AcceptGpuFrame(std::uint64_t frame, bool available, bool completed) noexcept
    {
        if (!available || !completed || frame <= completed_gpu_frame_) return false;
        completed_gpu_frame_ = frame;
        ++gpu_frames_;
        return true;
    }
    [[nodiscard]] std::uint64_t GpuFrames() const noexcept { return gpu_frames_; }
    [[nodiscard]] std::uint64_t LastGpuFrame() const noexcept { return completed_gpu_frame_; }
    [[nodiscard]] bool GpuFrameCoverageComplete(std::uint64_t rendered_frames) const noexcept
    {
        return gpu_frames_ == rendered_frames && completed_gpu_frame_ == rendered_frames;
    }
  private:
    std::uint64_t previous_qpc_{}, completed_gpu_frame_{}, gpu_frames_{};
};

// Single render-thread owner. Capture writes fixed chunks synchronously; a slow
// disk applies backpressure without a second unbounded queue or writer thread.
class DiagnosticLog
{
  public:
    static constexpr std::size_t kMaximumRows = 4096;
    static constexpr std::size_t kMaximumBytes = 2 * 1024 * 1024;
    static constexpr std::size_t kMaximumLine = 64 * 1024;
    DiagnosticLog(std::ostream &sink, bool capture, std::string_view header = {})
        : sink_(sink), capture_(capture)
    {
        Write(header);
    }
    void Append(std::string_view line)
    {
        ++records_;
        if (finished_ || failed_) { ++dropped_; return; }
        if (capture_)
        {
            Write(line);
            return;
        }
        // Keep whole rows to preserve CSV/NDJSON parseability.
        if (line.size() > kMaximumLine) { ++dropped_; return; }
        std::string stored(line);
        const auto bytes = stored.capacity();
        while (!recent_.empty() &&
            (recent_.size() >= kMaximumRows || retained_bytes_ + bytes > kMaximumBytes))
        {
            retained_bytes_ -= recent_.front().capacity();
            recent_.pop_front();
            ++dropped_;
        }
        if (bytes > kMaximumBytes) { ++dropped_; return; }
        recent_.push_back(std::move(stored));
        retained_bytes_ += bytes;
    }
    bool Finish()
    {
        if (!finished_)
        {
            if (!capture_) for (const auto &line : recent_) Write(line);
            FlushChunk();
            try { sink_.flush(); failed_ |= !sink_; }
            catch (...) { failed_ = true; }
            finished_ = true;
        }
        return !failed_;
    }
    bool Complete() const noexcept { return finished_ && !failed_ && dropped_ == 0; }
    bool Failed() const noexcept { return failed_; }
    std::uint64_t Records() const noexcept { return records_; }
    std::uint64_t Dropped() const noexcept { return dropped_; }
    std::size_t RetainedBytes() const noexcept { return retained_bytes_; }
    std::size_t RetainedRows() const noexcept { return recent_.size(); }
  private:
    void FlushChunk()
    {
        if (used_ && !failed_)
        {
            try { sink_.write(chunk_.data(), static_cast<std::streamsize>(used_)); failed_ |= !sink_; }
            catch (...) { failed_ = true; }
        }
        used_ = 0;
    }
    void Write(std::string_view text)
    {
        while (!text.empty() && !failed_)
        {
            const auto count = std::min(text.size(), chunk_.size() - used_);
            text.copy(chunk_.data() + used_, count);
            used_ += count; text.remove_prefix(count);
            if (used_ == chunk_.size()) FlushChunk();
        }
    }
    std::ostream &sink_;
    bool capture_{}, failed_{}, finished_{};
    std::array<char, 64 * 1024> chunk_{};
    std::size_t used_{}, retained_bytes_{};
    std::deque<std::string> recent_;
    std::uint64_t records_{}, dropped_{};
};
}
