#include <hs/jobs/task_system.hpp>
#include <taskflow/taskflow.hpp>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>
#include <crtdbg.h>
#pragma comment(lib, "Dbghelp.lib")
#endif

template <typename System>
concept AcceptsArbitraryTaskGroup = requires(System &system, hs::TaskGroup &group) {
    system.Submit(group, std::move_only_function<void(std::stop_token)>{});
};
static_assert(!AcceptsArbitraryTaskGroup<hs::TaskSystem>);

// Fail only allocations made by the submitting thread, never worker internals.
thread_local int allocation_budget = -1;
thread_local int allocation_count = 0;
void* operator new(std::size_t size)
{
    ++allocation_count;
    if (allocation_budget == 0) throw std::bad_alloc{};
    if (allocation_budget > 0) --allocation_budget;
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

void Check(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "CHECK FAILED: %s\n", message); throw std::runtime_error(message); }
}

struct ThrowingCopy
{
    ThrowingCopy() = default;
    ThrowingCopy(const ThrowingCopy&) { throw std::bad_alloc{}; }
    void operator()() const {}
};

void Backend()
{
    std::fprintf(stderr, "backend: construct\n");
    tf::Executor executor(1);
    std::fprintf(stderr, "backend: throwing copy\n");
    ThrowingCopy callable;
    bool caught = false;
    try { executor.silent_async(callable); }
    catch (const std::bad_alloc&) { caught = true; }
    std::fprintf(stderr, "backend: copy caught topology=%zu\n", executor.num_topologies());
    Check(caught, "callable construction must throw");
    Check(executor.num_topologies() == 0, "failed callable leaked topology");
    executor.wait_for_all();
    for (int budget = 0; budget < 10; ++budget)
    {
        std::atomic<int> ran{};
        bool rejected = false;
        std::fprintf(stderr, "backend: budget=%d submit\n", budget);
        allocation_budget = budget;
        try { executor.silent_async([&] { ++ran; }); }
        catch (const std::bad_alloc&) { rejected = true; }
        allocation_budget = -1;
        std::fprintf(stderr, "backend: budget=%d wait topology=%zu\n", budget, executor.num_topologies());
        executor.wait_for_all();
        Check(ran == (rejected ? 0 : 1), "backend acceptance must be atomic");
        Check(executor.num_topologies() == 0, "backend leaked topology");
    }
    // Actual Executor spill growth: stop its only consumer, fill its sole
    // external buffer, then fail each allocation before queue publication.
    std::fprintf(stderr, "backend: spill growth\n");
    tf::UnboundedWSQ<void*> sizing;
    const auto capacity = sizing.capacity();
    std::atomic<std::size_t> completed{};
    // MSVC Debug also allocates a string iterator proxy. Measure ordinary
    // node preparation, then fail each of the two *queue growth* allocations.
    allocation_count = 0;
    executor.silent_async([&] { ++completed; });
    const int node_allocations = allocation_count;
    executor.wait_for_all();
    completed = 0;
    std::atomic<bool> entered{}, release{};
    executor.silent_async([&] {
        entered.store(true); entered.notify_one();
        release.wait(false);
    });
    entered.wait(false);
    struct ReleaseWorker
    {
        std::atomic<bool>& release;
        tf::Executor& executor;
        ~ReleaseWorker() {
            release.store(true); release.notify_one(); executor.wait_for_all();
        }
    } release_worker{release, executor};
    std::fprintf(stderr, "backend: worker blocked capacity=%zu\n", capacity);
    for (std::size_t i = 0; i < capacity; ++i)
    {
        if (i % 256 == 0) std::fprintf(stderr, "backend: filling spill %zu\n", i);
        executor.silent_async([&] { ++completed; });
    }
    std::fprintf(stderr, "backend: spill full\n");
    for (int budget : {node_allocations, node_allocations + 1})
    {
        std::fprintf(stderr, "backend: spill budget=%d submit\n", budget);
        bool rejected = false;
        allocation_budget = budget;
        try { executor.silent_async([&] { ++completed; }); }
        catch (const std::bad_alloc&) { rejected = true; }
        allocation_budget = -1;
        std::fprintf(stderr, "backend: spill budget=%d rejected=%d topology=%zu\n", budget, rejected, executor.num_topologies());
        Check(rejected, "expected spill growth allocation failure");
        Check(executor.num_topologies() == capacity + 1, "spill failure leaked topology");
    }
    std::fprintf(stderr, "backend: release\n");
    release.store(true); release.notify_one();
    executor.wait_for_all();
    Check(completed == capacity, "rejected spill work ran");
    // Exercise both allocation sites in the actual spill-queue resize path.
    for (int budget = 0; budget < 4; ++budget)
    {
        std::fprintf(stderr, "backend: queue budget=%d\n", budget);
        tf::UnboundedWSQ<int*> queue(1);
        int values[3]{};
        queue.push(&values[0]); queue.push(&values[1]);
        bool rejected = false;
        allocation_budget = budget;
        try { queue.push(&values[2]); }
        catch (const std::bad_alloc&) { rejected = true; }
        allocation_budget = -1;
        if (!rejected) Check(queue.pop() == &values[2], "grown queue value");
        Check(queue.pop() == &values[1], "resize preserved second value");
        Check(queue.pop() == &values[0], "resize preserved first value");
    }
}

void Engine()
{
    static_assert(!std::is_default_constructible_v<hs::TaskGroup>);
    for (std::size_t workers : {0u, 1u, 4u})
    {
        std::fprintf(stderr, "engine: workers=%zu construct\n", workers);
        hs::TaskSystem tasks(workers);
        auto sealed = tasks.CreateTaskGroup();
        Check(sealed.Wait().Succeeded(), "empty group joins");
        allocation_count = 0;
        allocation_budget = 0;
        const auto sealed_accepted = sealed.Submit([](std::stop_token) {});
        allocation_budget = -1;
        Check(!sealed_accepted && allocation_count == 0,
              "sealed rejection prepared submission state");
        Check(!sealed.Submit([](std::stop_token) {}),
              "sealed group rejects submission");
        Check(sealed.Wait().Code() == hs::ErrorCode::InvalidState,
              "sealed rejection records failure");

        auto owned = tasks.CreateTaskGroup();
        Check(owned.Submit([](std::stop_token) {}),
              "valid submission is accepted");
        Check(owned.Wait().Succeeded(), "creator-bound group joins its accepted work");

        auto empty = tasks.CreateTaskGroup();
        hs::TaskGroup moved(std::move(empty));
        Check(!empty.Submit([](std::stop_token) {}),
              "moved-from group rejects submission");
        Check(!moved.Submit({}),
              "empty callable is rejected");
        Check(moved.Wait().Succeeded(), "rejected empty callable admits no work");
        for (int budget = 0; budget < 14; ++budget)
        {
            auto group = tasks.CreateTaskGroup();
            std::atomic<int> ran{};
            bool accepted{};
            bool threw = false;
            std::fprintf(stderr, "engine: workers=%zu budget=%d submit\n", workers, budget);
            allocation_budget = budget;
            try { accepted = group.Submit([&](std::stop_token) { ++ran; }); }
            catch (const std::bad_alloc&) { threw = true; }
            allocation_budget = -1;
            std::fprintf(stderr, "engine: workers=%zu budget=%d wait accepted=%d threw=%d\n", workers, budget, accepted, threw);
            const auto result = group.Wait();
            Check(ran == (accepted ? 1 : 0), "engine acceptance/completion mismatch");
            Check(!threw || ran == 0, "preparation failure published work");
            Check(result.Succeeded() || result.Code() == hs::ErrorCode::TaskFailed,
                  "unexpected submission result");
        }
        std::fprintf(stderr, "engine: workers=%zu persistent OOM\n", workers);
        auto failing = tasks.CreateTaskGroup();
        (void)failing.Submit([](std::stop_token) {
            allocation_budget = 0;
            throw 42;
        });
        // Restore the submitter (worker zero runs inline); other workers reset below.
        allocation_budget = -1;
        std::fprintf(stderr, "engine: workers=%zu persistent OOM wait\n", workers);
        Check(failing.Wait().Code() == hs::ErrorCode::TaskFailed,
              "non-allocating failure recording");
    }
    hs::TaskSystem tasks(0);
    std::fprintf(stderr, "engine: overflow\n");
    auto large = tasks.CreateTaskGroup();
    std::size_t chunks = 0;
    const auto maximum = std::numeric_limits<std::size_t>::max();
    large.ParallelFor(maximum, maximum - 1,
        [&](std::size_t begin, std::size_t end) {
            Check(end > begin, "ParallelFor wrapped"); ++chunks;
        });
    Check(large.Wait().Succeeded() && chunks == 2, "overflow-safe partition");
}

void Reentry()
{
    hs::TaskSystem tasks(0);
    auto group = tasks.CreateTaskGroup();
    bool called = false;
    (void)group.Submit([&](std::stop_token token) {
        std::stop_callback callback(token, [&] {
            called = true;
            (void)group.Submit([](std::stop_token) {});
        });
        // Nested inline failure requests cancellation while callback is registered.
        (void)group.Submit([](std::stop_token) { throw 7; });
    });
    Check(called, "stop callback not called");
    Check(group.Wait().Code() == hs::ErrorCode::TaskFailed, "first failure lost");
}

int main(int argc, char** argv)
{
#if defined(_WIN32)
    // CI must report a fatal CRT/noexcept failure instead of waiting for a dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::set_terminate([] {
        allocation_budget = -1; // Diagnostic symbol lookup only; the test fails.
        std::fputs("TERMINATE during failure injection\n", stderr);
        auto process = GetCurrentProcess();
        SymInitialize(process, nullptr, TRUE);
        void* frames[32]{};
        const auto count = CaptureStackBackTrace(0, 32, frames, nullptr);
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        for (USHORT i = 0; i < count; ++i)
            if (SymFromAddr(process, reinterpret_cast<DWORD64>(frames[i]), nullptr, symbol))
                std::fprintf(stderr, "  %s\n", symbol->Name);
        std::fflush(stderr);
        std::_Exit(3);
    });
#endif
    try
    {
        const std::string_view mode = argc > 1 ? argv[1] : "engine";
        if (mode == "backend") Backend();
        else if (mode == "reentry") Reentry();
        else Engine();
        std::cout << "PASS " << mode << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        allocation_budget = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
