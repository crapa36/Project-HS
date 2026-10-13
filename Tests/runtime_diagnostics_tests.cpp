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
    std::ostringstream output;
    DiagnosticLog recent(output,false,"header\n");
    const std::string line(2047,'x');
    for(int i=0;i<100000;++i)
    {
        recent.Append(line+"\n");
        Check(recent.RetainedBytes()<=DiagnosticLog::kMaximumBytes);
        Check(recent.RetainedRows()<=DiagnosticLog::kMaximumRows);
    }
    recent.Append(std::string(DiagnosticLog::kMaximumLine+1,'z'));
    Check(recent.Dropped()>0 && recent.Finish() && !recent.Complete());
    Check(output.str().starts_with("header\n") && output.str().find('z')==std::string::npos);
    std::ostringstream captured;
    DiagnosticLog capture(captured,true,"header\n");
    for(int i=0;i<1000;++i) capture.Append(line+"\n");
    Check(capture.RetainedBytes()==0 && capture.Finish() && capture.Complete());
    Check(captured.str().size()==7+1000*2048);
    FailedSink failure; std::ostream failing(&failure);
    DiagnosticLog failed(failing,true);
    for(int i=0;i<1000;++i) failed.Append(line);
    Check(!failed.Finish() && !failed.Complete() && failed.Failed() && failed.RetainedBytes()==0);
    // Flush-only failure, including exception-enabled streams.
    std::ostream throwing(&failure); throwing.exceptions(std::ios::badbit | std::ios::failbit);
    DiagnosticLog close_failure(throwing,true);
    Check(!close_failure.Finish() && !close_failure.Complete());
    std::cout<<"PASS bounded retention, streamed formatting and writer failures\n";
}
