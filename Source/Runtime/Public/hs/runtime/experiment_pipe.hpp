#pragma once

#include <hs/core/result.hpp>
#include <hs/core/types.hpp>

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hs
{

struct ExperimentPipeCommand
{
    Sequence sequence{};
    Tick target_tick{};
    std::string command;
    std::string payload_json;
};

[[nodiscard]] Result ParseExperimentPipeCommand(std::string_view json,
                                                ExperimentPipeCommand &command);
[[nodiscard]] std::string BuildExperimentPipeReply(Sequence sequence, Tick accepted_tick,
                                                   std::string_view status,
                                                   std::string_view result);

class ExperimentPipeCommandQueue
{
  public:
    [[nodiscard]] Result Handle(std::string_view request, Tick current_tick,
                                std::string &reply);
    [[nodiscard]] std::vector<ExperimentPipeCommand> TakeAccepted();
    [[nodiscard]] std::size_t AcceptedCount() const;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<Sequence, std::string> replies_;
    std::vector<ExperimentPipeCommand> accepted_;
};

using ExperimentPipeHandler =
    std::function<Result(std::string_view request, std::string &reply)>;

[[nodiscard]] Result ServeExperimentPipeOnce(std::wstring_view name,
                                             const ExperimentPipeHandler &handler);
[[nodiscard]] Result TransactExperimentPipe(std::wstring_view name,
                                            std::string_view request,
                                            std::string &reply,
                                            std::chrono::milliseconds timeout);

} // namespace hs
