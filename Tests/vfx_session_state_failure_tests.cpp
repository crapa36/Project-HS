#include "vfx_session_state.hpp"

#include <array>
#include <cstdlib>
#include <limits>
#include <new>
#include <iostream>
#include <stdexcept>
#include <utility>

thread_local int allocation_budget = -1;
void *operator new(std::size_t size)
{
    if (allocation_budget == 0) throw std::bad_alloc{};
    if (allocation_budget > 0) --allocation_budget;
    if (void *memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

namespace
{
class ScopedAllocationFailure
{
public:
    ScopedAllocationFailure() noexcept : previous_(allocation_budget) { allocation_budget = 0; }
    ~ScopedAllocationFailure() { allocation_budget = previous_; }
    ScopedAllocationFailure(const ScopedAllocationFailure &) = delete;
    ScopedAllocationFailure &operator=(const ScopedAllocationFailure &) = delete;

private:
    int previous_;
};

void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
}

void RunVfxSessionStateAllocationFailureTests()
{
    using State = hs::runtime_detail::VfxSessionState;
    using Store = hs::runtime_detail::ActiveEffectStore;
    State state;
    state.Synchronize(7);
    hs::PresentationEvent raw{};
    raw.session_id = 7;
    raw.tick = 1;
    raw.sequence = 91;
    Check(state.raw_events.Enqueue(raw) && state.raw_events.Prepare(1, 7).events.size() == 1,
          "surface failure fixture did not retain its raw activation");
    hs::VfxEventInput input{};
    input.sequence = raw.sequence;
    const auto surface = static_cast<std::uint16_t>(1u << static_cast<unsigned>(Store::Profile::EntitySurface));
    const std::array request{Store::AppendRequest{&input, surface}};
    Store::PreparedAppendBatch prepared;
    Check(state.active_effects.PrepareAppendBatch(request, prepared),
          "surface profile preparation failed");
    hs::runtime_detail::DecodedVfxState births;
    bool failed{};
    {
        ScopedAllocationFailure fail_next_allocation;
        try { (void)state.CommitPreparedActivation(births, prepared, true); }
        catch (const std::bad_alloc &) { failed = true; }
    }
    Check(failed && state.active_effects.Size() == 0 &&
          state.raw_events.Prepare(1, 7).events.size() == 1 &&
          state.decoded.entity_surface_captures.empty(),
          "surface capture allocation failure published profiles or acknowledged raw input");
    state.ReserveSurfaceCaptures(prepared.AdditionalProfileRows(Store::Profile::EntitySurface));
    bool committed{};
    {
        ScopedAllocationFailure warmed_commit;
        committed = state.CommitPreparedActivation(births, prepared, true);
    }
    Check(committed &&
          state.active_effects.ProfileEvents(Store::Profile::EntitySurface).size() == 1 &&
          state.raw_events.Prepare(1, 7).events.empty(),
          "surface activation did not recover and acknowledge after reservation");
}

int main()
{
    try
    {
        RunVfxSessionStateAllocationFailureTests();
        std::cout << "PASS vfx-session-state-allocation-failure\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        allocation_budget = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
