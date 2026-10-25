#include "../Source/Runtime/Private/runtime_diagnostics.hpp"
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
void Check(bool ok) { if (!ok) throw std::runtime_error("diagnostic contract"); }
std::uint64_t clock_ticks{};
std::uint64_t FakeClock() noexcept { return ++clock_ticks; }
struct FailedSink : std::streambuf
{
    std::streamsize xsputn(const char*,std::streamsize) override { return 0; }
    int sync() override { return -1; }
};
int main()
{
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
