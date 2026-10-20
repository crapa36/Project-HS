#include "../Source/Runtime/Private/runtime_diagnostics.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
void Check(bool ok) { if (!ok) throw std::runtime_error("diagnostic contract"); }
struct FailedSink : std::streambuf
{
    std::streamsize xsputn(const char*,std::streamsize) override { return 0; }
    int sync() override { return -1; }
};
int main()
{
    using hs::runtime_detail::DiagnosticLog;
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
    Check(!performance.AcceptGpuFrame(6, true, true));
    Check(performance.AcceptGpuFrame(8, true, true));
    Check(performance.GpuFrames() == 2 && performance.LastGpuFrame() == 8);
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
    std::ostringstream output;
    DiagnosticLog disabled(output,false,"header\n");
    const std::string line(2047,'x');
    disabled.Append(line);
    Check(disabled.Finish() && disabled.Complete() && disabled.Records()==0 &&
          disabled.Dropped()==0 && output.str().empty());
    std::ostringstream captured;
    DiagnosticLog capture(captured,true,"header\n");
    for(int i=0;i<1000;++i) capture.Append(line+"\n");
    Check(capture.Finish() && capture.Complete());
    Check(captured.str().size()==7+1000*2048);
    FailedSink failure; std::ostream failing(&failure);
    DiagnosticLog failed(failing,true);
    for(int i=0;i<1000;++i) failed.Append(line);
    Check(!failed.Finish() && !failed.Complete() && failed.Failed());
    // Flush-only failure, including exception-enabled streams.
    std::ostream throwing(&failure); throwing.exceptions(std::ios::badbit | std::ios::failbit);
    DiagnosticLog close_failure(throwing,true);
    Check(!close_failure.Finish() && !close_failure.Complete());
    DiagnosticLog disabled_failure(throwing,false,"header\n");
    disabled_failure.Append(line);
    Check(disabled_failure.Finish() && disabled_failure.Complete() &&
          !disabled_failure.Failed() && disabled_failure.Records()==0);
    std::cout<<"PASS disabled diagnostics, fixed-chunk streaming and writer failures\n";
}
