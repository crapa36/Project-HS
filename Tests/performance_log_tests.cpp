#include "../Source/Runtime/Private/performance_log.hpp"

#include <iterator>
#include <stdexcept>
#include <string_view>

namespace
{
void Check(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string Read(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    Check(static_cast<bool>(stream), "open performance log output");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}

int main(int argc, char **argv)
{
    Check(argc == 2, "expected output root argument");
    const std::filesystem::path root = std::filesystem::absolute(argv[1]);
    const std::array<std::string_view, 2> passes{"first_pass", "second_pass"};
    std::array<std::uint64_t, hs::kPerformanceFrameValueCount> values{};
    const std::array<std::uint64_t, 2> gpu{17, 19};
    std::filesystem::path first, second;
    {
        hs::PerformanceLog log;
        log.Start(root, 123, "{}", passes);
        first = log.Directory();
        Check(std::filesystem::is_directory(first), "create performance log directory");
        Check(first.filename().string().find(':') == std::string::npos, "portable directory name");
        values[0] = 11;
        log.Frame(values, gpu);
        log.Flush();
        auto frames = Read(first / "frames.csv");
        const auto header_end = frames.find('\n');
        const auto row_end = frames.find('\n', header_end + 1);
        Check(header_end != std::string::npos && row_end != std::string::npos,
              "header and frame rows exist");
        const auto header = frames.substr(0, header_end);
        const auto row = frames.substr(header_end + 1, row_end - header_end - 1);
        Check(std::count(header.begin(), header.end(), ',') ==
                  std::count(row.begin(), row.end(), ','), "frame/header column counts match");
        Check(std::count(row.begin(), row.end(), ',') + 1 == values.size() + gpu.size(),
              "frame includes every CPU and GPU value");
        Check(row.starts_with("11,") && row.ends_with(",17,19"), "frame values stay ordered");
        Check(header.ends_with(",first_pass_ns,second_pass_ns"), "GPU pass names remain in header");
        log.Frame(values, gpu);
        log.Event("failure", [] { return "{\"message\":\"test\"}"; }, true);
        frames = Read(first / "frames.csv");
        Check(std::count(frames.begin(), frames.end(), '\n') == 3, "buffered frames flush");
        Check(Read(first / "events.ndjson").find("\"event\":\"failure\"") != std::string::npos,
              "failure events flush");
        hs::PerformanceLog another;
        another.Start(root, 123, "{}", passes);
        second = another.Directory();
        Check(first != second, "log directories are unique");
        Check(Read(root / "latest.txt") == second.string() + '\n', "latest log receipt is updated");
        Check(Read(first / "startup.json") == "{}\n", "startup receipt contents");
        const auto blocked = root / (first.filename().string() + "-blocked");
        Check(!std::filesystem::exists(blocked), "failed-startup fixture begins absent");
        std::ofstream(blocked) << "file blocks directory creation";
        hs::PerformanceLog invalid;
        invalid.Start(blocked / "child", 123, "{}", passes);
        Check(invalid.Directory().empty(), "failed startup does not publish a directory");
        invalid.Frame(values, gpu);
        invalid.Flush();
        std::filesystem::remove(blocked);
    }
    std::filesystem::remove_all(first);
    std::filesystem::remove_all(second);
    std::filesystem::remove(root / "latest.txt");
}
