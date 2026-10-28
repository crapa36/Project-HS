#pragma once
#include <algorithm>
#include <array>
#include <bitset>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>

namespace hs::runtime_detail
{
using DiagnosticClock = std::uint64_t (*)() noexcept;
struct DiagnosticSample
{
    std::uint64_t ticks{}, calls{};
};

// Capture inventory for selected render-frame VFX checkpoints, not process RSS.
struct VfxWorkspaceSample
{
    std::size_t retained_bytes{};
    std::size_t raw_event_bytes{};
    std::size_t prepared_bytes{};
    std::size_t frozen_native_bytes{};
    std::size_t frame_scratch_bytes{};

    [[nodiscard]] std::size_t TotalBytes() const noexcept
    {
        const auto add = [](std::size_t a, std::size_t b) noexcept {
            return b > std::numeric_limits<std::size_t>::max() - a
                ? std::numeric_limits<std::size_t>::max() : a + b;
        };
        return add(add(add(add(retained_bytes, raw_event_bytes), prepared_bytes),
            frozen_native_bytes), frame_scratch_bytes);
    }
};

class VfxWorkspacePeak
{
  public:
    void Observe(VfxWorkspaceSample sample) noexcept
    {
        if (!observed_ || sample.TotalBytes() > sample_.TotalBytes())
        {
            sample_ = sample;
            observed_ = true;
        }
    }
    [[nodiscard]] VfxWorkspaceSample Sample() const noexcept { return sample_; }
    [[nodiscard]] std::size_t TotalBytes() const noexcept { return sample_.TotalBytes(); }
  private:
    VfxWorkspaceSample sample_{};
    bool observed_{};
};

// A null clock leaves ordinary play free of diagnostic clock reads.
class DiagnosticScope
{
  public:
    DiagnosticScope(DiagnosticSample &sample, DiagnosticClock clock) noexcept
        : sample_(sample), clock_(clock), begin_(clock ? clock() : 0) {}
    ~DiagnosticScope() { Stop(); }
    DiagnosticScope(const DiagnosticScope &) = delete;
    void Stop() noexcept
    {
        if (!clock_) return;
        const auto end = clock_();
        sample_.ticks += end >= begin_ ? end - begin_ : 0;
        ++sample_.calls;
        clock_ = nullptr;
    }
  private:
    DiagnosticSample &sample_;
    DiagnosticClock clock_{};
    std::uint64_t begin_{};
};

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

template <typename T>
inline void AppendCsvValue(std::string &row, T value)
{
    static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>);
    char buffer[64];
    const auto converted = [&] {
        if constexpr (std::is_floating_point_v<T>)
            return std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
        else
            return std::to_chars(buffer, buffer + sizeof(buffer), value);
    }();
    if (!row.empty()) row += ',';
    row.append(buffer, converted.ptr);
}

template <typename... Values>
inline void AppendCsvValues(std::string &row, const Values &...values)
{
    (AppendCsvValue(row, values), ...);
}

inline void AppendCsvText(std::string &row, std::string_view value)
{
    if (!row.empty()) row += ',';
    row += '"';
    for (const auto ch : value)
    {
        if (ch == '"') row += '"';
        row += ch;
    }
    row += '"';
}

// Runtime captures one main view; slot reuse and capture can observe completions out of order.
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
        if (!available || !completed || frame <= contiguous_gpu_frame_ || gpu_frame_overflowed_)
            return false;
        if (frame - contiguous_gpu_frame_ > pending_gpu_frames_.size())
        {
            gpu_frame_overflowed_ = true;
            return false;
        }
        const auto slot = static_cast<std::size_t>(frame % pending_gpu_frames_.size());
        if (pending_gpu_frames_[slot]) return false;
        pending_gpu_frames_[slot] = true;
        completed_gpu_frame_ = std::max(completed_gpu_frame_, frame);
        ++gpu_frames_;
        while (contiguous_gpu_frame_ < completed_gpu_frame_)
        {
            const auto next = static_cast<std::size_t>((contiguous_gpu_frame_ + 1) % pending_gpu_frames_.size());
            if (!pending_gpu_frames_[next]) break;
            pending_gpu_frames_[next] = false;
            ++contiguous_gpu_frame_;
        }
        return true;
    }
    [[nodiscard]] std::uint64_t GpuFrames() const noexcept { return gpu_frames_; }
    [[nodiscard]] std::uint64_t LastGpuFrame() const noexcept { return completed_gpu_frame_; }
    [[nodiscard]] bool GpuFrameCoverageComplete(std::uint64_t rendered_frames) const noexcept
    {
        return !gpu_frame_overflowed_ && gpu_frames_ == rendered_frames &&
               contiguous_gpu_frame_ == rendered_frames && completed_gpu_frame_ == rendered_frames;
    }
  private:
    std::uint64_t previous_qpc_{}, completed_gpu_frame_{}, contiguous_gpu_frame_{}, gpu_frames_{};
    // Match the renderer capture stream bound without allocating a lifetime-long ID set.
    std::bitset<240> pending_gpu_frames_;
    bool gpu_frame_overflowed_{};
};

// One thread owns each log. Capture writes fixed chunks synchronously; a slow
// disk applies backpressure without a second unbounded queue or writer thread.
class DiagnosticLog
{
  public:
    DiagnosticLog(std::ostream &sink, bool enabled, std::string_view header = {},
                  DiagnosticClock clock = nullptr)
        : sink_(sink), enabled_(enabled), clock_(clock)
    {
        if (enabled_)
        {
            chunk_.reset(new (std::nothrow) Chunk{});
            failed_ = !chunk_;
            if (!failed_) Write(header);
        }
    }
    void Append(std::string_view line)
    {
        if (!enabled_) return;
        ++records_;
        if (finished_ || failed_) { ++dropped_; return; }
        Write(line);
    }
    bool Flush()
    {
        if (enabled_ && !finished_)
        {
            FlushChunk();
            DiagnosticScope scope(io_, clock_);
            try { sink_.flush(); failed_ |= !sink_; }
            catch (...) { failed_ = true; }
        }
        return !failed_;
    }
    bool Finish()
    {
        Flush();
        finished_ = true;
        return !failed_;
    }
    bool Complete() const noexcept { return finished_ && !failed_ && dropped_ == 0; }
    bool Failed() const noexcept { return failed_; }
    std::uint64_t Records() const noexcept { return records_; }
    std::uint64_t Dropped() const noexcept { return dropped_; }
    DiagnosticSample Io() const noexcept { return io_; }
    std::uint64_t Bytes() const noexcept { return bytes_; }
  private:
    void FlushChunk()
    {
        if (used_ && !failed_)
        {
            DiagnosticScope scope(io_, clock_);
            try { sink_.write(chunk_->data(), static_cast<std::streamsize>(used_)); failed_ |= !sink_; }
            catch (...) { failed_ = true; }
            if (!failed_) bytes_ += used_;
        }
        used_ = 0;
    }
    void Write(std::string_view text)
    {
        while (!text.empty() && !failed_)
        {
            const auto count = std::min(text.size(), chunk_->size() - used_);
            text.copy(chunk_->data() + used_, count);
            used_ += count; text.remove_prefix(count);
            if (used_ == chunk_->size()) FlushChunk();
        }
    }
    std::ostream &sink_;
    bool enabled_{}, failed_{}, finished_{};
    DiagnosticClock clock_{};
    DiagnosticSample io_{};
    using Chunk = std::array<char, 64 * 1024>;
    std::unique_ptr<Chunk> chunk_;
    std::size_t used_{};
    std::uint64_t records_{}, dropped_{}, bytes_{};
};
}
