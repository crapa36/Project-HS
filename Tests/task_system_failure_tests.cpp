#include <hs/jobs/task_system.hpp>
#include <taskflow/taskflow.hpp>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

// Fail only allocations made by the submitting thread, never worker internals.
thread_local int allocation_budget = -1;
void* operator new(std::size_t size)
{
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
    if (!condition) throw std::runtime_error(message);
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
        const std::string name(120, 'n');
        std::fprintf(stderr, "backend: budget=%d submit\n", budget);
        allocation_budget = budget;
        try { executor.silent_async(name, [&] { ++ran; }); }
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
    std::atomic<bool> entered{}, release{};
    executor.silent_async([&] {
        entered.store(true); entered.notify_one();
        release.wait(false);
    });
    entered.wait(false);
    std::fprintf(stderr, "backend: worker blocked capacity=%zu\n", capacity);
    std::atomic<std::size_t> completed{};
    for (std::size_t i = 0; i < capacity; ++i)
        executor.silent_async([&] { ++completed; });
    for (int budget : {1, 2})
    {
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
    for (std::size_t workers : {0u, 1u, 4u})
    {
        std::fprintf(stderr, "engine: workers=%zu construct\n", workers);
        hs::TaskSystem tasks(workers);
        for (int budget = 0; budget < 14; ++budget)
        {
            hs::TaskGroup group;
            std::atomic<int> ran{};
            const std::string name(120, 'n');
            hs::TaskHandle handle;
            bool threw = false;
            std::fprintf(stderr, "engine: workers=%zu budget=%d submit\n", workers, budget);
            allocation_budget = budget;
            try { handle = tasks.Submit(group, {name}, [&](std::stop_token) { ++ran; }); }
            catch (const std::bad_alloc&) { threw = true; }
            allocation_budget = -1;
            std::fprintf(stderr, "engine: workers=%zu budget=%d wait handle=%d threw=%d\n", workers, budget, static_cast<bool>(handle), threw);
            const auto result = tasks.Wait(group);
            Check(ran == (handle ? 1 : 0), "engine acceptance/completion mismatch");
            Check(!threw || ran == 0, "preparation failure published work");
            Check(result.Succeeded() || result.Code() == hs::ErrorCode::TaskFailed,
                  "unexpected submission result");
        }
        std::fprintf(stderr, "engine: workers=%zu persistent OOM\n", workers);
        hs::TaskGroup failing;
        (void)tasks.Submit(failing, {"failure while allocator unavailable"}, [](std::stop_token) {
            allocation_budget = 0;
            throw 42;
        });
        // Restore the submitter (worker zero runs inline); other workers reset below.
        allocation_budget = -1;
        std::fprintf(stderr, "engine: workers=%zu persistent OOM wait\n", workers);
        Check(tasks.Wait(failing).Code() == hs::ErrorCode::TaskFailed,
              "non-allocating failure recording");
    }
    hs::TaskSystem tasks(0);
    std::fprintf(stderr, "engine: overflow\n");
    hs::TaskGroup large;
    std::size_t chunks = 0;
    const auto maximum = std::numeric_limits<std::size_t>::max();
    tasks.ParallelFor(large, {"overflow"}, maximum, maximum - 1,
        [&](std::size_t begin, std::size_t end) {
            Check(end > begin, "ParallelFor wrapped"); ++chunks;
        });
    Check(tasks.Wait(large).Succeeded() && chunks == 2, "overflow-safe partition");
}

void Reentry()
{
    hs::TaskSystem tasks(0);
    hs::TaskGroup group;
    bool called = false;
    (void)tasks.Submit(group, {"stop callback"}, [&](std::stop_token token) {
        std::stop_callback callback(token, [&] {
            called = true;
            (void)tasks.Submit(group, {"cancelled nested task"}, [](std::stop_token) {});
        });
        // Nested inline failure requests cancellation while callback is registered.
        (void)tasks.Submit(group, {"nested failure"}, [](std::stop_token) { throw 7; });
    });
    Check(called, "stop callback not called");
    Check(tasks.Wait(group).Code() == hs::ErrorCode::TaskFailed, "first failure lost");
}

int main(int argc, char** argv)
{
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
