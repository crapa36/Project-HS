#include <hs/jobs/task_system.hpp>

#include <taskflow/taskflow.hpp>

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace hs
{

struct TaskGroup::State
{
    // Failure publication cannot allocate, including when handling bad_alloc.
    void RecordFailureLocked(ErrorCode code, const char *message, const char *name = "") noexcept
    {
        if (failure_code == ErrorCode::None)
        {
            failure_code = code;
            std::snprintf(failure_message.data(), failure_message.size(), "%s%s%s",
                          name, *name ? ": " : "", message);
        }
    }

    void SetFailure(ErrorCode code, const char *message, const char *name = "") noexcept
    {
        bool first = false;
        {
            std::lock_guard lock(mutex);
            first = failure_code == ErrorCode::None;
            RecordFailureLocked(code, message, name);
        }
        // request_stop synchronously invokes user callbacks; never hold mutex.
        if (first) stop_source.request_stop();
    }

    struct Completion
    {
        State *state;
        ~Completion() { if (state) state->CompleteOne(); }
    };

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
    ErrorCode failure_code{ErrorCode::None};
    std::array<char, 512> failure_message{};
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
#if defined(_WIN32)
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
        wchar_t name[64]{};
        if (std::swprintf(name, std::size(name), L"HS Worker %zu", worker_id) <= 0) return;
        SetThreadDescription(GetCurrentThread(), name);
        named_by = this;
#endif
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
    const auto handle = TaskHandle{impl_->next_task_id.fetch_add(1, std::memory_order_relaxed)};
    // Construct directly: an MSVC Debug string move allocates an iterator
    // proxy inside noexcept and cannot propagate allocation failure.
    const std::string task_name(
        description.name.empty() ? std::string_view("unnamed") : description.name);
    auto shared_task =
        std::make_shared<std::move_only_function<void(std::stop_token)>>(std::move(task));

    auto run = [state, implementation = impl_.get(), shared_task, task_name]() mutable noexcept {
        TaskGroup::State::Completion completion{state.get()};
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
            state->SetFailure(ErrorCode::TaskFailed, exception.what(), task_name.c_str());
        }
        catch (...)
        {
            state->SetFailure(ErrorCode::TaskFailed, "unknown exception", task_name.c_str());
        }
    };

    {
        std::lock_guard lock(state->mutex);
        if (state->sealed)
        {
            state->RecordFailureLocked(ErrorCode::InvalidState, "Cannot submit after TaskSystem::Wait.");
            return {};
        }
        if (state->owner != nullptr && state->owner != impl_.get())
        {
            state->RecordFailureLocked(ErrorCode::InvalidState, "TaskGroup cannot span TaskSystem instances.");
            return {};
        }
        state->owner = impl_.get();
        state->pending.fetch_add(1, std::memory_order_relaxed);
    }
    TaskGroup::State::Completion reservation{state.get()};

    if (!impl_->executor)
    {
        reservation.state = nullptr;
        run();
        return handle;
    }

    try
    {
        // Pinned/patched backend throws only before publication. After success
        // the task owns completion (and may already have completed).
        impl_->executor->silent_async(task_name, std::move(run));
        reservation.state = nullptr;
    }
    catch (const std::exception &exception)
    {
        state->SetFailure(ErrorCode::TaskFailed, exception.what());
        return {};
    }
    catch (...)
    {
        state->SetFailure(ErrorCode::TaskFailed, "unknown submission exception");
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
        group.state_->SetFailure(ErrorCode::InvalidArgument,
                                "ParallelFor requires non-zero grain and a callable.");
        return;
    }
    if (count == 0)
    {
        return;
    }

    auto shared_task =
        std::make_shared<std::move_only_function<void(std::size_t, std::size_t)>>(std::move(task));
    for (std::size_t begin = 0; begin < count;)
    {
        const auto end = begin + std::min(grain, count - begin);
        const auto submitted = Submit(group, description, [shared_task, begin, end](std::stop_token stop) {
            if (!stop.stop_requested())
            {
                (*shared_task)(begin, end);
            }
        });
        if (!submitted) break;
        begin = end;
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
    if (state->failure_code != ErrorCode::None)
    {
        // Materialize Result only after all completions. Allocation failure may
        // propagate to this caller; it cannot strand pending work or an executor.
        return Result::Failure(state->failure_code, "hs_jobs", state->failure_message.data());
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
