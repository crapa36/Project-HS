#include "../Source/Runtime/Private/runtime_diagnostics.hpp"
#include "../Source/Runtime/Private/frame_performance_schema.hpp"
#include "../Source/Runtime/Private/vfx_workspace_size.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
void Check(bool ok) { if (!ok) throw std::runtime_error("diagnostic contract"); }
std::uint64_t clock_ticks{};
std::uint64_t FakeClock() noexcept { return ++clock_ticks; }

void CheckFramePerformanceSchema()
{
    using hs::runtime_detail::kFramePerformanceTaskNames;
    using hs::runtime_detail::kVfxProfilePrepareTaskIndex;
    Check(kFramePerformanceTaskNames.size() == 24);
    Check(kFramePerformanceTaskNames[10] == "vfx_local_outputs");
    Check(kVfxProfilePrepareTaskIndex == 23);
    Check(kFramePerformanceTaskNames[kVfxProfilePrepareTaskIndex] == "vfx_profile_prepare");
    const auto header = hs::runtime_detail::FramePerformanceCsvHeader();
    std::string sentinel_row;
    std::size_t sentinel_index{};
#define HS_FRAME_PERFORMANCE_FIELD(name, expression) \
    hs::runtime_detail::AppendCsvValue(sentinel_row, sentinel_index++);
#define HS_FRAME_PERFORMANCE_TASKS() \
    for (std::size_t task = 0; task < hs::runtime_detail::kFramePerformanceTaskNames.size(); ++task) \
    { \
        hs::runtime_detail::AppendCsvValue(sentinel_row, sentinel_index++); \
        hs::runtime_detail::AppendCsvValue(sentinel_row, sentinel_index++); \
    }
#include "../Source/Runtime/Private/frame_performance_fields.inc"
#undef HS_FRAME_PERFORMANCE_TASKS
#undef HS_FRAME_PERFORMANCE_FIELD
    std::istringstream csv(header + "\n" + sentinel_row + "\n");
    std::string header_line, row_line;
    std::getline(csv, header_line);
    std::getline(csv, row_line);
    const auto split = [](const std::string &line) {
        std::vector<std::string> fields;
        std::istringstream input(line);
        for (std::string field; std::getline(input, field, ',');) fields.push_back(std::move(field));
        return fields;
    };
    const auto names = split(header_line);
    const auto values = split(row_line);
    Check(names.size() == values.size() && names.size() == sentinel_index);
    Check(names.size() == 202);
    const auto profile_task_name = std::find(names.begin(), names.end(), "vfx_profile_prepare_us");
    Check(profile_task_name != names.end());
    const auto profile_task_column = static_cast<std::size_t>(profile_task_name - names.begin());
    Check(names[profile_task_column] == "vfx_profile_prepare_us");
    Check(names[profile_task_column + 1] == "vfx_profile_prepare_calls");
    Check(names[profile_task_column - 2] == "vfx_fresnel_us");
    Check(names[profile_task_column - 1] == "vfx_fresnel_calls");
    auto unique_names = names;
    std::sort(unique_names.begin(), unique_names.end());
    Check(std::adjacent_find(unique_names.begin(), unique_names.end()) == unique_names.end());
    for (std::size_t index = 0; index < values.size(); ++index)
        Check(values[index] == std::to_string(index));
    std::cout << "FRAME_SCHEMA_HEADER:" << header << '\n';
    std::cout << "FRAME_SCHEMA_ROW:" << sentinel_row << '\n';
}

void CheckNativeCopyHeadroom()
{
    using hs::runtime_detail::vfx_retained_size::NativeCopyGrowthBytes;
    std::vector<int> pending, frozen, prepared;
    prepared.reserve(64);
    pending.resize(prepared.capacity());
    Check(NativeCopyGrowthBytes(pending, frozen, prepared) == 0);
    prepared.push_back(1);
    Check(NativeCopyGrowthBytes(pending, frozen, prepared) ==
          (pending.size() / 2) * sizeof(int));
    frozen.reserve(pending.size());
    Check(NativeCopyGrowthBytes(pending, frozen, prepared) == 0);
    pending.resize(frozen.capacity() + 1);
    Check(NativeCopyGrowthBytes(pending, frozen, prepared) == 0);
    prepared.clear();
    prepared.reserve(pending.size());
    Check(NativeCopyGrowthBytes(pending, frozen, prepared) == 0);

    const auto nonempty_retained = NativeCopyGrowthBytes(12, 0, 4, sizeof(int));
    const auto empty_retained = NativeCopyGrowthBytes(12, 0, 0, sizeof(int));
    Check(nonempty_retained == 2 * sizeof(int) && empty_retained == 6 * sizeof(int));
}
struct FailedSink : std::streambuf
{
    std::streamsize xsputn(const char*,std::streamsize) override { return 0; }
    int sync() override { return -1; }
};
int main()
{
    CheckFramePerformanceSchema();
    CheckNativeCopyHeadroom();
    using hs::runtime_detail::DiagnosticLog;
    hs::runtime_detail::DiagnosticSample sample{};
    { hs::runtime_detail::DiagnosticScope scope(sample, nullptr); }
    Check(sample.ticks == 0 && sample.calls == 0 && clock_ticks == 0);
    {
        hs::runtime_detail::DiagnosticScope scope(sample, FakeClock);
        ++clock_ticks;
        scope.Stop();
        scope.Stop();
    }
    Check(sample.ticks == 2 && sample.calls == 1 && clock_ticks == 3);
    Check(hs::runtime_detail::CsvField("") == "\"\"");
    Check(hs::runtime_detail::CsvField("pass, \"native\"\nnext") == "\"pass, \"\"native\"\"\nnext\"");
    hs::runtime_detail::VfxWorkspacePeak workspace_peak;
    workspace_peak.Observe({.retained_bytes = 4, .raw_event_bytes = 3,
        .prepared_bytes = 2, .frozen_native_bytes = 1, .frame_scratch_bytes = 5});
    Check(workspace_peak.TotalBytes() == 15 &&
          workspace_peak.Sample().raw_event_bytes == 3);
    workspace_peak.Observe({.retained_bytes = 9});
    Check(workspace_peak.TotalBytes() == 15);
    workspace_peak.Observe({.retained_bytes = std::numeric_limits<std::size_t>::max(),
        .raw_event_bytes = 1});
    Check(workspace_peak.TotalBytes() == std::numeric_limits<std::size_t>::max() &&
          workspace_peak.Sample().raw_event_bytes == 1);
    hs::runtime_detail::PerformanceLogState performance;
    Check(performance.FrameInterval(1000) == 0);
    Check(performance.FrameInterval(2100) == 1100); // Includes work and pacing before the next frame.
    Check(performance.FrameInterval(1500) == 0);
    Check(performance.FrameInterval(1600) == 100);
    Check(performance.ElapsedTicks(2100, 1500) == 0);
    Check(!performance.AcceptGpuFrame(0, true, true));
    Check(!performance.AcceptGpuFrame(7, false, true));
    Check(!performance.AcceptGpuFrame(7, true, false));
    Check(performance.AcceptGpuFrame(7, true, true));
    Check(!performance.AcceptGpuFrame(7, true, true));
    Check(performance.AcceptGpuFrame(6, true, true));
    Check(!performance.AcceptGpuFrame(6, true, true));
    Check(performance.AcceptGpuFrame(8, true, true));
    Check(performance.GpuFrames() == 3 && performance.LastGpuFrame() == 8);
    Check(!performance.GpuFrameCoverageComplete(8));
    hs::runtime_detail::PerformanceLogState complete_tail;
    Check(complete_tail.GpuFrameCoverageComplete(0));
    Check(complete_tail.AcceptGpuFrame(1, true, true));
    Check(!complete_tail.GpuFrameCoverageComplete(3));
    // A drained tail is ordered after the previously emitted frame, with no duplicate records.
    Check(!complete_tail.AcceptGpuFrame(1, true, true));
    Check(complete_tail.AcceptGpuFrame(2, true, true));
    Check(complete_tail.AcceptGpuFrame(3, true, true));
    Check(complete_tail.GpuFrameCoverageComplete(3));
    Check(complete_tail.AcceptGpuFrame(5, true, true));
    Check(!complete_tail.GpuFrameCoverageComplete(5)); // A missing frame must not be declared complete.
    Check(complete_tail.AcceptGpuFrame(4, true, true));
    Check(complete_tail.GpuFrameCoverageComplete(5));
    hs::runtime_detail::PerformanceLogState out_of_order;
    for (std::uint64_t frame = 1; frame <= 7; ++frame)
        Check(out_of_order.AcceptGpuFrame(frame, true, true));
    Check(out_of_order.AcceptGpuFrame(9, true, true));
    Check(!out_of_order.GpuFrameCoverageComplete(9));
    Check(!out_of_order.AcceptGpuFrame(9, true, true));
    Check(out_of_order.AcceptGpuFrame(8, true, true));
    Check(out_of_order.LastGpuFrame() == 9 && out_of_order.GpuFrames() == 9);
    Check(out_of_order.GpuFrameCoverageComplete(9));
    Check(!out_of_order.AcceptGpuFrame(8, true, true));
    // Reuse all bitmap slots repeatedly, including a complete reverse-order window.
    hs::runtime_detail::PerformanceLogState ring;
    for (std::uint64_t window = 0; window < 4; ++window)
    {
        const auto end = (window + 1) * 240;
        for (auto frame = end; frame > window * 240; --frame)
            Check(ring.AcceptGpuFrame(frame, true, true));
        Check(ring.GpuFrameCoverageComplete(end));
        Check(!ring.AcceptGpuFrame(end - 239, true, true));
    }
    hs::runtime_detail::PerformanceLogState overflow;
    Check(!overflow.AcceptGpuFrame(241, false, true));
    Check(!overflow.AcceptGpuFrame(241, true, false));
    Check(overflow.GpuFrameCoverageComplete(0)); // Unavailable samples do not poison the stream.
    Check(!overflow.AcceptGpuFrame(241, true, true));
    Check(!overflow.GpuFrameCoverageComplete(0));
    Check(!overflow.AcceptGpuFrame(1, true, true)); // Overflow is sticky, not a skipped sample.
    Check(overflow.GpuFrames() == 0);
    hs::runtime_detail::PerformanceLogState large_id;
    Check(!large_id.AcceptGpuFrame(std::numeric_limits<std::uint64_t>::max(), true, true));
    Check(!large_id.GpuFrameCoverageComplete(0));
    std::ostringstream output;
    DiagnosticLog disabled(output,false,"header\n");
    const std::string line(2047,'x');
    disabled.Append(line);
    Check(disabled.Finish() && disabled.Complete() && disabled.Records()==0 &&
          disabled.Dropped()==0 && output.str().empty());
    std::ostringstream captured;
    DiagnosticLog capture(captured,true,"header\n",FakeClock);
    for(int i=0;i<1000;++i) capture.Append(line+"\n");
    Check(capture.Finish() && capture.Complete());
    Check(captured.str().size()==7+1000*2048);
    Check(capture.Bytes() == captured.str().size() && capture.Io().calls > 1 &&
          capture.Io().ticks == capture.Io().calls);
    std::ostringstream published;
    DiagnosticLog ready(published,true,"header\n");
    ready.Append("first\n");
    Check(published.str().empty());
    Check(ready.Flush() && !ready.Complete() && ready.Records()==1 &&
          published.str()=="header\nfirst\n");
    ready.Append("second\n");
    Check(published.str()=="header\nfirst\n");
    Check(ready.Finish() && ready.Complete() && ready.Records()==2 &&
          published.str()=="header\nfirst\nsecond\n");
    FailedSink failure; std::ostream failing(&failure);
    DiagnosticLog failed(failing,true);
    for(int i=0;i<1000;++i) failed.Append(line);
    Check(!failed.Finish() && !failed.Complete() && failed.Failed());
    // Flush-only failure, including exception-enabled streams.
    std::ostream throwing(&failure); throwing.exceptions(std::ios::badbit | std::ios::failbit);
    DiagnosticLog close_failure(throwing,true);
    Check(!close_failure.Flush() && close_failure.Failed() && !close_failure.Complete());
    Check(!close_failure.Finish() && !close_failure.Complete());
    DiagnosticLog disabled_failure(throwing,false,"header\n");
    disabled_failure.Append(line);
    Check(disabled_failure.Flush() && !disabled_failure.Failed());
    Check(disabled_failure.Finish() && disabled_failure.Complete() &&
          !disabled_failure.Failed() && disabled_failure.Records()==0);
    std::cout<<"PASS disabled diagnostics, fixed-chunk streaming and writer failures\n";
}
