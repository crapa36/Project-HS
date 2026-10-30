#include <hs/jobs/task_system.hpp>

#include <taskflow/taskflow.hpp>

#if defined(_WIN32) && defined(HS_DEVELOPMENT_TOOLS)
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
    void RecordFailureLocked(ErrorCode code, const char *message) noexcept
    {
        if (failure_code == ErrorCode::None)
        {
            failure_code = code;
            std::snprintf(failure_message.data(), failure_message.size(), "%s", message);
        }
    }

    void SetFailure(ErrorCode code, const char *message) noexcept
    {
        bool first = false;
        {
            std::lock_guard lock(mutex);
            first = failure_code == ErrorCode::None;
            RecordFailureLocked(code, message);
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
#if defined(_WIN32) && defined(HS_DEVELOPMENT_TOOLS)
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
};

TaskGroup::TaskGroup(TaskSystem &owner) : owner_(&owner), state_(std::make_shared<State>())
{
}
TaskGroup::~TaskGroup()
{
    assert(!state_ || state_->pending.load(std::memory_order_acquire) == 0);
}
TaskGroup::TaskGroup(TaskGroup &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), state_(std::move(other.state_))
{
}
TaskGroup &TaskGroup::operator=(TaskGroup &&other) noexcept
{
    if (this != &other)
    {
        assert(!state_ || state_->pending.load(std::memory_order_acquire) == 0);
        state_ = std::move(other.state_);
        owner_ = std::exchange(other.owner_, nullptr);
    }
    return *this;
}

bool TaskGroup::Submit(std::move_only_function<void(std::stop_token)> task)
{
    return owner_ && owner_->Submit(state_, std::move(task));
}

void TaskGroup::ParallelFor(
    std::size_t count, std::size_t grain,
    std::move_only_function<void(std::size_t begin, std::size_t end)> task)
{
    if (owner_) owner_->ParallelFor(state_, count, grain, std::move(task));
}

Result TaskGroup::Wait()
{
    if (!owner_)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_jobs", "TaskGroup is empty.");
    return owner_->Wait(state_);
}

void TaskGroup::RequestCancel() noexcept
{
    auto state = state_;
    if (state) state->stop_source.request_stop();
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

TaskGroup TaskSystem::CreateTaskGroup()
{
    return TaskGroup(*this);
}

bool TaskSystem::Submit(std::shared_ptr<TaskGroup::State> state,
                        std::move_only_function<void(std::stop_token)> task)
{
    if (!task)
    {
        return false;
    }

    const auto reject_if_unavailable = [&]() {
        if (state->sealed)
        {
            state->RecordFailureLocked(ErrorCode::InvalidState,
                                       "Cannot submit after TaskGroup::Wait.");
            return true;
        }
        return false;
    };
    {
        std::lock_guard lock(state->mutex);
        if (reject_if_unavailable()) return false;
    }

    const auto reserve = [&]() {
        std::lock_guard lock(state->mutex);
        if (reject_if_unavailable()) return false;
        state->pending.fetch_add(1, std::memory_order_relaxed);
        return true;
    };
#if defined(_WIN32) && defined(HS_DEVELOPMENT_TOOLS)
    const auto execute = [state, implementation = impl_.get()]
#else
    const auto execute = [state]
#endif
    (auto &callable) noexcept {
        TaskGroup::State::Completion completion{state.get()};
#if defined(_WIN32) && defined(HS_DEVELOPMENT_TOOLS)
        implementation->NameWorkerThread();
#endif
        try
        {
            const auto token = state->stop_source.get_token();
            if (!token.stop_requested()) callable(token);
        }
        catch (const std::exception &exception)
        {
            state->SetFailure(ErrorCode::TaskFailed, exception.what());
        }
        catch (...)
        {
            state->SetFailure(ErrorCode::TaskFailed, "unknown exception");
        }
    };

    if (!impl_->executor)
    {
        if (!reserve()) return false;
        TaskGroup::State::Completion reservation{state.get()};
        reservation.state = nullptr;
        execute(task);
        return true;
    }

    auto shared_task =
        std::make_shared<std::move_only_function<void(std::stop_token)>>(std::move(task));
    auto run = [shared_task, execute]() mutable noexcept {
        execute(*shared_task);
    };
    if (!reserve()) return false;
    TaskGroup::State::Completion reservation{state.get()};

    try
    {
        // Pinned/patched backend throws only before publication. After success
        // the task owns completion (and may already have completed).
        impl_->executor->silent_async(std::move(run));
        reservation.state = nullptr;
    }
    catch (const std::exception &exception)
    {
        state->SetFailure(ErrorCode::TaskFailed, exception.what());
        return false;
    }
    catch (...)
    {
        state->SetFailure(ErrorCode::TaskFailed, "unknown submission exception");
        return false;
    }

    return true;
}

void TaskSystem::ParallelFor(std::shared_ptr<TaskGroup::State> state,
                             std::size_t count,
                             std::size_t grain,
                             std::move_only_function<void(std::size_t, std::size_t)> task)
{
    if (grain == 0 || !task)
    {
        state->SetFailure(ErrorCode::InvalidArgument,
                          "ParallelFor requires non-zero grain and a callable.");
        return;
    }
    if (count == 0)
    {
        return;
    }

    if (!impl_->executor)
    {
        for (std::size_t begin = 0; begin < count;)
        {
            const auto end = begin + std::min(grain, count - begin);
            const auto submitted = Submit(state,
                [&task, begin, end](std::stop_token stop) {
                    if (!stop.stop_requested()) task(begin, end);
                });
            if (!submitted) break;
            begin = end;
        }
        return;
    }

    auto shared_task =
        std::make_shared<std::move_only_function<void(std::size_t, std::size_t)>>(std::move(task));
    for (std::size_t begin = 0; begin < count;)
    {
        const auto end = begin + std::min(grain, count - begin);
        const auto submitted = Submit(state, [shared_task, begin, end](std::stop_token stop) {
            if (!stop.stop_requested())
            {
                (*shared_task)(begin, end);
            }
        });
        if (!submitted) break;
        begin = end;
    }
}

Result TaskSystem::Wait(std::shared_ptr<TaskGroup::State> state)
{
    {
        std::lock_guard lock(state->mutex);
        state->sealed = true;
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
