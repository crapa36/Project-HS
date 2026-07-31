#pragma once

#include <hs/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string_view>

namespace hs
{

enum class TaskPriority : std::uint8_t
{
    Normal,
};

struct TaskDesc
{
    std::string_view name;
    TaskPriority priority{TaskPriority::Normal};
};

struct TaskHandle
{
    std::uint64_t value{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value != 0;
    }
};

class TaskSystem;

class TaskGroup
{
  public:
    TaskGroup();
    ~TaskGroup();

    TaskGroup(const TaskGroup &) = delete;
    TaskGroup &operator=(const TaskGroup &) = delete;
    TaskGroup(TaskGroup &&) noexcept;
    TaskGroup &operator=(TaskGroup &&) noexcept;

    void RequestCancel() noexcept;
    [[nodiscard]] bool IsCancelRequested() const noexcept;

  private:
    struct State;
    std::shared_ptr<State> state_;

    friend class TaskSystem;
};

class TaskSystem
{
  public:
    explicit TaskSystem(std::size_t worker_count = DefaultWorkerCount());
    ~TaskSystem();

    TaskSystem(const TaskSystem &) = delete;
    TaskSystem &operator=(const TaskSystem &) = delete;

    [[nodiscard]] TaskHandle Submit(TaskGroup &group, TaskDesc description,
                                    std::move_only_function<void(std::stop_token)> task);

    void ParallelFor(TaskGroup &group, TaskDesc description, std::size_t count, std::size_t grain,
                     std::move_only_function<void(std::size_t begin, std::size_t end)> task);

    [[nodiscard]] Result Wait(TaskGroup &group);
    [[nodiscard]] std::size_t WorkerCount() const noexcept;

    [[nodiscard]] static std::size_t DefaultWorkerCount() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hs
