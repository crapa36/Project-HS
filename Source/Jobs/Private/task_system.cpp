#include <hs/jobs/task_system.hpp>

#include <taskflow/taskflow.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <exception>
#include <format>
#include <mutex>
#include <thread>
#include <utility>

namespace hs
{

struct TaskGroup::State
{
    void SetFailure(Result failure)
    {
        std::lock_guard lock(mutex);
        if (result.Succeeded())
        {
            result = std::move(failure);
            stop_source.request_stop();
        }
    }

    void CompleteOne() noexcept
    {
        if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            pending.notify_all();
        }
    }

    std::atomic<std::size_t> pending{};
    std::stop_source stop_source;
    std::mutex mutex;
    Result result = Result::Success();
    void *owner{};
    bool sealed{};
};

struct TaskSystem::Impl
{
    explicit Impl(std::size_t count) : worker_count(count)
    {
        if (worker_count > 0)
        {
            executor = std::make_unique<tf::Executor>(worker_count);
        }
    }

    void NameWorkerThread() const noexcept
    {
        if (!executor || executor->this_worker() == nullptr)
        {
            return;
        }

        thread_local const Impl *named_by = nullptr;
        if (named_by == this)
        {
            return;
        }

        const auto worker_id = executor->this_worker()->id();
        const auto name = std::format(L"HS Worker {}", worker_id);
        SetThreadDescription(GetCurrentThread(), name.c_str());
        named_by = this;
    }

    std::size_t worker_count{};
    std::unique_ptr<tf::Executor> executor;
    std::atomic<std::uint64_t> next_task_id{1};
};

TaskGroup::TaskGroup() : state_(std::make_shared<State>())
{
}
TaskGroup::~TaskGroup()
{
    assert(!state_ || state_->pending.load(std::memory_order_acquire) == 0);
}
TaskGroup::TaskGroup(TaskGroup &&) noexcept = default;
TaskGroup &TaskGroup::operator=(TaskGroup &&) noexcept = default;

void TaskGroup::RequestCancel() noexcept
{
    if (state_)
    {
        state_->stop_source.request_stop();
    }
}

bool TaskGroup::IsCancelRequested() const noexcept
{
    return state_ && state_->stop_source.stop_requested();
}

TaskSystem::TaskSystem(std::size_t worker_count) : impl_(std::make_unique<Impl>(worker_count))
{
}

TaskSystem::~TaskSystem()
{
    if (impl_->executor)
    {
        impl_->executor->wait_for_all();
    }
}

TaskHandle TaskSystem::Submit(TaskGroup &group, TaskDesc description,
                              std::move_only_function<void(std::stop_token)> task)
{
    if (!group.state_ || !task)
    {
        return {};
    }

    const auto state = group.state_;
    {
        std::lock_guard lock(state->mutex);
        if (state->sealed)
        {
            if (state->result.Succeeded())
            {
                state->result = Result::Failure(ErrorCode::InvalidState, "hs_jobs",
                                                "Cannot submit after TaskSystem::Wait.");
            }
            return {};
        }
        if (state->owner != nullptr && state->owner != impl_.get())
        {
            if (state->result.Succeeded())
            {
                state->result = Result::Failure(ErrorCode::InvalidState, "hs_jobs",
                                                "TaskGroup cannot span TaskSystem instances.");
            }
            return {};
        }
        state->owner = impl_.get();
        state->pending.fetch_add(1, std::memory_order_relaxed);
    }

    const auto handle = TaskHandle{impl_->next_task_id.fetch_add(1, std::memory_order_relaxed)};
    const std::string task_name =
        description.name.empty() ? "unnamed" : std::string(description.name);
    auto shared_task =
        std::make_shared<std::move_only_function<void(std::stop_token)>>(std::move(task));

    auto run = [state, implementation = impl_.get(), shared_task, task_name]() mutable noexcept {
        implementation->NameWorkerThread();
        try
        {
            const auto token = state->stop_source.get_token();
            if (!token.stop_requested())
            {
                (*shared_task)(token);
            }
        }
        catch (const std::exception &exception)
        {
            state->SetFailure(Result::Failure(ErrorCode::TaskFailed, "hs_jobs",
                                              std::format("{}: {}", task_name, exception.what())));
        }
        catch (...)
        {
            state->SetFailure(Result::Failure(ErrorCode::TaskFailed, "hs_jobs",
                                              std::format("{}: unknown exception", task_name)));
        }
        state->CompleteOne();
    };

    if (!impl_->executor)
    {
        run();
        return handle;
    }

    try
    {
        impl_->executor->silent_async(task_name, std::move(run));
    }
    catch (const std::exception &exception)
    {
        state->SetFailure(Result::Failure(ErrorCode::TaskFailed, "hs_jobs", exception.what()));
        state->CompleteOne();
        return {};
    }

    return handle;
}

void TaskSystem::ParallelFor(TaskGroup &group, TaskDesc description, std::size_t count,
                             std::size_t grain,
                             std::move_only_function<void(std::size_t, std::size_t)> task)
{
    if (!group.state_)
    {
        return;
    }
    if (grain == 0 || !task)
    {
        group.state_->SetFailure(
            Result::Failure(ErrorCode::InvalidArgument, "hs_jobs",
                            "ParallelFor requires non-zero grain and a callable."));
        return;
    }
    if (count == 0)
    {
        return;
    }

    auto shared_task =
        std::make_shared<std::move_only_function<void(std::size_t, std::size_t)>>(std::move(task));
    for (std::size_t begin = 0; begin < count; begin += grain)
    {
        const auto end = std::min(begin + grain, count);
        (void)Submit(group, description, [shared_task, begin, end](std::stop_token stop) {
            if (!stop.stop_requested())
            {
                (*shared_task)(begin, end);
            }
        });
    }
}

Result TaskSystem::Wait(TaskGroup &group)
{
    if (!group.state_)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_jobs", "TaskGroup is empty.");
    }

    const auto state = group.state_;
    {
        std::lock_guard lock(state->mutex);
        state->sealed = true;
        if (state->owner != nullptr && state->owner != impl_.get())
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_jobs",
                                   "TaskGroup belongs to another TaskSystem.");
        }
    }

    if (impl_->executor && impl_->executor->this_worker() != nullptr)
    {
        impl_->executor->corun_until(
            [state] { return state->pending.load(std::memory_order_acquire) == 0; });
    }
    else
    {
        auto pending = state->pending.load(std::memory_order_acquire);
        while (pending != 0)
        {
            state->pending.wait(pending, std::memory_order_acquire);
            pending = state->pending.load(std::memory_order_acquire);
        }
    }

    std::lock_guard lock(state->mutex);
    if (!state->result.Succeeded())
    {
        return state->result;
    }
    if (state->stop_source.stop_requested())
    {
        return Result::Failure(ErrorCode::Cancelled, "hs_jobs", "TaskGroup cancelled.");
    }
    return Result::Success();
}

std::size_t TaskSystem::WorkerCount() const noexcept
{
    return impl_->worker_count;
}

std::size_t TaskSystem::DefaultWorkerCount() noexcept
{
    const auto logical = std::thread::hardware_concurrency();
    const auto available = logical > 3 ? logical - 3 : 1;
    return std::clamp<std::size_t>(available, 1, 8);
}

} // namespace hs
