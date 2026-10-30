#pragma once

#include <hs/core/result.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>

namespace hs
{

class TaskSystem;

class TaskGroup
{
  public:
    ~TaskGroup();

    TaskGroup(const TaskGroup &) = delete;
    TaskGroup &operator=(const TaskGroup &) = delete;
    TaskGroup(TaskGroup &&) noexcept;
    TaskGroup &operator=(TaskGroup &&) noexcept;

    // A TaskSystem must outlive every group it creates.
    // Preparation may throw before admission. Rejected backend submission
    // returns false and records failure; accepted work completes once.
    [[nodiscard]] bool Submit(std::move_only_function<void(std::stop_token)> task);
    // Internal preparation/submission failure is recorded; Wait joins admitted chunks.
    void ParallelFor(std::size_t count, std::size_t grain,
                     std::move_only_function<void(std::size_t begin, std::size_t end)> task);
    // Seals admission and joins work. Do not wait on the current task's group
    // (including from its synchronous stop callback). Result allocation may
    // throw only after joining; cancellation callbacks may submit without waiting.
    [[nodiscard]] Result Wait();
    void RequestCancel() noexcept;
    [[nodiscard]] bool IsCancelRequested() const noexcept;

  private:
    struct State;
    explicit TaskGroup(TaskSystem &owner);
    TaskSystem *owner_{};
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

    [[nodiscard]] TaskGroup CreateTaskGroup();
    [[nodiscard]] std::size_t WorkerCount() const noexcept;

    [[nodiscard]] static std::size_t DefaultWorkerCount() noexcept;

  private:
    struct Impl;
    [[nodiscard]] bool Submit(std::shared_ptr<TaskGroup::State> state,
                              std::move_only_function<void(std::stop_token)> task);
    void ParallelFor(std::shared_ptr<TaskGroup::State> state,
                    std::size_t count, std::size_t grain,
                    std::move_only_function<void(std::size_t begin, std::size_t end)> task);

    [[nodiscard]] Result Wait(std::shared_ptr<TaskGroup::State> state);
    std::unique_ptr<Impl> impl_;

    friend class TaskGroup;
};

} // namespace hs
