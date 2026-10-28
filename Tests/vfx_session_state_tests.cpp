#include "vfx_session_state.hpp"
#include "vfx_birth_admission.hpp"
#include "vfx_birth_retirement.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>

void TestVfxProfileAdmission();

thread_local int allocation_budget = -1;
thread_local std::size_t allocation_size_limit = std::numeric_limits<std::size_t>::max();
void *operator new(std::size_t size)
{
    if (allocation_budget == 0 || size > allocation_size_limit) throw std::bad_alloc{};
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
void Check(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

void CheckRetainedCapacitySaturation()
{
    using hs::runtime_detail::vfx_retained_size::Add;
    using hs::runtime_detail::vfx_retained_size::Multiply;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    Check(Add(0, 0) == 0 && Add(maximum, 0) == maximum && Add(maximum, 1) == maximum,
        "retained capacity addition must saturate at size_t maximum");
    Check(Multiply(0, maximum) == 0 && Multiply(maximum, 1) == maximum &&
          Multiply(maximum, 2) == maximum,
        "retained capacity multiplication must saturate at size_t maximum");
    Check(Add(maximum - 1, Add(1, 1)) == maximum,
        "nested retained capacity sums must saturate");
    struct TableShape
    {
        using value_type=std::pair<const std::uint64_t,std::uint32_t>;
        std::size_t buckets{},rows{};
        std::size_t bucket_count() const noexcept { return buckets; }
        std::size_t size() const noexcept { return rows; }
    };
    constexpr auto node_bytes=sizeof(TableShape::value_type)+3*sizeof(void *);
    Check(hs::runtime_detail::vfx_retained_size::HashTable(TableShape{8,2})==
        16*sizeof(void *)+3*node_bytes,
        "hash capacity omitted MSVC bucket endpoints or the list sentinel");
    Check(hs::runtime_detail::vfx_retained_size::HashTable(TableShape{maximum,0})==maximum&&
        hs::runtime_detail::vfx_retained_size::HashTable(TableShape{0,maximum})==maximum,
        "hash bucket/node capacity arithmetic must saturate");
}

void CheckVfxAppendReservation()
{
    using hs::runtime_detail::vfx_retained_size::ReserveAppend;
    std::vector<int> rows{17};
    const auto original_capacity = rows.capacity();
    const auto *original_data = rows.data();
    bool failed{};
    allocation_budget = 0;
    try { ReserveAppend(rows, rows.capacity()); }
    catch (const std::bad_alloc &) { failed = true; }
    allocation_budget = -1;
    Check(failed && rows.size() == 1 && rows.front() == 17 &&
        rows.capacity() == original_capacity && rows.data() == original_data,
        "failed VFX reservation changed the existing output prefix");

    ReserveAppend(rows, 7);
    const auto *reserved_data = rows.data();
    const auto reserved_capacity = rows.capacity();
    allocation_budget = 0;
    ReserveAppend(rows, 0);
    ReserveAppend(rows, 7);
    for (int i = 0; i < 7; ++i) rows.push_back(i);
    allocation_budget = -1;
    Check(rows.size() == 8 && rows.front() == 17 && rows.back() == 6 &&
        rows.capacity() == reserved_capacity && rows.data() == reserved_data,
        "reserved VFX output allocated or changed its existing prefix during generation");

    bool overflow{};
    try { ReserveAppend(rows, rows.max_size()); }
    catch (const std::length_error &) { overflow = true; }
    Check(overflow && rows.size() == 8 && rows.front() == 17 && rows.back() == 6 &&
        rows.capacity() == reserved_capacity && rows.data() == reserved_data,
        "overflowing VFX reservation changed the existing output prefix");
}

void CheckVfxSharedAppendBudget()
{
    using hs::runtime_detail::vfx_retained_size::AppendRowsWithinBudget;
    const std::array<std::size_t, 3> unequal{0, 10, 20};
    Check(AppendRowsWithinBudget<int>(20, 100, 100 + 12 * sizeof(int), unequal) == 11,
        "shared VFX budget double-charged reusable rows or missed a destination's growth");
    const std::array<std::size_t, 2> reusable{4, 9};
    Check(AppendRowsWithinBudget<int>(10, 100, 100, reusable) == 4 &&
          AppendRowsWithinBudget<int>(10, 101, 100, reusable) == 4,
        "a full VFX budget lost already owned rows or admitted new allocation");
    const std::array<std::size_t, 2> transferred{0, 100};
    Check(AppendRowsWithinBudget<int>(100, 0, 50 * sizeof(int), transferred) == 50 &&
          AppendRowsWithinBudget<int>(3, 0, 3 * sizeof(int), transferred) == 3,
        "VFX ownership transfer was charged as another retained copy");
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    const std::array<std::size_t, 2> empty{0, 0};
    const std::array<std::size_t, 2> full{maximum, maximum};
    Check(AppendRowsWithinBudget<unsigned char>(maximum, 0, maximum, empty) == maximum / 2 &&
          AppendRowsWithinBudget<unsigned char>(maximum, maximum, 0, full) == maximum &&
          AppendRowsWithinBudget<int>(10, 0, 100, {}) == 0,
        "shared VFX append search overflowed or admitted an unspecified destination");
}

void CheckVfxBirthRowSelection()
{
    using hs::runtime_detail::vfx_retained_size::BirthRequest;
    using hs::runtime_detail::vfx_retained_size::BirthStorage;
    using hs::runtime_detail::vfx_retained_size::SelectBirthRows;
    std::array<BirthStorage, 3> all_fit_storage{{
        {4, {1, 0, 2}, 3}, {12, {0, 4, 1}, 2}, {8, {2, 2, 2}, 3}}};
    std::size_t particle{}, line{}, flash{};
    std::array<BirthRequest, 3> all_fit_requests{{
        {2, &particle, 1, 0}, {2, &line, 3, 1}, {2, &flash, 2, 2}}};
    std::vector<std::size_t> order{77};
    Check(SelectBirthRows(all_fit_requests, all_fit_storage, 100, 136, order) == 36 &&
          particle == 2 && line == 2 && flash == 2 && order == std::vector<std::size_t>{77} &&
          all_fit_storage[0].unused_rows == std::array<std::size_t, 3>{0, 0, 0} &&
          all_fit_storage[1].unused_rows[1] == 2 &&
          all_fit_storage[2].unused_rows == std::array<std::size_t, 3>{0, 0, 0},
        "all-fit birth selection miscounted typed storage growth or touched sort scratch");

    std::array<BirthStorage, 2> partial_storage{{
        {4, {0, 2, 0}, 3}, {8, {1, 0, 0}, 2}}};
    std::array<std::size_t, 3> partial_admitted{};
    std::array<BirthRequest, 3> partial_requests{{
        {3, &partial_admitted[0], 5, 0},
        {2, &partial_admitted[1], 5, 1},
        {2, &partial_admitted[2], 9, 0}}};
    order.clear(); order.reserve(partial_requests.size());
    Check(SelectBirthRows(partial_requests, partial_storage, 10, 34, order) == 24 &&
          partial_admitted == std::array<std::size_t, 3>{0, 1, 2} &&
          order == std::vector<std::size_t>{2, 0, 1},
        "partial birth selection lost importance ordering, original-index ties, or per-kind free rows");

    std::array<BirthStorage, 1> reused_storage{{{2, {3, 2, 4}, 3}}};
    std::size_t reused{};
    std::array<BirthRequest, 1> reused_request{{{2, &reused, 0, 0}}};
    order = {55};
    Check(SelectBirthRows(reused_request, reused_storage, 50, 0, order) == 0 &&
          reused == 2 && order == std::vector<std::size_t>{55} &&
          reused_storage[0].unused_rows == std::array<std::size_t, 3>{1, 0, 2},
        "zero remaining budget failed to use already-free destination rows");

    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    std::array<BirthStorage, 1> overflow_storage{{{1, {0, 0, 0}, 1}}};
    std::array<std::size_t, 2> overflow_admitted{};
    std::array<BirthRequest, 2> overflow_requests{{
        {maximum, &overflow_admitted[0], 2, 0}, {1, &overflow_admitted[1], 1, 0}}};
    order.clear(); order.reserve(overflow_requests.size());
    Check(SelectBirthRows(overflow_requests, overflow_storage, 0, maximum, order) == maximum &&
          overflow_admitted == std::array<std::size_t, 2>{maximum, 0},
        "overflowing all-fit fanout was accepted as a saturated total");

    std::array<BirthStorage, 1> cost_overflow_storage{{{maximum, {0, 0, 0}, 2}}};
    std::size_t cost_overflow_admitted{};
    std::array<BirthRequest, 1> cost_overflow_request{{{1, &cost_overflow_admitted, 1, 0}}};
    order.clear(); order.reserve(cost_overflow_request.size());
    Check(SelectBirthRows(cost_overflow_request, cost_overflow_storage, 0, maximum, order) == 0 &&
          cost_overflow_admitted == 0,
        "birth row cost multiplication overflow admitted a row outside the budget");

    std::array<BirthStorage, 1> invalid_storage{{{4, {0, 0, 0}, 1}}};
    std::size_t invalid_admitted{};
    std::array<BirthRequest, 1> invalid_request{{{1, &invalid_admitted, 1, 1}}};
    bool invalid_rejected{};
    try { (void)SelectBirthRows(invalid_request, invalid_storage, 0, 100, order); }
    catch (const std::logic_error &) { invalid_rejected = true; }
    Check(invalid_rejected, "invalid VFX birth storage index was silently accepted");
    invalid_request[0] = {1, nullptr, 1, 0};
    invalid_rejected = false;
    try { (void)SelectBirthRows(invalid_request, invalid_storage, 0, 100, order); }
    catch (const std::logic_error &) { invalid_rejected = true; }
    Check(invalid_rejected, "missing VFX birth admission result was silently accepted");
}

void CheckVfxBirthAdmissionBoundary()
{
    using hs::runtime_detail::vfx_birth_admission::ReserveAndSelectBirthRows;
    using hs::runtime_detail::vfx_retained_size::BirthRequest;
    std::array<std::size_t, 3> required_rows{2, 1, 1};
    std::vector<int> prepared_particles, retained_particles, frozen_particles;
    std::vector<int> prepared_lines, retained_lines, frozen_lines;
    std::vector<int> prepared_flashes, retained_flashes(2), visible_flashes;
    std::vector<std::size_t> order;
    std::vector<BirthRequest> requests;
    bool measured{};
    ReserveAndSelectBirthRows(required_rows, requests, order,
        [&]() noexcept { measured = true; return std::size_t{0}; },
        prepared_particles, retained_particles, frozen_particles,
        prepared_lines, retained_lines, frozen_lines,
        prepared_flashes, retained_flashes, visible_flashes, 3, 100);
    Check(!measured && prepared_particles.capacity() >= 2 && prepared_lines.capacity() >= 1 &&
          prepared_flashes.capacity() >= 1 && visible_flashes.capacity() >= 6,
        "required birth reserve omitted retained rows or the required visible-flash suffix");

    std::array<std::size_t, 3> limited_rows{};
    std::vector<int> limited_particles, empty_retained_particles, empty_frozen_particles;
    std::vector<int> limited_lines, empty_retained_lines, empty_frozen_lines;
    std::vector<int> limited_flashes, empty_active_flashes, limited_visible_flashes;
    std::size_t admitted{};
    std::array<BirthRequest, 1> limited_requests{{{2, &admitted, 7, 0}}};
    std::vector<std::size_t> limited_order;
    bool measured_after_order{};
    ReserveAndSelectBirthRows(limited_rows, limited_requests, limited_order,
        [&]() noexcept {
            measured_after_order = limited_order.capacity() >= limited_requests.size();
            return std::size_t{0};
        }, limited_particles, empty_retained_particles, empty_frozen_particles,
        limited_lines, empty_retained_lines, empty_frozen_lines,
        limited_flashes, empty_active_flashes, limited_visible_flashes, 1, 2 * sizeof(int));
    Check(measured_after_order && admitted == 1 && limited_rows[0] == 1,
        "birth admission measured before its order scratch or exceeded the shared byte limit");

    std::array<std::size_t, 3> reused_rows{};
    std::vector<int> reused_particles, reused_retained_particles, reused_frozen_particles;
    std::vector<int> reused_lines, reused_retained_lines, reused_frozen_lines;
    std::vector<int> reused_flashes, reused_active_flashes, reused_visible_flashes;
    reused_particles.reserve(2);
    reused_frozen_particles.reserve(2);
    std::size_t reused_admitted{};
    std::array<BirthRequest, 1> reused_requests{{{2, &reused_admitted, 1, 0}}};
    std::vector<std::size_t> reused_order;
    ReserveAndSelectBirthRows(reused_rows, reused_requests, reused_order,
        [&]() noexcept { return std::size_t{100}; },
        reused_particles, reused_retained_particles, reused_frozen_particles,
        reused_lines, reused_retained_lines, reused_frozen_lines,
        reused_flashes, reused_active_flashes, reused_visible_flashes, 0, 100);
    Check(reused_admitted == 2 && reused_rows[0] == 2,
        "empty retained destinations failed to reuse already-reserved prepared and native rows");

    // With a retained prefix, AppendPreparedBirths clears the prepared buffer;
    // its capacity can then be swapped into a later mixed-prefix native batch.
    std::array<std::size_t, 3> retained_rows{2, 0, 0};
    std::vector<int> reusable_prepared_particles, nonempty_retained_particles, no_frozen_particles;
    reusable_prepared_particles.reserve(2);
    nonempty_retained_particles.push_back(7);
    std::vector<int> reusable_prepared_lines, no_retained_lines, no_frozen_lines;
    std::vector<int> reusable_prepared_flashes, no_retained_flashes, no_frozen_flashes, no_visible_flashes;
    std::size_t retained_admitted{};
    std::array<BirthRequest, 1> retained_requests{{{1, &retained_admitted, 1, 0}}};
    std::vector<std::size_t> retained_order;
    ReserveAndSelectBirthRows(retained_rows, retained_requests, retained_order,
        [&]() noexcept { return std::size_t{0}; },
        reusable_prepared_particles, nonempty_retained_particles, no_frozen_particles,
        reusable_prepared_lines, no_retained_lines, no_frozen_lines,
        reusable_prepared_flashes, no_retained_flashes, no_frozen_flashes,
        no_visible_flashes, 0, 4 * sizeof(int));
    Check(retained_admitted == 1 && retained_rows[0] == 3,
        "prepared capacity reusable after append was charged again as native freeze growth");
}

void CheckVisibleFlashRetirement()
{
    hs::runtime_detail::DecodedVfxState state;
    const auto flash = [](std::uint32_t seed, hs::Tick tick, float delay, float lifetime) {
        hs::VfxFlashSpawnInput row;
        row.stable_seed = seed; row.event_tick = tick;
        row.delay = delay; row.lifetime = lifetime;
        return row;
    };
    state.flashes = {flash(1, 0, 0, 1), flash(2, 0, 0, 4),
        flash(3, 0, 3, 1), flash(4, 180, 0, 1)};
    std::vector<hs::VfxFlashSpawnInput> visible;
    state.AppendVisibleFlashes(120, visible);
    Check(state.flashes.size() == 3 && visible.size() == 1 &&
          visible[0].stable_seed == 2 && visible[0].normalized_age == 0.5f,
        "flash retirement must free expired rows before birth admission and retain future/delayed rows");
    const auto retained = state.flashes.size();
    state.flashes.push_back(flash(5, 60, 0, 2));
    state.flashes.push_back(flash(6, 0, 0, 1));
    state.AppendVisibleFlashes(120, visible, retained);
    Check(state.flashes.size() == 4 && visible.size() == 2 &&
          visible[0].stable_seed == 2 && visible[1].stable_seed == 5 &&
          visible[1].normalized_age == 0.5f,
        "new flash suffix must retire late expired births without duplicating visible retained rows");
    visible.clear();
    state.AppendVisibleFlashes(240, visible);
    Check(state.flashes.empty() && visible.empty(), "flash endpoint retirement changed");
}

void CheckVfxBirthRetirement()
{
    using hs::runtime_detail::VfxGroundBirthSuppressionFacts;
    using hs::runtime_detail::vfx_birth_retirement::RetireCompatibilityBirthRows;
    using hs::runtime_detail::vfx_birth_retirement::RetireGroundSuppressedBirthRows;
    std::vector<hs::ParticleSpawnCommand> rows(7);
    rows[0].sequence = 10; rows[0].count = 4; rows[0].renderer = hs::VfxRenderer::Sprite;
    rows[1].sequence = 10; rows[1].count = 5; rows[1].authored_ballistic = true;
    rows[2].sequence = 30; rows[2].count = 6; rows[2].authored_ballistic = true;
    rows[3].sequence = 20; rows[3].count = 7; rows[3].renderer = hs::VfxRenderer::Mesh;
    rows[3].primitive = hs::VfxPrimitive::Arrow;
    rows[4].sequence = 21; rows[4].count = 8; rows[4].renderer = hs::VfxRenderer::Mesh;
    rows[4].primitive = hs::VfxPrimitive::ShockShell;
    rows[5].sequence = 22; rows[5].count = 9; rows[5].renderer = hs::VfxRenderer::Mesh;
    rows[5].primitive = hs::VfxPrimitive::ShockShell;
    rows[6].sequence = 20; rows[6].count = 0; rows[6].renderer = hs::VfxRenderer::Mesh;
    rows[6].primitive = hs::VfxPrimitive::Arrow;
    const std::array replaced_particles{hs::Sequence{10}, hs::Sequence{30}};
    const std::array owned_status{hs::Sequence{30}};
    const std::array replaced_meshes{hs::Sequence{20}, hs::Sequence{21}, hs::Sequence{22}};
    const std::array replaced_shells{hs::Sequence{22}};
    auto retained_prefix = RetireCompatibilityBirthRows(
        rows, rows.size(), replaced_particles, owned_status, replaced_meshes, replaced_shells);
    Check(retained_prefix == 2 && rows.size() == 2 && rows[0].sequence == 10 &&
          rows[0].count == 5 && rows[0].authored_ballistic && rows[1].sequence == 21 && rows[1].count == 8,
        "combined compatibility retirement changed typed ballistic or ShockShell replacement behavior");

    // Persistent shards publish after compatibility retirement, then the ground facts
    // intentionally inspect the enlarged retained queue.
    hs::ParticleSpawnCommand status_shard;
    status_shard.sequence = 40; status_shard.count = 3; status_shard.authored_ballistic = true;
    status_shard.renderer = hs::VfxRenderer::Mesh;
    rows.push_back(status_shard);
    hs::ParticleSpawnCommand ground_ring;
    ground_ring.sequence = 30; ground_ring.count = 2;
    ground_ring.renderer = hs::VfxRenderer::Ground; ground_ring.primitive = hs::VfxPrimitive::Ring;
    rows.push_back(ground_ring);
    const std::array facts{VfxGroundBirthSuppressionFacts{30, false, true, false, false},
        VfxGroundBirthSuppressionFacts{40, true, false, false, false}};
    retained_prefix = RetireGroundSuppressedBirthRows(rows, retained_prefix, facts);
    Check(retained_prefix == 2 && rows.size() == 2 && rows[0].count == 5 && rows[1].count == 8,
        "ground retirement did not include post-commit status births or changed unrelated rows");
    const std::array<VfxGroundBirthSuppressionFacts, 0> no_facts{};
    const auto size_before_empty = rows.size();
    retained_prefix = RetireGroundSuppressedBirthRows(rows, retained_prefix, no_facts);
    Check(rows.size() == size_before_empty && retained_prefix == 2,
        "empty ground suppression facts changed pending command rows");
}

void CheckBirthRetirementUpdatesSortPrefixAndFreeze()
{
    using namespace hs;
    using namespace hs::runtime_detail;
    const auto command = [](Sequence sequence, Tick tick, std::uint32_t count) {
        ParticleSpawnCommand row;
        row.sequence = sequence; row.tick = tick; row.count = count;
        return row;
    };
    VfxSessionState state;
    state.Synchronize(90);
    state.decoded.particle_spawns = {
        command(1, 1, 0), command(2, 3, 2), command(3, 6, 3),
        command(4, 5, 0), command(5, 2, 4), command(7, 4, 6),
        command(9, 6, 8), command(8, 12, 7)};
    std::vector<ParticleSpawnCommand> oracle = state.decoded.particle_spawns;
    constexpr std::size_t original_prefix = 3;
    const std::array replaced{Sequence{2}, Sequence{5}};

    allocation_budget = 0;
    auto retained = vfx_birth_retirement::RetireCompatibilityBirthRows(
        state.decoded.particle_spawns, original_prefix, replaced, std::span<const Sequence>{},
        std::span<const Sequence>{}, std::span<const Sequence>{});
    allocation_budget = -1;
    Check(retained == 1 && state.decoded.particle_spawns.size() == 4 &&
          state.decoded.particle_spawns[0].sequence == 3,
          "compatibility retirement did not compact old and new no-op rows without allocation");
    for (auto &row : oracle)
        if (row.count == 0 || row.sequence == 2 || row.sequence == 5) row.count = 0;

    const auto status_shard = command(6, 7, 5);
    state.decoded.particle_spawns.push_back(status_shard);
    oracle.push_back(status_shard);
    const std::array ground_facts{VfxGroundBirthSuppressionFacts{6, true, false, false, false}};
    allocation_budget = 0;
    retained = vfx_birth_retirement::RetireGroundSuppressedBirthRows(
        state.decoded.particle_spawns, retained, ground_facts);
    allocation_budget = -1;
    Check(retained == 1 && state.decoded.particle_spawns.size() == 4,
          "ground retirement failed to remove a post-commit shard in place");
    for (auto &row : oracle)
        if (row.sequence == 6) row.count = 0;

    state.decoded.SortNewParticleSpawns(retained);
    std::stable_sort(oracle.begin() + original_prefix, oracle.end(),
        [](const auto &a, const auto &b) { return a.tick < b.tick; });
    if (oracle.size() > original_prefix && oracle[original_prefix].tick < oracle[original_prefix - 1].tick)
        std::inplace_merge(oracle.begin(), oracle.begin() + original_prefix, oracle.end(),
            [](const auto &a, const auto &b) { return a.tick < b.tick; });
    oracle.erase(std::remove_if(oracle.begin(), oracle.end(),
        [](const auto &row) { return row.count == 0; }), oracle.end());
    Check(oracle.size() == 4 && state.decoded.particle_spawns.size() == oracle.size(),
          "mark-zero oracle and in-place retirement produced different live rows");
    Check(oracle[0].sequence == 7 && oracle[1].sequence == 3 &&
          oracle[2].sequence == 9 && oracle[3].sequence == 8,
          "old and new same-tick births lost stable merge ordering");
    for (std::size_t index = 0; index < oracle.size(); ++index)
        Check(state.decoded.particle_spawns[index].sequence == oracle[index].sequence &&
              state.decoded.particle_spawns[index].tick == oracle[index].tick,
              "retirement changed the stable old-prefix/new-suffix sort order");

    VfxSessionState reference;
    reference.Synchronize(90);
    reference.decoded.particle_spawns = oracle;
    state.native_batch.particle_spawns.reserve(3);
    reference.native_batch.particle_spawns.reserve(3);
    allocation_budget = 0;
    const auto *actual_batch = state.PrepareNativeBatch(8, true);
    const auto *oracle_batch = reference.PrepareNativeBatch(8, true);
    allocation_budget = -1;
    Check(actual_batch && oracle_batch && actual_batch->particle_spawns.size() == 3 &&
          oracle_batch->particle_spawns.size() == 3 &&
          state.decoded.particle_spawns.size() == 1 && reference.decoded.particle_spawns.size() == 1 &&
          actual_batch->particle_spawns[0].sequence == oracle_batch->particle_spawns[0].sequence &&
          actual_batch->particle_spawns[1].sequence == oracle_batch->particle_spawns[1].sequence &&
          actual_batch->particle_spawns[2].sequence == oracle_batch->particle_spawns[2].sequence &&
          state.decoded.particle_spawns[0].sequence == reference.decoded.particle_spawns[0].sequence &&
          state.decoded.particle_spawns[0].sequence == 8,
          "retirement changed frozen due order or retained the wrong future tail");
}

auto Retained(hs::runtime_detail::DecodedVfxState &state)
{
    return std::tie(state.particle_spawns, state.effect_lines, state.flashes,
        state.entity_surface_captures);
}

void Resize(hs::runtime_detail::DecodedVfxState &state, std::size_t count)
{
    std::apply([&](auto &...retained) { (retained.resize(count), ...); }, Retained(state));
}

void CheckSize(hs::runtime_detail::DecodedVfxState &state, std::size_t count)
{
    Check(std::apply([&](const auto &...retained) {
        return ((retained.size() == count) && ...);
    }, Retained(state)), "every decoded retention category must transfer/reset");
}

void CheckActiveEffectRetention()
{
    using Store = hs::runtime_detail::ActiveEffectStore;
    Store effects;
    hs::VfxEventInput a{}, b{}, c{}, missing{};
    a.sequence = 10; b.sequence = 20; c.sequence = 30; missing.sequence = 40;
    const auto ar = effects.Intern(a), br = effects.Intern(b), cr = effects.Intern(c);
    Check(effects.Add(Store::Profile::Ground, ar) &&
          effects.Add(Store::Profile::Ground, br) &&
          effects.Add(Store::Profile::Ground, cr), "initial effect membership");
    const auto *unchanged = effects.Get(ar);
    effects.Compact();
    Check(effects.Get(ar) == unchanged && effects.Find(c) == cr,
          "unchanged membership must preserve rows and their indices");
    allocation_budget = 0;
    effects.SetQuality(hs::VfxQuality::Low);
    allocation_budget = -1;
    Check(effects.Get(ar) == unchanged && effects.Find(c) == cr &&
          effects.Get(ar)->quality == hs::VfxQuality::Low && effects.Get(br)->quality == hs::VfxQuality::Low &&
          effects.ProfileEvents(Store::Profile::Ground).size() == 3,
          "retained quality update allocated, changed episode identity or lost profile membership");
    effects.EraseIf(Store::Profile::Ground, [&](const hs::VfxEventInput &event) {
        return event.sequence == b.sequence || event.sequence == missing.sequence;
    });
    std::vector<hs::runtime_detail::RetainedVfxEntitySurfaceCapture> captures(1);
    captures[0].row = br;
    effects.Compact(captures);
    Check(effects.Size() == 3 && captures[0].row == br,
          "capture retains an effect after its profile expires");
    captures.clear();
    effects.Compact(captures);
    const auto retained = effects.ProfileEvents(Store::Profile::Ground);
    Check(effects.Size() == 2 && effects.Find(a) == 0 && effects.Find(c) == 1 &&
          retained.size() == 2 && retained[0].sequence == 10 &&
          retained[1].sequence == 30,
          "stable erase and compaction preserve retained profile order");
    effects.EraseIf(Store::Profile::Ground, [](const auto &) { return true; });
    effects.Compact();
    Check(effects.Size() == 0 && !effects.Get(0), "last effect retirement clears rows");
    Check(effects.Intern(b) == 0, "row storage is reusable after retirement");
}

void CheckGeometrySectorIdentity()
{
    using Store = hs::runtime_detail::ActiveEffectStore;
    Store effects;
    std::array<hs::VfxEventInput, 4> sectors{};
    for (std::uint32_t i = 0; i < sectors.size(); ++i)
    {
        sectors[i].sequence = 10;
        sectors[i].event_tick = 20;
        sectors[i].geometry_owner_id = 30;
        sectors[i].geometry_effect_id = 40;
        sectors[i].status_episode_generation = 50;
        sectors[i].geometry_sector_index = i;
        const auto row = effects.Intern(sectors[i]);
        Check(row == i && effects.Add(Store::Profile::Ground, row),
              "safe sectors with shared episode identity need distinct rows");
    }

    auto rebound = sectors;
    for (auto &sector : rebound) sector.stable_seed = 99;
    effects.RebindProfile(Store::Profile::Ground, rebound);
    for (std::uint32_t i = 0; i < rebound.size(); ++i)
        Check(effects.Get(i)->stable_seed == 99 && effects.Find(sectors[i]) == i,
              "profile rebind must preserve each sector's row identity");

    effects.EraseIf(Store::Profile::Ground, [](const hs::VfxEventInput &event) {
        return event.geometry_sector_index == 0 || event.geometry_sector_index == 3;
    });
    effects.Compact();
    Check(effects.Size() == 2 && effects.Get(0)->geometry_sector_index == 1 &&
          effects.Get(1)->geometry_sector_index == 2 &&
          effects.Find(sectors[1]) == 0 && effects.Find(sectors[2]) == 1,
          "profile commit and compaction must retain distinct sector rows");
}

void CheckPreparedActiveEffectAppendBatch()
{
    using Store = hs::runtime_detail::ActiveEffectStore;
    const auto bit = [](Store::Profile profile) {
        return static_cast<std::uint16_t>(1u << static_cast<unsigned>(profile));
    };
    Store effects;
    hs::VfxEventInput existing{}, added{};
    existing.sequence = 71;
    added.sequence = 82;
    hs::VfxRingGapsPayload gaps;
    gaps.gap_angles_degrees = {15.0f, 45.0f};
    added.payload = gaps;
    effects.Append(Store::Profile::Ground, existing);
    const auto original_row = effects.Find(existing);

    std::array requests{
        Store::AppendRequest{&existing, static_cast<std::uint16_t>(bit(Store::Profile::Ground) |
            bit(Store::Profile::Ribbon))},
        Store::AppendRequest{&added, bit(Store::Profile::Distortion)},
        Store::AppendRequest{&added, static_cast<std::uint16_t>(bit(Store::Profile::Distortion) |
            bit(Store::Profile::Light))},
        Store::AppendRequest{&existing, static_cast<std::uint16_t>(bit(Store::Profile::Light) |
            bit(Store::Profile::PointRate))},
    };
    Store::PreparedAppendBatch prepared;
    Check(effects.PrepareAppendBatch(requests, prepared), "valid profile batch prepares");
    Check(effects.Size() == 1 && effects.Find(added) == std::numeric_limits<Store::Row>::max() &&
          effects.ProfileEvents(Store::Profile::Ribbon).empty() &&
          effects.ProfileEvents(Store::Profile::Light).empty() && effects.Find(existing) == original_row,
          "prepared rows and memberships remain invisible before commit");
    effects.CancelAppendBatch(prepared);
    Check(effects.Size() == 1 && effects.Find(added) == std::numeric_limits<Store::Row>::max() &&
          effects.ProfileEvents(Store::Profile::Ground).size() == 1,
          "cancelled batch leaves canonical rows and memberships unchanged");

    Check(effects.PrepareAppendBatch(requests, prepared), "reusable profile batch prepares again");
    std::get<hs::VfxRingGapsPayload>(added.payload).gap_angles_degrees[0] = 99.0f;
    Check(effects.CommitAppendBatch(prepared), "prepared profile batch commits");
    const auto added_row = effects.Find(added);
    Check(effects.Size() == 2 && added_row != original_row &&
          std::get<hs::VfxRingGapsPayload>(effects.Get(added_row)->payload).gap_angles_degrees[0] == 15.0f,
          "new canonical row owns the payload copy made during preparation");
    const auto ribbon = effects.ProfileEvents(Store::Profile::Ribbon);
    const auto distortion = effects.ProfileEvents(Store::Profile::Distortion);
    const auto light = effects.ProfileEvents(Store::Profile::Light);
    const auto point_rate = effects.ProfileEvents(Store::Profile::PointRate);
    Check(ribbon.size() == 1 && ribbon[0].sequence == existing.sequence &&
          distortion.size() == 1 && distortion[0].sequence == added.sequence &&
          light.size() == 2 && light[0].sequence == added.sequence && light[1].sequence == existing.sequence &&
          point_rate.size() == 1 && point_rate[0].sequence == existing.sequence,
          "profile additions deduplicate requests and preserve request order per profile");

    const std::array duplicates{
        Store::AppendRequest{&added, bit(Store::Profile::Distortion)},
        Store::AppendRequest{&added, bit(Store::Profile::Distortion)},
        Store::AppendRequest{&existing, bit(Store::Profile::Light)},
    };
    Check(effects.PrepareAppendBatch(duplicates, prepared) && effects.CommitAppendBatch(prepared) &&
          effects.Size() == 2 && effects.ProfileEvents(Store::Profile::Distortion).size() == 1 &&
          effects.ProfileEvents(Store::Profile::Light).size() == 2,
          "reused batch does not publish duplicate rows or repeat membership");

    hs::VfxEventInput ignored{};
    ignored.sequence = 99;
    const std::array invalid{
        Store::AppendRequest{&ignored, static_cast<std::uint16_t>(1u << static_cast<unsigned>(Store::Profile::Count))}};
    Check(!effects.PrepareAppendBatch(invalid, prepared) && effects.Size() == 2 &&
          effects.Find(ignored) == std::numeric_limits<Store::Row>::max(),
          "out-of-range profile masks reject before changing the store");
    const std::array zero_mask{Store::AppendRequest{&ignored, 0}};
    Check(effects.PrepareAppendBatch(zero_mask, prepared) && effects.CommitAppendBatch(prepared) &&
          effects.Size() == 2 && effects.Find(ignored) == std::numeric_limits<Store::Row>::max(),
          "zero profile mask does not intern an unreferenced row");

    Store stale;
    const std::array stale_request{Store::AppendRequest{&ignored, bit(Store::Profile::Ribbon)}};
    Check(stale.PrepareAppendBatch(stale_request, prepared), "stale guard batch prepares");
    hs::VfxEventInput concurrent{};
    concurrent.sequence = 111;
    stale.Append(Store::Profile::Screen, concurrent);
    Check(!stale.CommitAppendBatch(prepared) && stale.Size() == 1 &&
          stale.Find(ignored) == std::numeric_limits<Store::Row>::max(),
          "a row/index/profile size change invalidates prepared publication");
    stale.CancelAppendBatch(prepared);
    Store original_owner, other_owner;
    Check(original_owner.PrepareAppendBatch(stale_request, prepared) &&
          !other_owner.CommitAppendBatch(prepared) && other_owner.Size() == 0 &&
          original_owner.CommitAppendBatch(prepared) && original_owner.Size() == 1,
          "prepared rows may publish only to their original store even when both base shapes match");

    // Request count is identical, but retained-row staging must scale with
    // distinct keys. Keep a large dynamic payload on the first duplicate so
    // planning can be checked before the one canonical copy is materialized.
    constexpr std::size_t duplicate_count = 48;
    std::vector<float> first_angles(256);
    for (std::size_t i = 0; i < first_angles.size(); ++i)
        first_angles[i] = static_cast<float>(i) * 0.5f;
    std::vector<hs::VfxEventInput> duplicate_inputs(duplicate_count);
    std::vector<Store::AppendRequest> duplicate_requests;
    duplicate_requests.reserve(duplicate_count);
    Store duplicate_store;
    hs::VfxEventInput duplicate_existing{};
    duplicate_existing.sequence = 1200;
    duplicate_store.Append(Store::Profile::Ribbon, duplicate_existing);
    duplicate_requests.push_back({&duplicate_existing, bit(Store::Profile::Light)});
    for (std::size_t i = 0; i < duplicate_inputs.size(); ++i)
    {
        auto &input = duplicate_inputs[i];
        input.sequence = 1300;
        input.stable_seed = static_cast<std::uint32_t>(i);
        hs::VfxRingGapsPayload payload;
        payload.gap_angles_degrees = first_angles;
        payload.gap_angles_degrees[0] = static_cast<float>(i);
        input.payload = std::move(payload);
        duplicate_requests.push_back({&input, bit(Store::Profile::Distortion)});
    }
    // Match request count and profile masks, varying only the number of new
    // event keys to compare the unique-row staging footprint.
    std::vector<hs::VfxEventInput> unique_inputs(duplicate_count);
    std::vector<Store::AppendRequest> unique_requests;
    unique_requests.reserve(duplicate_count);
    unique_requests.push_back({&duplicate_existing, bit(Store::Profile::Light)});
    for (std::size_t i = 0; i < unique_inputs.size(); ++i)
    {
        auto &input = unique_inputs[i];
        input.sequence = static_cast<hs::Sequence>(1400 + i);
        hs::VfxRingGapsPayload payload;
        payload.gap_angles_degrees = first_angles;
        input.payload = std::move(payload);
        unique_requests.push_back({&input, bit(Store::Profile::Distortion)});
    }
    Store unique_store;
    unique_store.Append(Store::Profile::Ribbon, duplicate_existing);
    Store::PreparedAppendBatch duplicate_prepared, unique_prepared;
    bool duplicate_planned{};
    // Permit bookkeeping and unique-row storage, but reject a request-sized event array.
    allocation_size_limit = duplicate_count / 2 * sizeof(hs::VfxEventInput);
    try { duplicate_planned = duplicate_store.PrepareAppendBatch(duplicate_requests, duplicate_prepared); }
    catch (...) { allocation_size_limit = std::numeric_limits<std::size_t>::max(); throw; }
    allocation_size_limit = std::numeric_limits<std::size_t>::max();
    Check(duplicate_planned && unique_store.PrepareAppendBatch(unique_requests, unique_prepared) &&
          duplicate_prepared.OwnedPayloadCapacityBytes() < unique_prepared.OwnedPayloadCapacityBytes() / 16 &&
          duplicate_prepared.LogicalCapacityBytes() < unique_prepared.LogicalCapacityBytes(),
          "duplicate requests reserve/copy by unique new keys, not request count");
    duplicate_inputs[0].stable_seed = 999;
    std::get<hs::VfxRingGapsPayload>(duplicate_inputs[0].payload).gap_angles_degrees[0] = -1.0f;
    Check(duplicate_store.CommitAppendBatch(duplicate_prepared) && duplicate_store.Size() == 2 &&
          duplicate_store.ProfileEvents(Store::Profile::Light).size() == 1 &&
          duplicate_store.ProfileEvents(Store::Profile::Distortion).size() == 1 &&
          duplicate_store.ProfileEvents(Store::Profile::Distortion)[0].stable_seed == 0 &&
          std::get<hs::VfxRingGapsPayload>(duplicate_store.ProfileEvents(
              Store::Profile::Distortion)[0].payload).gap_angles_degrees[0] == 0.0f,
          "planning preserves first duplicate payload and per-profile request order");
    unique_store.CancelAppendBatch(unique_prepared);

    // A nested payload allocation can fail after planning, before any store
    // publication. Reuse the outer staging capacities so the injected failure
    // specifically exercises payload materialization.
    Store allocation_store;
    Store::PreparedAppendBatch allocation_prepared;
    hs::VfxEventInput warm_input{};
    warm_input.sequence = 2100;
    const std::array warm_requests{
        Store::AppendRequest{&warm_input, bit(Store::Profile::Ribbon)}};
    Check(allocation_store.PrepareAppendBatch(warm_requests, allocation_prepared),
        "allocation-failure staging warms its outer vectors");
    allocation_store.CancelAppendBatch(allocation_prepared);
    hs::VfxEventInput failing_input{};
    failing_input.sequence = 2101;
    hs::VfxRingGapsPayload failing_payload;
    failing_payload.gap_angles_degrees = first_angles;
    failing_input.payload = std::move(failing_payload);
    const std::array failing_requests{
        Store::AppendRequest{&failing_input, bit(Store::Profile::Ribbon)}};
    bool payload_allocation_failed{};
    allocation_budget = 0;
    try { (void)allocation_store.PrepareAppendBatch(failing_requests, allocation_prepared); }
    catch (const std::bad_alloc &) { payload_allocation_failed = true; }
    allocation_budget = -1;
    Check(payload_allocation_failed && allocation_store.Size() == 0 &&
          allocation_store.Find(failing_input) == std::numeric_limits<Store::Row>::max() &&
          allocation_store.ProfileEvents(Store::Profile::Ribbon).empty(),
          "failed unique payload materialization leaves retained rows and memberships unpublished");

    Store unsorted;
    hs::VfxEventInput high{}, low{}, middle{};
    high.sequence = 900; high.stable_seed = 91;
    low.sequence = 100; low.stable_seed = 81;
    middle.sequence = 500; middle.stable_seed = 33;
    auto repeated_high = high; repeated_high.stable_seed = 17;
    const std::array source_order{
        Store::AppendRequest{&high, bit(Store::Profile::Ground)},
        Store::AppendRequest{&low, bit(Store::Profile::Ground)},
        Store::AppendRequest{&repeated_high, static_cast<std::uint16_t>(
            bit(Store::Profile::Ground) | bit(Store::Profile::Light))},
        Store::AppendRequest{&middle, bit(Store::Profile::Ground)},
    };
    for (unsigned warm = 0; warm < 2; ++warm)
    {
        Check(unsorted.PrepareAppendBatch(source_order, prepared) &&
            unsorted.CommitAppendBatch(prepared), "unsorted key batch prepares and commits");
        const auto got = unsorted.ProfileEvents(Store::Profile::Ground);
        Check(unsorted.Size() == 3 && got.size() == 3 &&
            got[0].sequence == 900 && got[0].stable_seed == 91 &&
            got[1].sequence == 100 && got[2].sequence == 500 &&
            unsorted.ProfileEvents(Store::Profile::Light)[0].stable_seed == 91,
            "indexed request dedup must preserve first input payload and source-order memberships");
    }
}

void CheckOptionalProfileAdmission()
{
    using Store = hs::runtime_detail::ActiveEffectStore;
    using Profile = Store::Profile;
    const auto bit = [](Profile profile) {
        return static_cast<std::uint16_t>(1u << static_cast<unsigned>(profile));
    };
    const auto held = [](const Store &store, const Store::PreparedAppendBatch &prepared,
                          const std::vector<std::size_t> &order) {
        using namespace hs::runtime_detail::vfx_retained_size;
        return Add(store.LogicalCapacityBytes(), Add(prepared.LogicalCapacityBytes(), Vector(order)));
    };

    // A cosmetic-only event may be omitted at zero growth budget while CPU
    // activation still commits and acknowledges the ready raw prefix.
    hs::runtime_detail::VfxSessionState activation;
    activation.Synchronize(61);
    hs::PresentationEvent raw{};
    raw.session_id = 61; raw.tick = 1; raw.sequence = 1;
    Check(activation.raw_events.Enqueue(raw) && activation.raw_events.Prepare(1, 61).entries.size() == 1,
        "cosmetic admission fixture has a ready raw event");
    hs::VfxEventInput cosmetic{};
    cosmetic.sequence = 610;
    hs::VfxRingGapsPayload large_gaps;
    large_gaps.gap_angles_degrees.resize(512, 15.0f);
    cosmetic.payload = std::move(large_gaps);
    const std::array cosmetic_request{Store::AppendRequest{&cosmetic, bit(Profile::Light), false,
        bit(Profile::Light), 10}};
    Store::PreparedAppendBatch cosmetic_prepared;
    std::vector<std::size_t> order;
    using hs::runtime_detail::vfx_retained_size::Add;
    using hs::runtime_detail::vfx_retained_size::Vector;
    Store::ProfileAdmission zero_budget{Add(activation.TrackedLogicalCapacityBytes(),
        Add(cosmetic_prepared.LogicalCapacityBytes(), Vector(order))), 0, order};
    Check(activation.active_effects.PrepareAppendBatch(cosmetic_request, cosmetic_prepared, &zero_budget) &&
          zero_budget.requested_cosmetic_memberships == 1 && zero_budget.admitted_cosmetic_memberships == 0 &&
          cosmetic_prepared.OwnedPayloadCapacityBytes() == 0 && activation.active_effects.Size() == 0,
          "zero budget drops cosmetic row before payload cloning or index-node creation");
    hs::runtime_detail::DecodedVfxState no_births;
    Check(activation.CommitPreparedActivation(no_births, cosmetic_prepared, true) &&
          activation.active_effects.Size() == 0 && activation.raw_events.Prepare(1, 61).entries.empty(),
          "admitting no cosmetic profile still commits CPU activation and acknowledges raw input");

    // Required output survives the same tiny budget; cosmetic bits sharing its
    // row remain independently droppable.
    Store mixed;
    hs::VfxEventInput mixed_input{};
    mixed_input.sequence = 620;
    hs::VfxRingGapsPayload mixed_gaps;
    mixed_gaps.gap_angles_degrees = {17.0f, 23.0f};
    mixed_input.payload = mixed_gaps;
    auto required = Store::AppendRequest{&mixed_input,
        static_cast<std::uint16_t>(bit(Profile::Ribbon) | bit(Profile::Light)), false,
        bit(Profile::Light), 1};
    Store::PreparedAppendBatch mixed_prepared;
    std::vector<std::size_t> mixed_order;
    Store::ProfileAdmission mixed_admission{held(mixed, mixed_prepared, mixed_order), 0, mixed_order};
    const std::array mixed_requests{required};
    Check(mixed.PrepareAppendBatch(mixed_requests, mixed_prepared, &mixed_admission) &&
          mixed_admission.requested_cosmetic_memberships == 1 &&
          mixed_admission.admitted_cosmetic_memberships == 0 && mixed.CommitAppendBatch(mixed_prepared) &&
          mixed.Size() == 1 && mixed.ProfileEvents(Profile::Ribbon).size() == 1 &&
          mixed.ProfileEvents(Profile::Light).empty() &&
          std::get<hs::VfxRingGapsPayload>(mixed.ProfileEvents(Profile::Ribbon)[0].payload)
              .gap_angles_degrees[0] == 17.0f,
          "required profile and first-input payload survive while shared-row cosmetic profile is dropped");

    Store duplicate_store;
    hs::VfxEventInput duplicate_input{}; duplicate_input.sequence = 621;
    const std::array duplicate_cosmetics{
        Store::AppendRequest{&duplicate_input, bit(Profile::Light), false, bit(Profile::Light), 3},
        Store::AppendRequest{&duplicate_input, bit(Profile::Light), false, bit(Profile::Light), 7}};
    Store::PreparedAppendBatch duplicate_prepared;
    std::vector<std::size_t> duplicate_order;
    Store::ProfileAdmission duplicate_admission{held(duplicate_store, duplicate_prepared, duplicate_order),
        std::numeric_limits<std::size_t>::max(), duplicate_order};
    Check(duplicate_store.PrepareAppendBatch(duplicate_cosmetics, duplicate_prepared, &duplicate_admission) &&
          duplicate_admission.requested_cosmetic_memberships == 1 &&
          duplicate_admission.admitted_cosmetic_memberships == 1 &&
          duplicate_store.CommitAppendBatch(duplicate_prepared) && duplicate_store.Size() == 1 &&
          duplicate_store.ProfileEvents(Profile::Light).size() == 1,
          "duplicate requests for one row/profile are planned and charged once");

    Store promoted;
    Store::PreparedAppendBatch promoted_prepared;
    std::vector<std::size_t> promoted_order;
    const std::array promoted_requests{
        duplicate_cosmetics[0], Store::AppendRequest{&duplicate_input, bit(Profile::Light)}};
    Store::ProfileAdmission promoted_admission{held(promoted, promoted_prepared, promoted_order),
        0, promoted_order};
    Check(promoted.PrepareAppendBatch(promoted_requests, promoted_prepared, &promoted_admission) &&
          promoted_admission.requested_cosmetic_memberships == 0 &&
          promoted.CommitAppendBatch(promoted_prepared) && promoted.Size() == 1 &&
          promoted.ProfileEvents(Profile::Light).size() == 1,
          "a later required duplicate protects the membership before cosmetic selection");

    const auto context_input = [](std::uint64_t sequence, std::uint64_t owner) {
        hs::VfxEventInput input;
        input.sequence = sequence; input.effect_handle = 8;
        input.payload = hs::VfxContextPayload{owner};
        return input;
    };
    Store replacement_store;
    auto old_context = context_input(622, 88);
    replacement_store.Append(Profile::Ground, old_context);
    auto new_context = context_input(623, 88);
    const std::array replacement_request{Store::AppendRequest{&new_context,
        static_cast<std::uint16_t>(bit(Profile::Ground) | bit(Profile::Screen)), true,
        bit(Profile::Screen), 0}};
    Store::PreparedAppendBatch replacement_prepared;
    std::vector<std::size_t> replacement_order;
    Store::ProfileAdmission replacement_admission{held(replacement_store, replacement_prepared,
        replacement_order), 0, replacement_order};
    Check(replacement_store.PrepareAppendBatch(replacement_request, replacement_prepared,
              &replacement_admission) && replacement_admission.requested_cosmetic_memberships == 0 &&
          replacement_store.CommitAppendBatch(replacement_prepared) &&
          replacement_store.ProfileEvents(Profile::Ground).size() == 1 &&
          replacement_store.ProfileEvents(Profile::Ground)[0].sequence == 623 &&
          replacement_store.ProfileEvents(Profile::Screen).size() == 1,
          "Ground context replacement forces every requested profile to remain required");

    // Warm capacity for two outputs, then measure the logical marginal cost of
    // one cosmetic membership so priority and stable source-order ties can be
    // checked without depending on allocator over-allocation.
    Store prioritized;
    hs::VfxEventInput base{}; base.sequence = 630;
    prioritized.Append(Profile::Ground, base);
    prioritized.Append(Profile::Light, base);
    Store::PreparedAppendBatch prepared;
    std::vector<std::size_t> priority_order;
    hs::VfxEventInput warm_a{}, warm_b{};
    warm_a.sequence = 631; warm_b.sequence = 632;
    for (auto *input : {&warm_a, &warm_b})
    {
        hs::VfxRingGapsPayload payload; payload.gap_angles_degrees = {1.0f, 2.0f, 3.0f, 4.0f};
        input->payload = std::move(payload);
    }
    const std::array warm_requests{
        Store::AppendRequest{&warm_a, bit(Profile::Light), false, bit(Profile::Light), 1},
        Store::AppendRequest{&warm_b, bit(Profile::Light), false, bit(Profile::Light), 1}};
    Store::ProfileAdmission warm_admission{held(prioritized, prepared, priority_order),
        std::numeric_limits<std::size_t>::max(), priority_order};
    Check(prioritized.PrepareAppendBatch(warm_requests, prepared, &warm_admission),
        "priority fixture warm-up prepares cosmetic rows");
    prioritized.CancelAppendBatch(prepared);

    hs::VfxEventInput probe{}; probe.sequence = 633;
    hs::VfxRingGapsPayload probe_payload; probe_payload.gap_angles_degrees = {1.0f, 2.0f, 3.0f, 4.0f};
    probe.payload = std::move(probe_payload);
    const std::array probe_request{
        Store::AppendRequest{&probe, bit(Profile::Light), false, bit(Profile::Light), 1}};
    const auto probe_held = held(prioritized, prepared, priority_order);
    Store::ProfileAdmission probe_admission{probe_held, std::numeric_limits<std::size_t>::max(), priority_order};
    Check(prioritized.PrepareAppendBatch(probe_request, prepared, &probe_admission),
        "priority fixture measures one admitted membership");
    const auto one_membership_cost = held(prioritized, prepared, priority_order) - probe_held;
    prioritized.CancelAppendBatch(prepared);

    hs::VfxEventInput low_first{}, high_second{};
    low_first.sequence = 634; high_second.sequence = 635;
    for (auto *input : {&low_first, &high_second})
    {
        hs::VfxRingGapsPayload payload; payload.gap_angles_degrees = {1.0f, 2.0f, 3.0f, 4.0f};
        input->payload = std::move(payload);
    }
    const std::array priority_requests{
        Store::AppendRequest{&low_first, bit(Profile::Light), false, bit(Profile::Light), 2},
        Store::AppendRequest{&high_second, bit(Profile::Light), false, bit(Profile::Light), 9}};
    const auto priority_held = held(prioritized, prepared, priority_order);
    Store::ProfileAdmission priority_admission{priority_held,
        hs::runtime_detail::vfx_retained_size::Add(priority_held, one_membership_cost), priority_order};
    Check(prioritized.PrepareAppendBatch(priority_requests, prepared, &priority_admission) &&
          priority_admission.requested_cosmetic_memberships == 2 &&
          priority_admission.admitted_cosmetic_memberships == 1 &&
          prioritized.CommitAppendBatch(prepared) && prioritized.ProfileEvents(Profile::Light).size() == 2 &&
          prioritized.ProfileEvents(Profile::Light)[1].sequence == 635,
          "higher-importance cosmetic membership wins when only one fits");

    // With equal importance, original request order breaks the tie. Reuse the
    // same warmed capacities so the exact one-membership allowance remains valid.
    hs::VfxEventInput tie_first{}, tie_second{};
    tie_first.sequence = 636; tie_second.sequence = 637;
    for (auto *input : {&tie_first, &tie_second})
    {
        hs::VfxRingGapsPayload payload; payload.gap_angles_degrees = {1.0f, 2.0f, 3.0f, 4.0f};
        input->payload = std::move(payload);
    }
    const std::array tie_requests{
        Store::AppendRequest{&tie_first, bit(Profile::Light), false, bit(Profile::Light), 5},
        Store::AppendRequest{&tie_second, bit(Profile::Light), false, bit(Profile::Light), 5}};
    const auto tie_held = held(prioritized, prepared, priority_order);
    Store::ProfileAdmission tie_admission{tie_held,
        hs::runtime_detail::vfx_retained_size::Add(tie_held, one_membership_cost), priority_order};
    Check(prioritized.PrepareAppendBatch(tie_requests, prepared, &tie_admission) &&
          tie_admission.admitted_cosmetic_memberships == 1 && prioritized.CommitAppendBatch(prepared) &&
          prioritized.ProfileEvents(Profile::Light).size() == 3 &&
          prioritized.ProfileEvents(Profile::Light)[2].sequence == 636,
          "equal-priority admission uses original request order and leaves canonical profile order");

    prioritized.EraseIf(Profile::Light, [](const auto &) { return true; });
    const std::array reuse_request{
        Store::AppendRequest{&base, bit(Profile::Light), false, bit(Profile::Light), 1}};
    const auto reuse_held = held(prioritized, prepared, priority_order);
    Store::ProfileAdmission reuse_admission{reuse_held, 0, priority_order};
    Check(prioritized.PrepareAppendBatch(reuse_request, prepared, &reuse_admission) &&
          reuse_admission.requested_cosmetic_memberships == 1 &&
          reuse_admission.admitted_cosmetic_memberships == 1 &&
          prioritized.CommitAppendBatch(prepared) &&
          prioritized.ProfileEvents(Profile::Light).size() == 1 &&
          prioritized.ProfileEvents(Profile::Light)[0].sequence == base.sequence &&
          held(prioritized, prepared, priority_order) == reuse_held,
          "zero-growth membership reuses existing row and reference capacity above the soft budget");
}

void CheckPreparedGroundOwnerReplacement()
{
    using Store = hs::runtime_detail::ActiveEffectStore;
    const auto bit = [](Store::Profile profile) {
        return static_cast<std::uint16_t>(1u << static_cast<unsigned>(profile));
    };
    const auto context = [](hs::Sequence sequence, std::uint64_t owner) {
        hs::VfxEventInput input;
        input.sequence = sequence; input.effect_handle = 1; input.event_tick = 10;
        input.stable_seed = static_cast<std::uint32_t>(sequence);
        input.payload = hs::VfxContextPayload{owner};
        return input;
    };
    auto old_a = context(10, 7), old_b = context(20, 8);
    hs::VfxEventInput ordinary;
    ordinary.sequence = 30; ordinary.effect_handle = 2;
    ordinary.payload = hs::VfxCirclePayload{};
    Store actual, sequential;
    for (auto *store : {&actual, &sequential})
    {
        store->Append(Store::Profile::Ground, old_a);
        store->Append(Store::Profile::Ground, old_b);
        store->Append(Store::Profile::Ground, ordinary);
        store->Append(Store::Profile::Light, old_a);
    }
    auto first_a = context(40, 7), last_b = context(60, 8), after_a = context(70, 7);
    auto plain = ordinary; plain.sequence = 50;
    auto discarded_a = context(35, 7);
    const std::array requests{
        Store::AppendRequest{&discarded_a, bit(Store::Profile::Ground), true},
        Store::AppendRequest{&first_a, static_cast<std::uint16_t>(bit(Store::Profile::Ground) |
            bit(Store::Profile::Light)), true},
        Store::AppendRequest{&plain, bit(Store::Profile::Ground)},
        Store::AppendRequest{&first_a, bit(Store::Profile::Ground), true},
        Store::AppendRequest{&last_b, bit(Store::Profile::Ground), true},
        Store::AppendRequest{&after_a, bit(Store::Profile::Ground)},
    };
    const auto apply_sequential = [&](const auto &batch) {
        for (const auto &request : batch)
        {
            if (request.replace_ground_context_owner)
            {
                const auto owner = std::get<hs::VfxContextPayload>(request.input->payload).owner_id;
                sequential.EraseIf(Store::Profile::Ground, [&](const auto &older) {
                    const auto *context_payload = std::get_if<hs::VfxContextPayload>(&older.payload);
                    return older.effect_handle == request.input->effect_handle &&
                        context_payload && context_payload->owner_id == owner;
                });
            }
            for (unsigned p = 0; p < static_cast<unsigned>(Store::Profile::Count); ++p)
                if (request.profile_mask & (1u << p))
                    sequential.Append(static_cast<Store::Profile>(p), *request.input);
        }
    };
    const auto compare_profiles = [&] {
        for (unsigned p = 0; p < static_cast<unsigned>(Store::Profile::Count); ++p)
        {
            const auto profile = static_cast<Store::Profile>(p);
            const auto got = actual.ProfileEvents(profile), expected = sequential.ProfileEvents(profile);
            Check(got.size() == expected.size(), "prepared replacement changed profile membership count");
            for (std::size_t i = 0; i < got.size(); ++i)
                Check(got[i].sequence == expected[i].sequence &&
                    got[i].effect_handle == expected[i].effect_handle &&
                    got[i].stable_seed == expected[i].stable_seed,
                    "prepared replacement changed sequential profile order or seed");
        }
    };
    Store::PreparedAppendBatch prepared;
    Check(actual.PrepareAppendBatch(requests, prepared), "Ground replacement batch prepares");
    compare_profiles();
    actual.CancelAppendBatch(prepared);
    compare_profiles();
    Check(actual.PrepareAppendBatch(requests, prepared) && actual.CommitAppendBatch(prepared),
        "Ground replacement batch commits after cancellation");
    apply_sequential(requests);
    compare_profiles();
    {
        const auto ground = actual.ProfileEvents(Store::Profile::Ground);
        Check(ground.size() == 5 && ground[0].sequence == 30 && ground[1].sequence == 50 &&
            ground[2].sequence == 40 && ground[3].sequence == 60 && ground[4].sequence == 70 &&
            actual.ProfileEvents(Store::Profile::Light).size() == 2,
            "last replacement must retain other profiles and use its final request position");
    }
    actual.Compact(); sequential.Compact();
    compare_profiles();
    Check(actual.Size() == sequential.Size(), "replacement compaction must retire only unreferenced rows");
    Check(actual.PrepareAppendBatch(requests, prepared) && actual.CommitAppendBatch(prepared),
        "warm replacement batch prepares again");
    apply_sequential(requests);
    compare_profiles();
    const std::array invalid_payload{Store::AppendRequest{&ordinary, bit(Store::Profile::Ground), true}};
    Check(!actual.PrepareAppendBatch(invalid_payload, prepared),
        "Ground context replacement must reject a non-context payload");
    const std::array invalid_profile{Store::AppendRequest{&first_a, bit(Store::Profile::Light), true}};
    Check(!actual.PrepareAppendBatch(invalid_profile, prepared),
        "Ground replacement must include Ground membership");
    compare_profiles();
}

void CheckOwnerBindingStorageReuse()
{
    using Store = hs::runtime_detail::ActiveEffectStore;
    Store effects;
    hs::VfxEventInput input{};
    input.sequence = 17;
    input.geometry_owner_id = 99;
    hs::VfxRingGapsPayload gaps;
    gaps.gap_angles_degrees = {10.0f, 20.0f, 30.0f};
    input.payload = gaps;
    effects.Append(Store::Profile::Ground, input);
    effects.Append(Store::Profile::Light, input);

    std::size_t refresh_count{};
    const auto refresh = [&](hs::VfxEventInput &bound) {
        ++refresh_count;
        auto &angles = std::get<hs::VfxRingGapsPayload>(bound.payload).gap_angles_degrees;
        angles[0] = 15.0f;
        return true;
    };
    effects.BeginFrameBindings();
    effects.RefreshProfile(Store::Profile::Ground, refresh);
    effects.RefreshProfile(Store::Profile::Light, refresh);
    auto view = effects.ProfileEvents(Store::Profile::Ground);
    auto *angles = std::get<hs::VfxRingGapsPayload>(view[0].payload).gap_angles_degrees.data();
    const auto capacity = std::get<hs::VfxRingGapsPayload>(view[0].payload).gap_angles_degrees.capacity();
    Check(refresh_count == 1 && angles && capacity >= 3 &&
          std::get<hs::VfxRingGapsPayload>(effects.Get(0)->payload).gap_angles_degrees[0] == 10.0f,
          "owner binding is shared while canonical birth payload stays unchanged");

    effects.BeginFrameBindings();
    effects.RefreshProfile(Store::Profile::Ground, refresh);
    view = effects.ProfileEvents(Store::Profile::Ground);
    const auto &reused = std::get<hs::VfxRingGapsPayload>(view[0].payload).gap_angles_degrees;
    Check(reused.data() == angles && reused.capacity() == capacity && refresh_count == 2,
          "stable ring-gap owner binding reuses nested vector allocation across frames");
    for (hs::Sequence sequence = 1000; sequence < 2024; ++sequence)
    {
        hs::VfxEventInput unrelated{};
        unrelated.sequence = sequence;
        (void)effects.Intern(unrelated);
    }
    effects.BeginFrameBindings();
    bool reused_without_growth = true;
    allocation_budget = 0;
    try
    {
        effects.RefreshProfile(Store::Profile::Ground, refresh);
        effects.RefreshProfile(Store::Profile::Light, refresh);
    }
    catch (const std::bad_alloc &) { reused_without_growth = false; }
    allocation_budget = -1;
    Check(reused_without_growth && refresh_count == 3,
        "unrelated retained rows grew binding scratch or duplicated shared owner refresh");
}

void CheckOptionalFlashBudget()
{
    using hs::runtime_detail::AvailableVfxFlashAppendRows;
    std::vector<hs::VfxFlashSpawnInput> flashes;
    flashes.reserve(7);
    flashes.resize(3);
    const auto spare = flashes.capacity() - flashes.size();
    constexpr auto row_bytes = sizeof(hs::VfxFlashSpawnInput);
    constexpr std::size_t held = 1000;
    Check(AvailableVfxFlashAppendRows(held, held, flashes) == spare,
        "budget must credit existing unused output capacity without charging it twice");
    Check(AvailableVfxFlashAppendRows(held, held + 2 * row_bytes - 1, flashes) == spare + 1,
        "partial row budget must not admit another complete flash");
    Check(AvailableVfxFlashAppendRows(held, held - 1, flashes) == spare,
        "workspace pressure may reuse prior row reservations but must forbid optional growth");
    Check(flashes.size() == 3, "admission must not alter essential output");
    flashes.clear();
    Check(AvailableVfxFlashAppendRows(held, held, flashes) == flashes.capacity(),
        "subsequent frames must reuse the full reserved fire output space");
    Check(AvailableVfxFlashAppendRows(std::numeric_limits<std::size_t>::max(), held, flashes) == flashes.capacity(),
        "saturated accounting must allow only already owned rows without subtraction underflow");
}

void CheckTrackedVfxCapacityAccounting()
{
    using Store = hs::runtime_detail::ActiveEffectStore;
    const auto payload_bytes = [](const hs::VfxEventInput &input) {
        return std::get<hs::VfxRingGapsPayload>(input.payload).gap_angles_degrees.capacity() *
            sizeof(float);
    };

    Store store;
    hs::VfxEventInput ring{};
    ring.sequence = 901;
    ring.geometry_owner_id = 19;
    hs::VfxRingGapsPayload gaps;
    gaps.gap_angles_degrees.reserve(24);
    gaps.gap_angles_degrees = {10.0f, 30.0f, 70.0f};
    ring.payload = gaps;
    const auto row = store.Intern(ring);
    Check(store.OwnedPayloadCapacityBytes() == payload_bytes(*store.Get(row)),
          "canonical intern must account its owned ring-gap capacity once");
    Check(store.Add(Store::Profile::Ground, row) && store.Add(Store::Profile::Ribbon, row) &&
          !store.Add(Store::Profile::Ribbon, row) &&
          store.OwnedPayloadCapacityBytes() == payload_bytes(*store.Get(row)),
          "shared profiles must not double count a canonical payload or duplicate membership");

    store.BeginFrameBindings();
    store.RefreshProfile(Store::Profile::Ground, [&](hs::VfxEventInput &bound) {
        auto &angles = std::get<hs::VfxRingGapsPayload>(bound.payload).gap_angles_degrees;
        angles.reserve(40);
        return true;
    });
    const auto ground = store.ProfileEvents(Store::Profile::Ground);
    Check(store.OwnedPayloadCapacityBytes() == payload_bytes(*store.Get(row)) + payload_bytes(ground[0]),
          "owner binding must account its independent nested copy after refresh growth");
    const auto binding_bytes = store.OwnedPayloadCapacityBytes();
    store.BeginFrameBindings();
    store.RefreshProfile(Store::Profile::Ground, [&](hs::VfxEventInput &) { return true; });
    Check(store.OwnedPayloadCapacityBytes() == binding_bytes,
          "reused bound rows must retain accurately charged nested capacity");
    const auto bound_copy_bytes = payload_bytes(store.ProfileEvents(Store::Profile::Ground)[0]);

    hs::VfxEventInput retiring = ring;
    retiring.sequence = 902;
    const auto retiring_row = store.Intern(retiring);
    store.Add(Store::Profile::Ground, retiring_row);
    const auto before_compact = store.OwnedPayloadCapacityBytes();
    store.EraseIf(Store::Profile::Ground, [](const auto &input) { return input.sequence == 902; });
    store.Compact();
    Check(store.Size() == 1 && store.OwnedPayloadCapacityBytes() < before_compact &&
          store.OwnedPayloadCapacityBytes() == payload_bytes(*store.Get(0)) +
              bound_copy_bytes,
          "compaction must retire dropped nested copies while preserving bound highwater storage");

    Store other;
    hs::VfxEventInput plain{};
    plain.sequence = 903;
    (void)other.Intern(plain);
    const auto retained_bytes = store.OwnedPayloadCapacityBytes();
    store.Swap(other);
    Check(store.OwnedPayloadCapacityBytes() == 0 && other.OwnedPayloadCapacityBytes() == retained_bytes,
          "store swaps must transfer the nested capacity accounting with row ownership");
    other.Clear();
    Check(other.OwnedPayloadCapacityBytes() == 0 && other.LogicalCapacityBytes() > 0,
          "clear releases nested payloads while retaining reusable outer capacities");

    hs::VfxEventInput staged{};
    staged.sequence = 904;
    hs::VfxRingGapsPayload staged_gaps;
    staged_gaps.gap_angles_degrees.reserve(17);
    staged_gaps.gap_angles_degrees = {5.0f, 15.0f};
    staged.payload = staged_gaps;
    const auto profile_bit = static_cast<std::uint16_t>(1u << static_cast<unsigned>(Store::Profile::Light));
    const std::array request{Store::AppendRequest{&staged, profile_bit}};
    Store::PreparedAppendBatch prepared;
    Check(other.PrepareAppendBatch(request, prepared) &&
          prepared.OwnedPayloadCapacityBytes() > 0 && other.OwnedPayloadCapacityBytes() == 0,
          "prepared rows must account nested copies before they become store-visible");
    other.CancelAppendBatch(prepared);
    Check(prepared.OwnedPayloadCapacityBytes() == 0 && prepared.LogicalCapacityBytes() > 0 &&
          other.OwnedPayloadCapacityBytes() == 0,
          "cancel frees nested staged payloads while retaining reusable scratch capacity");

    Check(other.PrepareAppendBatch(request, prepared) &&
          prepared.OwnedPayloadCapacityBytes() > 0 &&
          other.CommitAppendBatch(prepared) && prepared.OwnedPayloadCapacityBytes() == 0 &&
          other.OwnedPayloadCapacityBytes() == payload_bytes(*other.Get(0)),
          "commit transfers nested staged ownership exactly once into canonical rows");

    hs::VfxEventInput stale_input = staged;
    stale_input.sequence = 905;
    const std::array stale_request{Store::AppendRequest{&stale_input, profile_bit}};
    Check(other.PrepareAppendBatch(stale_request, prepared) &&
          prepared.OwnedPayloadCapacityBytes() > 0,
          "stale batch fixture owns its prepared nested copy");
    hs::VfxEventInput concurrent{};
    concurrent.sequence = 906;
    other.Append(Store::Profile::Screen, concurrent);
    const auto stale_payload_bytes = prepared.OwnedPayloadCapacityBytes();
    Check(!other.CommitAppendBatch(prepared) &&
          prepared.OwnedPayloadCapacityBytes() == stale_payload_bytes,
          "rejected stale publication retains accurate staged ownership until cancellation");
    other.CancelAppendBatch(prepared);
    Check(prepared.OwnedPayloadCapacityBytes() == 0,
          "cancelling a stale prepared batch releases its nested payload ownership");
    const std::array failed_request{Store::AppendRequest{&staged,
        static_cast<std::uint16_t>(1u << static_cast<unsigned>(Store::Profile::Count))}};
    Check(!other.PrepareAppendBatch(failed_request, prepared) &&
          prepared.OwnedPayloadCapacityBytes() == 0,
          "failed batch preparation leaves no nested scratch payload charged");

    hs::runtime_detail::VfxSessionState state;
    state.decoded.particle_spawns.reserve(13);
    state.decoded.effect_lines.reserve(7);
    state.decoded.flashes.reserve(5);
    state.decoded.particle_spawns.push_back({.tick = 10});
    state.decoded.effect_lines.push_back({.tick = 10});
    const auto before_freeze = state.TrackedLogicalCapacityBytes();
    auto *native = state.PrepareNativeBatch(10, false);
    Check(native && state.TrackedLogicalCapacityBytes() == before_freeze,
          "all-due native freeze transfers capacity without losing aggregate accounting");
    const auto while_frozen = state.TrackedLogicalCapacityBytes();
    Check(state.AcknowledgeNativeBatch(native->identity, true) &&
          state.TrackedLogicalCapacityBytes() == while_frozen,
          "native acknowledgement clears contents but keeps reusable vector capacity charged");

    hs::runtime_detail::DecodedVfxState births;
    births.particle_spawns.reserve(11);
    births.particle_spawns.push_back({.sequence = 905});
    const auto birth_bytes = births.LogicalCapacityBytes();
    const auto before_birth_swap = state.TrackedLogicalCapacityBytes();
    Check(state.AppendPreparedBirths(births) && births.particle_spawns.empty() &&
          state.decoded.particle_spawns.capacity() >= 11 &&
          state.TrackedLogicalCapacityBytes() >= before_birth_swap + birth_bytes,
          "empty decoded queues take ownership of prepared backing and charge it to session state");

    hs::runtime_detail::DecodedVfxState replacement;
    replacement.particle_spawns.reserve(29);
    replacement.flashes.reserve(9);
    const auto combined_before_swap = state.TrackedLogicalCapacityBytes() +
        replacement.LogicalCapacityBytes();
    state.SwapDecoded(replacement);
    Check(state.TrackedLogicalCapacityBytes() + replacement.LogicalCapacityBytes() ==
              combined_before_swap,
          "decoded publication swaps reusable backing without dropping its capacity accounting");
    const auto after_decoded_clear = state.TrackedLogicalCapacityBytes();
    state.ClearDecoded();
    Check(state.TrackedLogicalCapacityBytes() == after_decoded_clear,
          "clearing decoded contents retains and charges the reusable vector highwater");

    hs::PresentationEvent event{};
    event.session_id = 1;
    event.tick = 10;
    Check(state.raw_events.Enqueue(event), "raw event accounting fixture enqueue");
    const auto raw_pending = state.raw_events.LogicalCapacityBytes();
    state.raw_events.Prepare(10, 1);
    Check(raw_pending > 0 && state.raw_events.LogicalCapacityBytes() > 0 &&
          state.raw_events.in_flight.entries.capacity() > 0,
          "raw activation accounts pending and in-flight event storage");
}

void CheckPersistentBurstOwnerSelection()
{
    using State = hs::runtime_detail::VfxSessionState;
    using Kind = hs::PersistentVfxKind;
    constexpr std::array kinds{Kind::PlayerBowDraw, Kind::ChargedFullReady,
        Kind::EmpoweredReady, Kind::MultishotRetarget, Kind::RicochetBleedExtend};
    State state;
    state.Synchronize(7);
    Check(state.MarkPersistentBurst(12, static_cast<std::uint8_t>(kinds[3])), "preexisting burst");
    std::vector<hs::VfxPersistentInput> owners(2);
    owners[0].source_visual_kind = static_cast<std::uint8_t>(kinds[0]);
    owners[1].stable_id = 99;
    owners[1].source_visual_kind = static_cast<std::uint8_t>(Kind::FireArea);
    for (std::size_t index = 0; index < 768; ++index)
    {
        hs::VfxPersistentInput owner;
        owner.stable_id = (index * 7) % 37 + 1;
        owner.source_visual_kind = static_cast<std::uint8_t>(kinds[(index / 37) % kinds.size()]);
        owner.effect_handle = static_cast<std::uint32_t>(index + 1);
        owner.stable_seed = static_cast<std::uint32_t>(index + 100);
        owners.push_back(owner);
    }
    const auto original_selection = [&](std::span<const hs::VfxPersistentInput> inputs) {
        std::vector<std::size_t> selected;
        for (std::size_t index = 0; index < inputs.size(); ++index)
        {
            const auto &owner = inputs[index];
            const auto kind = static_cast<Kind>(owner.source_visual_kind);
            if (kind != Kind::PlayerBowDraw && kind != Kind::ChargedFullReady &&
                kind != Kind::EmpoweredReady && kind != Kind::MultishotRetarget &&
                kind != Kind::RicochetBleedExtend) continue;
            if (!owner.stable_id || state.HasPersistentBurst(owner.stable_id, owner.source_visual_kind) ||
                std::ranges::any_of(selected, [&](std::size_t prior) {
                    return inputs[prior].stable_id == owner.stable_id &&
                        inputs[prior].source_visual_kind == owner.source_visual_kind;
                })) continue;
            selected.push_back(index);
        }
        return selected;
    };
    std::vector<std::size_t> selected{9999};
    state.SelectUnseenPersistentBurstOwners(owners, selected);
    Check(selected == original_selection(owners), "batched burst selection changed first-match or source order");
    const auto *backing = selected.data();
    const auto capacity = selected.capacity();
    std::ranges::reverse(owners);
    state.SelectUnseenPersistentBurstOwners(owners, selected);
    Check(selected == original_selection(owners) && selected.data() == backing && selected.capacity() == capacity,
        "reordered burst inputs changed selection or repeated its scratch allocation");
    Check(state.PersistentBurstCount() == 1, "selection must not publish episode tombstones");
    std::vector<State::PersistentBurstKey> keys;
    for (const auto index : selected)
        keys.push_back({owners[index].stable_id, owners[index].source_visual_kind});
    std::vector<hs::ParticleSpawnCommand> commands;
    std::vector<hs::ParticleSpawnCommand> destination;
    Check(state.CommitPersistentBursts(keys, commands, destination), "selected bursts commit");
    const auto burst_capacity = state.PersistentBurstCapacityBytes();
    state.SelectUnseenPersistentBurstOwners(owners, selected);
    Check(selected.empty(), "committed burst owners were selected again");
    state.Synchronize(8);
    state.SelectUnseenPersistentBurstOwners(owners, selected);
    Check(selected == original_selection(owners) && !selected.empty() && state.PersistentBurstCount() == 0 &&
          state.PersistentBurstCapacityBytes() >= burst_capacity,
          "new session clears burst tombstones while retaining reusable scratch capacity");
}

void CheckPersistentBurstCommit()
{
    {
        hs::runtime_detail::VfxSessionState prepared;
        using Key = hs::runtime_detail::VfxSessionState::PersistentBurstKey;
        const std::array keys{Key{51, 2}, Key{52, 3}};
        const std::vector<hs::ParticleSpawnCommand> commands(2);
        std::vector<hs::ParticleSpawnCommand> destination;
        prepared.ReservePersistentBursts(keys.size());
        destination.reserve(commands.size());
        const auto capacity = prepared.PersistentBurstCapacityBytes();
        allocation_budget = 0;
        bool committed{};
        try { committed = prepared.CommitPersistentBursts(keys, commands, destination); }
        catch (const std::bad_alloc &) {}
        allocation_budget = -1;
        Check(committed && destination.size() == commands.size() &&
              prepared.PersistentBurstCapacityBytes() == capacity,
            "persistent metadata reservation did not cover non-allocating publication");
    }
    hs::runtime_detail::VfxSessionState state;
    state.Synchronize(3);
    using Key = hs::runtime_detail::VfxSessionState::PersistentBurstKey;
    const std::array empty_output_key{Key{41, 2}};
    const std::vector<hs::ParticleSpawnCommand> no_commands;
    std::vector<hs::ParticleSpawnCommand> destination;
    Check(state.CommitPersistentBursts(empty_output_key, no_commands, destination) &&
          state.HasPersistentBurst(41, 2) && destination.empty(),
          "an activated empty burst still records its persistent episode");
    Check(!state.CommitPersistentBursts(empty_output_key, no_commands, destination) &&
          !state.MarkPersistentBurst(41, 2),
          "a committed empty burst cannot be revived by later output or quality changes");

    const std::array keys{Key{42, 3}, Key{43, 4}};
    std::vector<hs::ParticleSpawnCommand> staged(2);
    staged[0].sequence = 11; staged[0].seed = 101;
    staged[1].sequence = 12; staged[1].seed = 102;
    Check(state.CommitPersistentBursts(keys, staged, destination) && destination.size() == 2 &&
          destination[0].sequence == 11 && destination[1].sequence == 12 &&
          state.HasPersistentBurst(42, 3) && state.HasPersistentBurst(43, 4),
          "burst keys and staged commands commit once in stable input order");
    const std::array out_of_order{Key{49, 1}, Key{45, 4}, Key{47, 2}};
    Check(state.CommitPersistentBursts(out_of_order, no_commands, destination) &&
          state.HasPersistentBurst(45, 4) && state.HasPersistentBurst(47, 2) &&
          state.HasPersistentBurst(49, 1) && state.PersistentBurstCount() == 6,
          "separate batches accept out-of-order keys while keeping the retained set searchable");
    const auto warm_capacity = state.PersistentBurstCapacityBytes();
    const std::array warm_key{Key{48, 3}};
    Check(state.CommitPersistentBursts(warm_key, no_commands, destination) &&
          state.PersistentBurstCapacityBytes() >= warm_capacity,
          "sorted burst scratch capacities are retained and included in the capacity report");
    Check(!state.CommitPersistentBursts(keys, staged, destination) && destination.size() == 2,
          "retrying committed burst inputs does not append commands twice");
    const std::array invalid_keys{Key{44, 5}, Key{44, 5}};
    Check(!state.CommitPersistentBursts(invalid_keys, staged, destination) &&
          !state.HasPersistentBurst(44, 5) && destination.size() == 2,
          "duplicate keys reject the batch before publishing a tombstone or commands");
    const std::array zero_key{Key{0, 5}};
    Check(!state.CommitPersistentBursts(zero_key, no_commands, destination) &&
          !state.HasPersistentBurst(0, 5) && state.PersistentBurstCount() == 7,
          "zero stable ids reject the batch without changing retained tombstones");

    hs::runtime_detail::VfxSessionState allocation_failure;
    allocation_failure.Synchronize(9);
    const std::array allocation_warm_keys{Key{89, 1}, Key{88, 1}};
    Check(allocation_failure.CommitPersistentBursts(allocation_warm_keys, no_commands,
          allocation_failure.decoded.particle_spawns), "warm burst scratch capacities before fault injection");
    const std::array second_warm_key{Key{90, 1}};
    Check(allocation_failure.CommitPersistentBursts(second_warm_key, no_commands,
        allocation_failure.decoded.particle_spawns), "warm both alternating burst key buffers");
    const std::array allocation_key{Key{91, 1}};
    std::vector<hs::ParticleSpawnCommand> allocation_destination(1);
    allocation_destination[0].seed = 991;
    const auto allocation_destination_size = allocation_destination.size();
    const auto burst_capacity_before_failure = allocation_failure.PersistentBurstCapacityBytes();
    std::vector<hs::ParticleSpawnCommand> allocation_commands(
        allocation_destination.capacity() - allocation_destination.size() + 1);
    allocation_budget = 0;
    bool failed_allocation{};
    try { (void)allocation_failure.CommitPersistentBursts(allocation_key, allocation_commands, allocation_destination); }
    catch (const std::bad_alloc &) { failed_allocation = true; }
    allocation_budget = -1;
    Check(failed_allocation && allocation_failure.PersistentBurstCount() == 3 &&
          allocation_failure.HasPersistentBurst(89, 1) && allocation_failure.HasPersistentBurst(88, 1) &&
          allocation_failure.HasPersistentBurst(90, 1) && !allocation_failure.HasPersistentBurst(91, 1) &&
          allocation_destination.size() == allocation_destination_size && allocation_destination[0].seed == 991 &&
          allocation_failure.PersistentBurstCapacityBytes() == burst_capacity_before_failure,
          "destination allocation failure rolls back new tombstones and commands while preserving warm capacities");
    bool reused_without_allocation{};
    allocation_budget=0;
    try { reused_without_allocation=allocation_failure.CommitPersistentBursts(
        allocation_key,no_commands,allocation_failure.decoded.particle_spawns); }
    catch(const std::bad_alloc &) {}
    allocation_budget=-1;
    Check(reused_without_allocation&&allocation_failure.HasPersistentBurst(91,1)&&
        allocation_failure.PersistentBurstCapacityBytes()==burst_capacity_before_failure,
        "warmed burst publication allocated a fresh key node or scratch backing");
}

void CheckPersistentBurstRetirement()
{
    using State = hs::runtime_detail::VfxSessionState;
    using Key = State::PersistentBurstKey;
    const auto retire = [](State &owner, std::span<const Key> current,
                           std::span<const Key> previous, bool has_current, hs::Tick tick) {
        owner.RetireUnownedPersistentBursts(has_current, tick, [&](const Key &key) noexcept {
            return std::ranges::find(current, key) != current.end() ||
                std::ranges::find(previous, key) != previous.end();
        });
    };
    constexpr auto kind = static_cast<std::uint8_t>(hs::PersistentVfxKind::ChargedFullReady);
    State state;
    state.Synchronize(5);
    const std::array first_episode{Key{501, kind}, Key{503, kind}};
    const std::vector<hs::ParticleSpawnCommand> no_commands;
    std::vector<hs::ParticleSpawnCommand> destination;
    Check(state.CommitPersistentBursts(first_episode, no_commands, destination),
          "first status episode commits");
    const std::array current_owner{Key{501, kind}};
    retire(state, current_owner, {}, true, 100);
    Check(state.HasPersistentBurst(501, kind) && !state.HasPersistentBurst(503, kind),
          "retiring an absent key preserves sorted lookup for the current owner");
    retire(state, {}, current_owner, true, 101);
    Check(state.HasPersistentBurst(501, kind), "previous interpolation owner retains its tombstone");
    retire(state, {}, {}, false, 102);
    Check(state.HasPersistentBurst(501, kind), "a skipped current snapshot cannot retire an episode");
    retire(state, {}, {}, true, 102);
    Check(!state.HasPersistentBurst(501, kind), "absence from both authoritative samples retires an ended episode");

    const std::array next_episode{Key{502, kind}};
    Check(state.CommitPersistentBursts(next_episode, no_commands, destination) &&
          state.HasPersistentBurst(502, kind), "a new stable episode can commit after retirement");
    retire(state, next_episode, {}, true, 102);
    retire(state, next_episode, {}, true, 90);
    Check(!state.HasPersistentBurst(502, kind), "same-session timeline rewind explicitly clears status tombstones");

    State frozen;
    frozen.Synchronize(6);
    hs::ParticleSpawnCommand command;
    command.tick = 20; command.authored_birth_tick = 19; command.seed = 77;
    const std::vector staged{command};
    const std::array frozen_key{Key{601, kind}};
    Check(frozen.CommitPersistentBursts(frozen_key, staged, frozen.decoded.particle_spawns),
          "frozen-batch status command commits");
    auto *batch = frozen.PrepareNativeBatch(20, true);
    Check(batch && batch->particle_spawns.size() == 1, "status command enters a frozen native batch");
    const auto identity = batch->identity;
    retire(frozen, {}, {}, true, 21);
    Check(!frozen.HasPersistentBurst(601, kind) && frozen.native_batch_active &&
          frozen.native_batch.particle_spawns[0].seed == 77 &&
          frozen.AcknowledgeNativeBatch(identity, true),
          "retiring CPU episode identity leaves copied frozen native work and its receipt independent");
}
}

void CheckPendingEmissionOrder()
{
    hs::runtime_detail::DecodedVfxState state;
    const auto append = [&](hs::Tick due, hs::Sequence sequence, hs::Tick birth) {
        hs::ParticleSpawnCommand command;
        command.tick = due; command.sequence = sequence; command.authored_birth_tick = birth;
        state.particle_spawns.push_back(command);
    };
    append(120, 1, 100);
    state.SortNewParticleSpawns(0);
    append(101, 2, 101);
    append(120, 3, 120);
    state.SortNewParticleSpawns(1);
    Check(state.particle_spawns[0].sequence == 2 && state.particle_spawns[1].sequence == 1 &&
        state.particle_spawns[2].sequence == 3 && state.particle_spawns[1].authored_birth_tick == 100,
        "delayed activation reordered equal-due births or changed absolute birth time");
    const auto *retained = state.particle_spawns.data();
    state.SortNewParticleSpawns(state.particle_spawns.size());
    Check(state.particle_spawns.data() == retained, "unchanged pending queue replaced its storage");
    state.particle_spawns.erase(state.particle_spawns.begin());
    append(120, 4, 120);
    append(100, 5, 100);
    state.SortNewParticleSpawns(2);
    Check(state.particle_spawns[0].sequence == 5 && state.particle_spawns[1].sequence == 1 &&
        state.particle_spawns[2].sequence == 3 && state.particle_spawns[3].sequence == 4,
        "accepted-prefix retirement lost stable pending/new due ordering");
}

void CheckPreparedBirthAppend()
{
    hs::runtime_detail::VfxSessionState state;
    hs::runtime_detail::DecodedVfxState prepared;
    state.decoded.particle_spawns.push_back({.sequence = 10, .tick = 1});
    state.decoded.effect_lines.push_back({.sequence = 20, .tick = 2});
    state.decoded.flashes.push_back({.stable_seed = 30});

    prepared.particle_spawns.push_back({.sequence = 40, .tick = 3});
    prepared.particle_spawns.push_back({.sequence = 50, .tick = 4});
    prepared.effect_lines.push_back({.sequence = 60, .tick = 5});
    prepared.flashes.push_back({.stable_seed = 70});
    const auto staged_capacities = std::array{
        prepared.particle_spawns.capacity(), prepared.effect_lines.capacity(), prepared.flashes.capacity()};
    Check(state.AppendPreparedBirths(prepared), "prepared births must append when scratch is independent");
    Check(state.decoded.particle_spawns.size() == 3 &&
          state.decoded.particle_spawns[0].sequence == 10 &&
          state.decoded.particle_spawns[1].sequence == 40 &&
          state.decoded.particle_spawns[2].sequence == 50 &&
          state.decoded.effect_lines.size() == 2 && state.decoded.effect_lines[1].sequence == 60 &&
          state.decoded.flashes.size() == 2 && state.decoded.flashes[1].stable_seed == 70,
          "prepared append must preserve retained prefixes and staged order");
    Check(state.decoded.particle_spawns.capacity() >= 3 &&
          state.decoded.effect_lines.capacity() >= 2 && state.decoded.flashes.capacity() >= 2,
          "all destination capacities must cover committed births");
    Check(prepared.particle_spawns.empty() && prepared.effect_lines.empty() && prepared.flashes.empty() &&
          prepared.particle_spawns.capacity() == staged_capacities[0] &&
          prepared.effect_lines.capacity() == staged_capacities[1] &&
          prepared.flashes.capacity() == staged_capacities[2],
          "successful commit clears scratch while retaining its reusable capacity");
    Check(!state.AppendPreparedBirths(state.decoded) && state.decoded.particle_spawns.size() == 3,
          "retained output cannot be used as its own prepared source");

    hs::runtime_detail::VfxSessionState rejected;
    hs::runtime_detail::DecodedVfxState stage;
    stage.particle_spawns.push_back({.sequence = 80, .tick = 8});
    const auto accepted_prefix = stage.MarkBirths();
    stage.particle_spawns.push_back({.sequence = 90, .tick = 9});
    stage.effect_lines.push_back({.sequence = 100, .tick = 10});
    stage.flashes.push_back({.stable_seed = 110});
    stage.RollbackBirths(accepted_prefix);
    Check(rejected.AppendPreparedBirths(stage) &&
          rejected.decoded.particle_spawns.size() == 1 &&
          rejected.decoded.particle_spawns[0].sequence == 80 &&
          rejected.decoded.effect_lines.empty() && rejected.decoded.flashes.empty(),
          "rejected per-event output must roll back to the accepted scratch prefix");

    hs::runtime_detail::VfxSessionState vacant;
    hs::runtime_detail::DecodedVfxState owned_stage;
    vacant.decoded.particle_spawns.reserve(16);
    vacant.decoded.effect_lines.reserve(16);
    vacant.decoded.flashes.reserve(16);
    owned_stage.particle_spawns.push_back({.sequence = 120});
    owned_stage.effect_lines.push_back({.sequence = 130});
    owned_stage.flashes.push_back({.stable_seed = 140});
    owned_stage.entity_surface_captures.push_back({.row = 9});
    const auto *particle_storage = owned_stage.particle_spawns.data();
    const auto *line_storage = owned_stage.effect_lines.data();
    const auto *flash_storage = owned_stage.flashes.data();
    Check(vacant.AppendPreparedBirths(owned_stage) &&
          vacant.decoded.particle_spawns.data() == particle_storage &&
          vacant.decoded.effect_lines.data() == line_storage &&
          vacant.decoded.flashes.data() == flash_storage &&
          owned_stage.particle_spawns.capacity() >= 16 &&
          owned_stage.effect_lines.capacity() >= 16 && owned_stage.flashes.capacity() >= 16 &&
          owned_stage.entity_surface_captures.size() == 1 &&
          owned_stage.entity_surface_captures.front().row == 9,
          "empty queues must accept staged backing without copying or consuming unrelated captures");

    hs::runtime_detail::VfxSessionState empty;
    hs::runtime_detail::DecodedVfxState no_births;
    Check(empty.AppendPreparedBirths(no_births) && empty.decoded.particle_spawns.empty() &&
          empty.decoded.effect_lines.empty() && empty.decoded.flashes.empty(),
          "empty prepared stage is a successful no-op");

    hs::runtime_detail::VfxSessionState warm_empty;
    hs::runtime_detail::DecodedVfxState smaller_empty_stage;
    warm_empty.decoded.particle_spawns.reserve(32);
    warm_empty.decoded.effect_lines.reserve(32);
    warm_empty.decoded.flashes.reserve(32);
    smaller_empty_stage.particle_spawns.reserve(16);
    smaller_empty_stage.effect_lines.reserve(16);
    smaller_empty_stage.flashes.reserve(16);
    const auto *warm_particle_storage = warm_empty.decoded.particle_spawns.data();
    const auto *warm_line_storage = warm_empty.decoded.effect_lines.data();
    const auto *warm_flash_storage = warm_empty.decoded.flashes.data();
    Check(warm_empty.AppendPreparedBirths(smaller_empty_stage) &&
          warm_empty.decoded.particle_spawns.data() == warm_particle_storage &&
          warm_empty.decoded.effect_lines.data() == warm_line_storage &&
          warm_empty.decoded.flashes.data() == warm_flash_storage &&
          warm_empty.decoded.particle_spawns.capacity() >= 32 &&
          warm_empty.decoded.effect_lines.capacity() >= 32 &&
          warm_empty.decoded.flashes.capacity() >= 32,
          "empty prepared transfer must preserve a larger warm destination allocation");

    hs::runtime_detail::VfxSessionState coordinated;
    using Store = hs::runtime_detail::ActiveEffectStore;
    hs::VfxEventInput event;
    event.sequence = 150;
    const std::array requests{Store::AppendRequest{&event,
        static_cast<std::uint16_t>((1u << static_cast<unsigned>(Store::Profile::EntitySurface)) |
                                  (1u << static_cast<unsigned>(Store::Profile::Light)))}};
    Store::PreparedAppendBatch profiles;
    hs::runtime_detail::DecodedVfxState births;
    births.flashes.push_back({.stable_seed = 160});
    Check(coordinated.active_effects.PrepareAppendBatch(requests, profiles) &&
          coordinated.active_effects.Size() == 0 && coordinated.decoded.flashes.empty(),
          "profile preparation must not publish rows or decoded births");
    Check(coordinated.AppendPreparedBirths(births) &&
          coordinated.active_effects.CommitAppendBatch(profiles) &&
          coordinated.active_effects.Size() == 1 && coordinated.decoded.flashes.size() == 1 &&
          coordinated.active_effects.ProfileEvents(Store::Profile::EntitySurface).size() == 1 &&
          coordinated.active_effects.ProfileEvents(Store::Profile::Light).size() == 1,
          "publishing independently prepared births must preserve the adjacent profile commit");
}

void CheckEmptyPreparedShardCapacityTransfer()
{
    using State = hs::runtime_detail::VfxSessionState;
    using hs::runtime_detail::vfx_birth_admission::ReserveAndSelectBirthRows;
    using hs::runtime_detail::vfx_retained_size::BirthRequest;

    State state;
    state.Synchronize(21);
    hs::PresentationEvent raw{};
    raw.session_id = 21;
    raw.tick = 1;
    raw.sequence = 101;
    Check(state.raw_events.Enqueue(raw) && state.raw_events.Prepare(1, 21).entries.size() == 1,
        "status shard fixture must have a ready raw activation");

    constexpr std::size_t shard_count = 2;
    hs::runtime_detail::DecodedVfxState prepared_births;
    std::array<std::size_t, 3> required_rows{shard_count, 0, 0};
    std::vector<BirthRequest> no_decorative_requests;
    std::vector<std::size_t> order;
    std::vector<hs::VfxFlashSpawnInput> visible;
    ReserveAndSelectBirthRows(required_rows, no_decorative_requests, order,
        []() noexcept { return std::size_t{}; },
        prepared_births.particle_spawns, state.decoded.particle_spawns,
        state.native_batch.particle_spawns,
        prepared_births.effect_lines, state.decoded.effect_lines,
        state.native_batch.effect_lines,
        prepared_births.flashes, state.decoded.flashes, visible, 0, 0);
    Check(prepared_births.particle_spawns.empty() &&
          prepared_births.particle_spawns.capacity() >= shard_count &&
          state.decoded.particle_spawns.empty() && state.decoded.particle_spawns.capacity() == 0,
        "required status shards must reserve prepared storage without fabricating birth rows");
    const auto *prepared_storage = prepared_births.particle_spawns.data();

    const std::array keys{State::PersistentBurstKey{901, 3}};
    state.ReservePersistentBursts(keys.size());
    std::vector<hs::ParticleSpawnCommand> status_shards;
    status_shards.reserve(shard_count);
    status_shards.push_back({.sequence = 201, .tick = 1});
    status_shards.push_back({.sequence = 202, .tick = 1});
    hs::runtime_detail::ActiveEffectStore::PreparedAppendBatch profiles;

    bool committed{};
    bool shards_committed{};
    allocation_budget = 0;
    try
    {
        committed = state.CommitPreparedActivation(prepared_births, profiles, false);
        shards_committed = committed && state.CommitPersistentBursts(
            keys, status_shards, state.decoded.particle_spawns);
    }
    catch (const std::bad_alloc &)
    {
        allocation_budget = -1;
        throw std::runtime_error("pre-reserved empty status shards allocated after activation commit");
    }
    catch (...)
    {
        allocation_budget = -1;
        throw;
    }
    allocation_budget = -1;
    Check(committed && shards_committed && state.decoded.particle_spawns.data() == prepared_storage &&
          state.decoded.particle_spawns.size() == shard_count &&
          state.decoded.particle_spawns[0].sequence == 201 &&
          state.decoded.particle_spawns[1].sequence == 202 &&
          state.PersistentBurstCount() == 1 && state.raw_events.in_flight.entries.empty(),
        "empty prepared capacity must transfer before status shard append and raw acknowledgement");

    State stale;
    stale.Synchronize(22);
    raw.session_id = 22;
    raw.sequence = 102;
    Check(stale.raw_events.Enqueue(raw) && stale.raw_events.Prepare(1, 22).entries.size() == 1,
        "stale shard fixture must have a ready raw activation");
    hs::runtime_detail::DecodedVfxState stale_births;
    std::array<std::size_t, 3> stale_required_rows{shard_count, 0, 0};
    std::vector<BirthRequest> stale_requests;
    std::vector<std::size_t> stale_order;
    ReserveAndSelectBirthRows(stale_required_rows, stale_requests, stale_order,
        []() noexcept { return std::size_t{}; },
        stale_births.particle_spawns, stale.decoded.particle_spawns, stale.native_batch.particle_spawns,
        stale_births.effect_lines, stale.decoded.effect_lines, stale.native_batch.effect_lines,
        stale_births.flashes, stale.decoded.flashes, visible, 0, 0);
    Check(stale_births.particle_spawns.empty() && stale_births.particle_spawns.capacity() >= shard_count,
        "stale fixture must own a reserved empty required-shard buffer");
    const auto *stale_prepared_storage = stale_births.particle_spawns.data();
    const auto *stale_destination_storage = stale.decoded.particle_spawns.data();
    hs::runtime_detail::ActiveEffectStore::PreparedAppendBatch stale_profiles;
    hs::VfxEventInput profile_input;
    profile_input.sequence = raw.sequence;
    const std::array profile_requests{hs::runtime_detail::ActiveEffectStore::AppendRequest{
        &profile_input, static_cast<std::uint16_t>(1u << static_cast<unsigned>(
            hs::runtime_detail::ActiveEffectStore::Profile::Screen))}};
    Check(stale.active_effects.PrepareAppendBatch(profile_requests, stale_profiles),
        "stale shard fixture profile preparation failed");
    hs::VfxEventInput concurrent_profile;
    concurrent_profile.sequence = 103;
    stale.active_effects.Append(hs::runtime_detail::ActiveEffectStore::Profile::Screen, concurrent_profile);
    Check(!stale.CommitPreparedActivation(stale_births, stale_profiles, true) &&
          stale_births.particle_spawns.data() == stale_prepared_storage &&
          stale.decoded.particle_spawns.data() == stale_destination_storage &&
          stale.decoded.particle_spawns.empty() && stale.raw_events.Prepare(1, 22).entries.size() == 1,
        "stale activation must not transfer empty prepared capacity or acknowledge its raw event");
}

void CheckPreparedSurfaceCaptureReservation()
{
    using State = hs::runtime_detail::VfxSessionState;
    using Store = hs::runtime_detail::ActiveEffectStore;
    State state;
    state.Synchronize(7);
    hs::PresentationEvent raw;
    raw.session_id = 7;
    raw.tick = 1;
    raw.sequence = 91;
    Check(state.raw_events.Enqueue(raw) && state.raw_events.Prepare(1, 7).entries.size() == 1,
        "surface capture fixture did not retain its raw activation");
    hs::VfxEventInput input;
    input.sequence = raw.sequence;
    const std::array requests{Store::AppendRequest{&input,
        static_cast<std::uint16_t>(1u << static_cast<unsigned>(Store::Profile::EntitySurface))}};
    Store::PreparedAppendBatch profiles;
    hs::runtime_detail::DecodedVfxState births;
    Check(state.active_effects.PrepareAppendBatch(requests, profiles) &&
        profiles.AdditionalProfileRows(Store::Profile::EntitySurface) == 1,
        "surface capture fixture did not prepare its exact membership count");

    bool failed{};
    allocation_budget = 0;
    try { (void)state.CommitPreparedActivation(births, profiles, true); }
    catch (const std::bad_alloc &) { failed = true; }
    allocation_budget = -1;
    Check(failed && state.active_effects.Size() == 0 &&
        state.active_effects.CanCommitAppendBatch(profiles) &&
        state.raw_events.in_flight.entries.size() == 1 &&
        state.decoded.entity_surface_captures.empty(),
        "surface capture allocation failure published profiles or acknowledged raw input");

    state.ReserveSurfaceCaptures(profiles.AdditionalProfileRows(Store::Profile::EntitySurface));
    Check(state.active_effects.Size() == 0 && state.raw_events.in_flight.entries.size() == 1 &&
        state.decoded.entity_surface_captures.capacity() >= 1,
        "surface capture preflight published activation before birth preparation");
    allocation_budget = 0;
    const bool committed = state.CommitPreparedActivation(births, profiles, true);
    allocation_budget = -1;
    Check(committed &&
        state.raw_events.in_flight.entries.empty() &&
        state.active_effects.ProfileEvents(Store::Profile::EntitySurface).size() == 1 &&
        state.decoded.entity_surface_captures.empty() &&
        state.decoded.entity_surface_captures.capacity() >= 1,
        "surface activation retry did not reserve its eventual retained capture");
    const auto capacity = state.decoded.entity_surface_captures.capacity();
    allocation_budget = 0;
    state.decoded.entity_surface_captures.push_back({});
    allocation_budget = -1;
    Check(state.decoded.entity_surface_captures.capacity() == capacity,
        "surface capture transfer allocated after CPU acceptance");
}

void CheckPreparedActivationCommit()
{
    using State = hs::runtime_detail::VfxSessionState;
    using Store = hs::runtime_detail::ActiveEffectStore;

    State state;
    state.Synchronize(7);
    hs::ParticleSpawnCommand native_command;
    native_command.sequence = 70;
    native_command.tick = 1;
    state.decoded.particle_spawns.push_back(native_command);
    auto *native = state.PrepareNativeBatch(1, false);
    Check(native && native->particle_spawns.size() == 1,
        "native work must be independently frozen before CPU activation");
    const auto native_identity = native->identity;
    state.decoded.particle_spawns.reserve(1);
    const auto pending_capacity = state.decoded.particle_spawns.capacity();
    for (std::size_t i = 0; i < pending_capacity; ++i)
        state.decoded.particle_spawns.push_back({.sequence = static_cast<std::uint64_t>(80 + i), .tick = 2});

    hs::PresentationEvent event;
    event.session_id = 7;
    event.tick = 1;
    event.sequence = 1;
    Check(state.raw_events.Enqueue(event) && state.raw_events.Prepare(1, 7).entries.size() == 1,
        "raw activation must be ready before prepared commit");
    hs::VfxEventInput profile_input;
    profile_input.sequence = 90;
    const std::array requests{Store::AppendRequest{&profile_input,
        static_cast<std::uint16_t>(1u << static_cast<unsigned>(Store::Profile::Ground))}};
    Store::PreparedAppendBatch profiles;
    Check(state.active_effects.PrepareAppendBatch(requests, profiles),
        "profile batch must prepare before activation commit");
    hs::runtime_detail::DecodedVfxState births;
    births.particle_spawns.push_back({.sequence = 100, .tick = 3});

    allocation_budget = 0;
    bool allocation_failed{};
    try
    {
        (void)state.CommitPreparedActivation(births, profiles, true);
    }
    catch (const std::bad_alloc &)
    {
        allocation_failed = true;
    }
    allocation_budget = -1;
    Check(allocation_failed && state.decoded.particle_spawns.size() == pending_capacity &&
          state.decoded.particle_spawns[0].sequence == 80 && state.active_effects.Size() == 0 &&
          births.particle_spawns.size() == 1 && state.raw_events.Prepare(1, 7).entries.size() == 1,
        "allocation failure must leave birth/profile contents and raw activation unpublished");

    Check(state.CommitPreparedActivation(births, profiles, true) &&
          state.decoded.particle_spawns.size() == pending_capacity + 1 &&
          state.decoded.particle_spawns[pending_capacity].sequence == 100 &&
          state.active_effects.Size() == 1 && state.raw_events.Prepare(1, 7).entries.empty(),
        "successful activation publishes births and profiles before acknowledging raw input");
    Check(state.native_batch_active && state.native_batch.particle_spawns.size() == 1 &&
          state.native_batch.particle_spawns[0].sequence == 70 &&
          !state.AcknowledgeNativeBatch(native_identity, false) && state.native_batch_active,
        "CPU activation acknowledgement must leave unsubmitted native work pending");

    State stale;
    stale.Synchronize(8);
    event.session_id = 8;
    Check(stale.raw_events.Enqueue(event) && stale.raw_events.Prepare(1, 8).entries.size() == 1,
        "stale fixture raw activation must be ready");
    profile_input.sequence = 91;
    const std::array stale_requests{Store::AppendRequest{&profile_input,
        static_cast<std::uint16_t>(1u << static_cast<unsigned>(Store::Profile::Ground))}};
    Store::PreparedAppendBatch stale_profiles;
    Check(stale.active_effects.PrepareAppendBatch(stale_requests, stale_profiles),
        "stale fixture profiles must prepare");
    hs::VfxEventInput concurrent;
    concurrent.sequence = 92;
    stale.active_effects.Append(Store::Profile::Screen, concurrent);
    hs::runtime_detail::DecodedVfxState stale_births;
    stale_births.flashes.push_back({.stable_seed = 93});
    Check(!stale.CommitPreparedActivation(stale_births, stale_profiles, true) &&
          stale.active_effects.Size() == 1 && stale.decoded.flashes.empty() &&
          stale_births.flashes.size() == 1 && stale.raw_events.Prepare(1, 8).entries.size() == 1,
        "stale profiles must reject activation before births or raw acknowledgement");

    State cancelled;
    cancelled.Synchronize(9);
    event.session_id = 9;
    Check(cancelled.raw_events.Enqueue(event) && cancelled.raw_events.Prepare(1, 9).entries.size() == 1,
        "cancel fixture raw activation must be ready");
    Store::PreparedAppendBatch cancelled_profiles;
    hs::runtime_detail::DecodedVfxState cancelled_births;
    cancelled_births.flashes.push_back({.stable_seed = 94});
    cancelled.active_effects.CancelAppendBatch(cancelled_profiles);
    Check(!cancelled.CommitPreparedActivation(cancelled_births, cancelled_profiles, true) &&
          cancelled.decoded.flashes.empty() && cancelled_births.flashes.size() == 1 &&
          cancelled.raw_events.Prepare(1, 9).entries.size() == 1,
        "cancelled profiles must reject activation before births or raw acknowledgement");

    State alias;
    alias.Synchronize(10);
    event.session_id = 10;
    Check(alias.raw_events.Enqueue(event) && alias.raw_events.Prepare(1, 10).entries.size() == 1,
        "alias fixture raw activation must be ready");
    Check(!alias.CommitPreparedActivation(alias.decoded, cancelled_profiles, false) &&
          alias.raw_events.Prepare(1, 10).entries.size() == 1,
        "aliased birth source must reject activation before raw acknowledgement");
}

void CheckActivationBeforeNativeEmission()
{
    hs::runtime_detail::VfxSessionState state;
    state.Synchronize(7);
    hs::PresentationEvent first;
    first.session_id = 7; first.tick = 100; first.sequence = 1;
    Check(state.raw_events.Enqueue(first), "first raw activation enqueue");
    const auto &activation = state.raw_events.Prepare(100, 7);
    Check(activation.entries.size() == 1, "raw event did not reach CPU activation");
    hs::ParticleSpawnCommand delayed;
    delayed.sequence = activation.entries[0].event.sequence;
    delayed.tick = 120; delayed.authored_birth_tick = 120;
    state.decoded.particle_spawns.push_back(delayed);
    state.raw_events.AcknowledgeActivation();
    Check(state.raw_events.Prepare(100, 7).entries.empty() &&
        state.decoded.particle_spawns.size() == 1 && state.decoded.particle_spawns[0].tick == 120,
        "native delay replayed raw activation or lost its owned emission");
    auto second = first;
    second.tick = 101; second.sequence = 2;
    Check(state.raw_events.Enqueue(second) && state.raw_events.Prepare(101, 7).entries.size() == 1,
        "pending native emission blocked the next CPU activation");
    auto immediate = delayed;
    immediate.tick = 101; immediate.authored_birth_tick = 101; immediate.sequence = 2;
    state.decoded.particle_spawns.push_back(immediate);
    state.decoded.SortNewParticleSpawns(1);
    state.raw_events.AcknowledgeActivation();
    Check(state.decoded.particle_spawns[0].sequence == 2 &&
        state.decoded.particle_spawns[1].sequence == 1 && state.raw_events.Prepare(101, 7).entries.empty(),
        "CPU activation order replaced native due order or activated twice");
}

void CheckFrozenNativeBatches()
{
    hs::runtime_detail::VfxSessionState state;
    state.Synchronize(7);
    const auto particle = [&](hs::Tick due, hs::Sequence sequence, hs::Tick birth) {
        hs::ParticleSpawnCommand command;
        command.tick = due; command.sequence = sequence; command.authored_birth_tick = birth;
        state.decoded.particle_spawns.push_back(command);
    };
    particle(120, 1, 100); // A: activated at 100, authored delay 20.
    particle(101, 2, 101); // B: activated at 101, immediate.
    hs::VfxLineSpawnCommand line;
    line.tick = 101; line.sequence = 4;
    state.decoded.effect_lines.push_back(line);
    state.decoded.SortNewParticleSpawns(0);
    Check(state.decoded.particle_spawns[0].sequence == 2 &&
          state.decoded.particle_spawns[1].sequence == 1,
          "delayed source A must remain due after immediate source B");

    auto *batch = state.PrepareNativeBatch(101, true);
    Check(batch && batch->identity.session == 7 && batch->identity.stateful_target == 101 &&
          batch->particle_spawns.size() == 1 && batch->particle_spawns[0].sequence == 2 &&
          batch->particle_spawns[0].authored_birth_tick == 101 && batch->effect_lines.size() == 1 &&
          batch->effect_lines[0].sequence == 4,
          "batch freezes only the due prefix and preserves absolute birth");
    const auto identity = batch->identity;
    const auto particle_storage = batch->particle_spawns.data();
    const auto particle_capacity = batch->particle_spawns.capacity();
    particle(102, 3, 102);
    // Mirror the app's retained-prefix sort before freezing newly delivered rows.
    state.decoded.SortNewParticleSpawns(1);
    auto *retry = state.PrepareNativeBatch(102, true);
    Check(retry == batch && retry->identity.sequence == identity.sequence &&
          retry->identity.stateful_target == 101 && retry->particle_spawns.size() == 1 &&
          state.decoded.particle_spawns.size() == 2,
          "retry holds the batch while newer emissions remain pending");
    auto wrong = identity;
    ++wrong.sequence;
    auto wrong_target = identity;
    ++wrong_target.stateful_target;
    auto wrong_session = identity;
    ++wrong_session.session;
    Check(!state.AcknowledgeNativeBatch(wrong, true) &&
          !state.AcknowledgeNativeBatch(wrong_target, true) &&
          !state.AcknowledgeNativeBatch(wrong_session, true) &&
          !state.AcknowledgeNativeBatch(identity, false) && state.native_batch_active,
          "only a submitted exact identity acknowledges the batch");
    Check(state.AcknowledgeNativeBatch(identity, true) &&
          state.last_committed_stateful_target == 101 && !state.native_batch_active &&
          state.native_batch.particle_spawns.capacity() == particle_capacity,
          "matching submission advances the committed target and retires the batch");
    batch = state.PrepareNativeBatch(102, true);
    Check(batch && batch->particle_spawns.size() == 1 && batch->particle_spawns[0].sequence == 3,
          "next batch consumes the newly due pending emission");
    const auto second_identity = batch->identity;
    Check(state.AcknowledgeNativeBatch(second_identity, true), "second batch submitted");

    const auto no_batch_sequence = state.next_native_batch_sequence;
    Check(state.PrepareNativeBatch(102, false) == nullptr &&
          state.next_native_batch_sequence == no_batch_sequence,
          "no emission and no world step creates no batch or consumes its identity");
    batch = state.PrepareNativeBatch(103, true);
    Check(batch && batch->particle_spawns.empty() && batch->effect_lines.empty(),
          "world step can be submitted with empty emission channels");
    const auto step_identity = batch->identity;
    Check(state.AcknowledgeNativeBatch(step_identity, true), "empty world-step batch submission");

    batch = state.PrepareNativeBatch(104, true);
    const auto before_epoch_change = batch->identity;
    state.UpdateTerminalClock(104, hs::SessionPhase::Victory, hs::FixedStepClock::Clock::time_point{});
    Check(state.PrepareNativeBatch(104, true)->identity.sequence == before_epoch_change.sequence,
          "display clock changes do not cancel a frozen batch");
    batch = state.PrepareNativeBatch(90, true);
    Check(batch && batch->identity.stateful_target == 90 && batch->identity.reset_particle_state &&
          state.native_batch_active,
          "same-session rewind cancels old batch and requests particle reset");
    const auto rewind_identity = batch->identity;
    batch = state.PrepareNativeBatch(90, true);
    Check(batch->identity.sequence == rewind_identity.sequence &&
          batch->identity.stateful_target == rewind_identity.stateful_target &&
          batch->particle_spawns.data() == particle_storage &&
          batch->particle_spawns.capacity() == particle_capacity,
          "rewind retry preserves identity and retained batch storage");
    batch = state.PrepareNativeBatch(91, true);
    Check(batch->identity.sequence == rewind_identity.sequence &&
          batch->identity.stateful_target == rewind_identity.stateful_target,
          "newer target cannot replace an unsubmitted rewind retry");
    Check(state.AcknowledgeNativeBatch(rewind_identity, true) && !state.particle_reset_pending,
          "submitted rewind reset clears its pending reset request");

    batch = state.PrepareNativeBatch(91, true);
    const auto discarded_identity = batch->identity;
    state.DiscardNativeBatch();
    Check(!state.native_batch_active && state.last_committed_stateful_target == 90 &&
          !state.AcknowledgeNativeBatch(discarded_identity, true),
          "discarded catalog output must not advance the committed target or accept a late receipt");
    state.PrepareNativeBatch(91, true);
    Check(state.Synchronize(8) && !state.native_batch_active && !state.has_committed_stateful_target &&
          !state.particle_reset_pending,
          "session synchronization discards batch and target state");

    hs::runtime_detail::VfxSessionState all_due;
    all_due.Synchronize(9);
    hs::ParticleSpawnCommand birth;
    birth.tick = 200; birth.authored_birth_tick = 199; birth.position = {1, 2, 3}; birth.seed = 41;
    all_due.decoded.particle_spawns.push_back(birth);
    const auto *prepared_storage = all_due.decoded.particle_spawns.data();
    auto *frozen = all_due.PrepareNativeBatch(200, true);
    Check(frozen && frozen->particle_spawns.data() == prepared_storage &&
              all_due.decoded.particle_spawns.empty(), "an entirely due channel transfers ownership without copying");
    birth.tick = 201; birth.position = {8, 9, 10}; birth.seed = 42;
    all_due.decoded.particle_spawns.push_back(birth);
    Check(all_due.PrepareNativeBatch(201, true) == frozen && frozen->particle_spawns.data() == prepared_storage &&
              frozen->particle_spawns[0].position.x == 1 && frozen->particle_spawns[0].seed == 41 &&
              frozen->particle_spawns[0].authored_birth_tick == 199 && all_due.decoded.particle_spawns.size() == 1,
          "retry retains the first prepared world-space birth while newer owner data stays pending");
}

void CheckNativeBatchReusesPreparedStorage()
{
    hs::runtime_detail::VfxSessionState state;
    state.Synchronize(77);
    hs::ParticleSpawnCommand particle;
    particle.tick = 1; particle.sequence = 11; particle.seed = 101;
    state.decoded.particle_spawns.push_back(particle);
    particle.tick = 2; particle.sequence = 12; particle.seed = 102;
    state.decoded.particle_spawns.push_back(particle);
    particle.tick = 8; particle.sequence = 13; particle.seed = 103;
    state.decoded.particle_spawns.push_back(particle);
    hs::VfxLineSpawnCommand line;
    line.tick = 1; line.sequence = 21;
    state.decoded.effect_lines.push_back(line);
    line.tick = 8; line.sequence = 22;
    state.decoded.effect_lines.push_back(line);

    hs::runtime_detail::DecodedVfxState scratch;
    scratch.particle_spawns.reserve(2);
    scratch.effect_lines.reserve(1);
    const auto *particle_storage = scratch.particle_spawns.data();
    const auto *line_storage = scratch.effect_lines.data();
    allocation_budget = 0;
    auto *batch = state.PrepareNativeBatch(2, true, &scratch);
    allocation_budget = -1;
    Check(batch && batch->particle_spawns.data() == particle_storage &&
          batch->effect_lines.data() == line_storage &&
          batch->particle_spawns.size() == 2 && batch->particle_spawns[0].sequence == 11 &&
          batch->particle_spawns[1].sequence == 12 && batch->effect_lines.size() == 1 &&
          batch->effect_lines[0].sequence == 21 &&
          state.decoded.particle_spawns.size() == 1 && state.decoded.particle_spawns[0].sequence == 13 &&
          state.decoded.effect_lines.size() == 1 && state.decoded.effect_lines[0].sequence == 22 &&
          scratch.particle_spawns.empty() && scratch.effect_lines.empty(),
          "mixed native freeze did not reuse prepared capacity or preserve future tails");
    const auto identity = batch->identity;
    allocation_budget = 0;
    auto *retry = state.PrepareNativeBatch(3, true, &scratch);
    allocation_budget = -1;
    Check(retry == batch && retry->identity.sequence == identity.sequence &&
          retry->particle_spawns.data() == particle_storage && retry->effect_lines.data() == line_storage &&
          retry->particle_spawns.size() == 2 && retry->particle_spawns[0].sequence == 11 &&
          state.decoded.particle_spawns.size() == 1 && state.decoded.particle_spawns[0].sequence == 13,
          "native retry replaced frozen ownership while using reusable prepared storage");

    hs::runtime_detail::VfxSessionState failed;
    failed.Synchronize(78);
    particle.tick = 1; particle.sequence = 31;
    failed.decoded.particle_spawns.push_back(particle);
    particle.tick = 2; particle.sequence = 32;
    failed.decoded.particle_spawns.push_back(particle);
    hs::runtime_detail::DecodedVfxState undersized;
    const auto next_identity = failed.next_native_batch_sequence;
    allocation_budget = 0;
    bool allocation_failed{};
    try { (void)failed.PrepareNativeBatch(1, true, &undersized); }
    catch (const std::bad_alloc &) { allocation_failed = true; }
    allocation_budget = -1;
    Check(allocation_failed && !failed.native_batch_active &&
          failed.next_native_batch_sequence == next_identity && failed.decoded.particle_spawns.size() == 2 &&
          failed.decoded.particle_spawns[0].sequence == 31 && failed.decoded.particle_spawns[1].sequence == 32,
          "failed native reserve changed identity or pending prefix");

    const auto reject_scratch = [&](hs::runtime_detail::DecodedVfxState &invalid) {
        const auto sequence = failed.next_native_batch_sequence;
        bool rejected{};
        try { (void)failed.PrepareNativeBatch(1, true, &invalid); }
        catch (const std::invalid_argument &) { rejected = true; }
        Check(rejected && !failed.native_batch_active && failed.next_native_batch_sequence == sequence &&
              failed.decoded.particle_spawns.size() == 2 && failed.decoded.particle_spawns[0].sequence == 31,
              "invalid native scratch changed pending ownership or batch identity");
    };
    reject_scratch(failed.decoded);
    hs::runtime_detail::DecodedVfxState nonempty;
    nonempty.effect_lines.push_back({});
    reject_scratch(nonempty);
}

void CheckRawSchedulerAdaptiveStorage()
{
    using Scheduler = hs::runtime_detail::VfxSessionState::RawEventScheduler;
    Scheduler future;
    Check(future.Enqueue({.sequence=1,.tick=1,.session_id=1}),"first adaptive event");
    const auto initial_slots=future.LogicalCapacityBytes()/sizeof(Scheduler::Entry);
    Check(initial_slots<Scheduler::kCapacity,"first raw event allocated the whole ingress cap");
    for(std::uint64_t sequence=2;sequence<=301;++sequence)
        Check(future.Enqueue({.sequence=sequence,.tick=10000,.session_id=1}),"future event enqueue");
    const auto &first=future.Prepare(1,1);
    Check(first.entries.size()==1&&first.entries.capacity()<future.PendingCount()&&
        first.entries.front().event.sequence==1&&future.PendingFront()->event.sequence==2,
        "ready staging reserved or consumed the future tail");
    const auto *frozen=first.entries.data();
    for(std::uint64_t sequence=302;sequence<=700;++sequence)
        Check(future.Enqueue({.sequence=sequence,.tick=10000,.session_id=1}),"pending growth during activation");
    Check(future.Prepare(10000,1).entries.data()==frozen&&first.entries.size()==1,
        "pending ring growth invalidated the in-flight activation");
    future.AcknowledgeActivation();

    Scheduler growing;
    for(std::uint64_t sequence=1;sequence<=initial_slots;++sequence)
        Check(growing.Enqueue({.sequence=sequence,.tick=1,.session_id=1}),"growth fixture enqueue");
    const auto held=growing.LogicalCapacityBytes();
    bool failed{};
    allocation_budget=0;
    try { (void)growing.Enqueue({.sequence=initial_slots+1,.tick=2,.session_id=1}); }
    catch(const std::bad_alloc &) { failed=true; }
    allocation_budget=-1;
    Check(failed&&growing.PendingCount()==initial_slots&&growing.PendingFront()->event.sequence==1&&
        growing.LogicalCapacityBytes()==held,"failed ring growth changed pending ownership");
    Check(growing.Enqueue({.sequence=initial_slots+1,.tick=2,.session_id=1})&&
        growing.Prepare(1,1).entries.size()==initial_slots&&growing.PendingCount()==1,
        "ring growth retry lost the ready prefix or future event");

    Scheduler wrapped;
    for(std::uint64_t sequence=1;sequence<=initial_slots;++sequence)
        Check(wrapped.Enqueue({.sequence=sequence,.tick=sequence<=initial_slots/2?1u:2u,.session_id=1}),
            "wrapped growth fixture enqueue");
    Check(wrapped.Prepare(1,1).entries.size()==initial_slots/2,"wrapped growth initial prefix");
    wrapped.AcknowledgeActivation();
    for(std::uint64_t sequence=initial_slots+1;sequence<=initial_slots+initial_slots/2+1;++sequence)
        Check(wrapped.Enqueue({.sequence=sequence,.tick=2,.session_id=1}),"wrapped backing growth");
    const auto &wrapped_batch=wrapped.Prepare(2,1);
    Check(wrapped_batch.entries.size()==initial_slots+1,"wrapped growth lost events");
    for(std::size_t index=0;index<wrapped_batch.entries.size();++index)
        Check(wrapped_batch.entries[index].event.sequence==initial_slots/2+index+1,
            "wrapped growth changed FIFO order");

    Scheduler source;
    Check(source.Enqueue({.sequence=0,.tick=0,.session_id=0}),"stale ready-prefix fixture enqueue");
    for(std::uint64_t sequence=1;sequence<=64;++sequence)
        Check(source.Enqueue({.sequence=sequence,.tick=1,.session_id=1}),"ready rollback fixture enqueue");
    bool mid_staging_failure{};
    bool completed{};
    for(int budget=0;budget<32&&!completed;++budget)
    {
        auto candidate=source;
        bool allocation_failed{};
        allocation_budget=budget;
        try { (void)candidate.Prepare(1,1); }
        catch(const std::bad_alloc &) { allocation_failed=true; }
        allocation_budget=-1;
        if(allocation_failed)
        {
            mid_staging_failure|=candidate.in_flight.entries.capacity()!=0;
            Check(candidate.PendingCount()==65&&candidate.PendingFront()->event.sequence==0&&
                candidate.in_flight.entries.empty(),"failed prefix staging partially consumed the raw FIFO");
        }
        else completed=true;
        const auto &retry=candidate.Prepare(1,1);
        Check(retry.entries.size()==64&&candidate.PendingCount()==0,"prefix staging retry lost events");
        for(std::size_t index=0;index<retry.entries.size();++index)
            Check(retry.entries[index].event.sequence==index+1,"prefix staging retry reordered events");
    }
    Check(completed&&mid_staging_failure,"ready staging allocation failures were not exercised");
}

void CheckRawSchedulerRing()
{
    using Scheduler = hs::runtime_detail::VfxSessionState::RawEventScheduler;
    Scheduler scheduler;
    Check(scheduler.PendingCount() == 0 && !scheduler.PendingFront() &&
        scheduler.LogicalCapacityBytes() == 0, "unused raw FIFO must not allocate its backing");
    std::uint64_t next_sequence = 1;
    std::uint64_t session = 1;
    const auto enqueue = [&](hs::Tick tick, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i)
        {
            hs::PresentationEvent event;
            event.session_id = session; event.tick = tick; event.sequence = next_sequence++;
            Check(scheduler.Enqueue(event), "raw ring rejected an available slot");
        }
    };
    constexpr auto half = Scheduler::kCapacity / 2;
    enqueue(1, half);
    enqueue(2, half);
    Check(!scheduler.HasCapacity(), "raw ring must count all pending slots");
    const auto &first = scheduler.Prepare(1, 1);
    Check(first.entries.size() == half && first.entries.front().event.sequence == 1 &&
        first.entries.back().event.sequence == half && scheduler.PendingCount() == half,
        "raw ring first ready prefix must preserve event order");
    Check(!scheduler.HasCapacity(), "in-flight ownership must retain its ingress slots");
    const auto reserved_bytes = scheduler.LogicalCapacityBytes();
    Check(scheduler.Prepare(99, 1).entries.size() == half && scheduler.PendingCount() == half,
        "unacknowledged batch must not drain newer raw events");
    scheduler.AcknowledgeActivation();
    for (hs::Tick tick = 2; tick <= 7; ++tick)
    {
        enqueue(tick + 1, half);
        const auto &batch = scheduler.Prepare(tick, 1);
        Check(batch.entries.size() == half && batch.entries.front().event.sequence ==
            (tick - 1) * half + 1 && batch.entries.back().event.sequence == tick * half,
            "raw ring wraparound changed FIFO order");
        Check(scheduler.LogicalCapacityBytes() == reserved_bytes &&
            scheduler.PendingCount() == half, "warm raw FIFO must reuse both bounded backings");
        scheduler.AcknowledgeActivation();
    }
    scheduler.DiscardBeforeSession(2);
    Check(!scheduler.PendingFront() && scheduler.PendingCount() == 0 &&
        scheduler.LogicalCapacityBytes() == reserved_bytes,
        "session discard must retain and charge reusable FIFO capacity");
    session = 2;
    enqueue(9, 1);
    Check(scheduler.Prepare(9, 2).entries.front().event.sequence == next_sequence - 1,
        "discarded wrapped FIFO must accept the next event without stale slots");
    Scheduler moved(std::move(scheduler));
    Check(scheduler.PendingCount() == 0 && !scheduler.PendingFront() &&
        scheduler.Prepare(9, 2).entries.empty() && moved.Prepare(9, 2).entries.size() == 1,
        "moving a wrapped FIFO must transfer both backings and reset source indices");
    Check(scheduler.Enqueue({.tick = 10, .session_id = 2}) &&
        scheduler.Prepare(10, 2).entries.size() == 1,
        "moved-from FIFO must remain reusable");
    moved = std::move(scheduler);
    Check(moved.Prepare(10, 2).entries.front().event.tick == 10 &&
        scheduler.Prepare(9, 2).entries.front().event.tick == 9,
        "FIFO move assignment must keep each swapped backing and its indices together");
}

int main()
{
    try
    {
        TestVfxProfileAdmission();
        CheckRetainedCapacitySaturation();
        CheckVfxAppendReservation();
        CheckVfxSharedAppendBudget();
        CheckVfxBirthRowSelection();
        CheckVfxBirthAdmissionBoundary();
        CheckVfxBirthRetirement();
        CheckBirthRetirementUpdatesSortPrefixAndFreeze();
        CheckVisibleFlashRetirement();
        CheckActiveEffectRetention();
        CheckGeometrySectorIdentity();
        CheckPreparedActiveEffectAppendBatch();
        CheckOptionalProfileAdmission();
        CheckPreparedGroundOwnerReplacement();
        CheckOwnerBindingStorageReuse();
        CheckTrackedVfxCapacityAccounting();
        CheckOptionalFlashBudget();
        CheckPersistentBurstCommit();
        CheckPersistentBurstOwnerSelection();
        CheckPersistentBurstRetirement();
        CheckPendingEmissionOrder();
        CheckPreparedBirthAppend();
        CheckEmptyPreparedShardCapacityTransfer();
        CheckPreparedActivationCommit();
        CheckPreparedSurfaceCaptureReservation();
        CheckActivationBeforeNativeEmission();
        CheckFrozenNativeBatches();
        CheckNativeBatchReusesPreparedStorage();
        CheckRawSchedulerAdaptiveStorage();
        CheckRawSchedulerRing();
        static_assert(noexcept(std::declval<hs::runtime_detail::VfxSessionState &>().SwapDecoded(
            std::declval<hs::runtime_detail::DecodedVfxState &>())));
        using hs::SessionPhase;
        using Clock = hs::FixedStepClock;
        hs::runtime_detail::VfxSessionState state;
        Check(state.Synchronize(10), "initial session");
        const auto start = Clock::Clock::time_point{};
        Check(state.UpdateTerminalClock(120, SessionPhase::Victory, start) == 120,
              "terminal episode starts on snapshot tick");
        state.low_health_pulse.stable_id = 77;
        Check(state.MarkPersistentBurst(88, 3), "first burst admitted");
        hs::PresentationEvent pending;
        pending.session_id = 10; pending.tick = 140; pending.sequence = 9;
        Check(state.raw_events.Enqueue(pending), "event enqueue");
        Resize(state.decoded, 1);
        const auto lease = std::make_shared<int>(1);
        state.decoded.entity_surface_captures[0].generation_lease = lease;

        // A rejected reload destroys only its prepared copy.
        {
            auto failed = state.decoded;
            Resize(failed, 2);
            failed.entity_surface_captures[0].generation_lease.reset();
        }
        CheckSize(state.decoded, 1);
        Check(state.decoded.entity_surface_captures[0].generation_lease == lease,
              "failed preparation must preserve captured generation lease");

        auto candidate = state.decoded;
        Resize(candidate, 2);
        state.SwapDecoded(candidate);
        CheckSize(state.decoded, 2);
        CheckSize(candidate, 1);
        Check(state.decoded.entity_surface_captures[0].generation_lease == lease,
              "successful publication preserves the prepared capture lease");
        Check(state.session_id == 10 && state.raw_events.PendingCount() == 1 &&
                  state.low_health_pulse.stable_id == 77 && !state.MarkPersistentBurst(88, 3),
              "generation publication must preserve semantic session state");
        Check(state.UpdateTerminalClock(120, SessionPhase::Victory,
                  start + Clock::kFixedStep * 5) == 125,
              "generation publication must not restart terminal time");
        state.ClearDecoded();
        CheckSize(state.decoded, 0);
        Check(state.terminal_clock.has_value() && state.raw_events.PendingCount() == 1 &&
                  !state.MarkPersistentBurst(88, 3),
              "decoded clear must preserve pending events, clock and burst tombstones");
        Check(state.raw_events.Prepare(139, 10).entries.empty(), "future event dispatched early");
        const auto &batch = state.raw_events.Prepare(140, 10);
        Check(batch.entries.size() == 1 && batch.entries[0].event.sequence == 9,
              "pending event becomes ready at its authoritative tick");
        Check(state.raw_events.Prepare(140, 10).entries.size() == 1,
              "unacknowledged ready batch remains owned for retry");
        state.raw_events.AcknowledgeActivation();
        Check(state.raw_events.Prepare(140, 10).entries.empty(),
              "acknowledged event consumes exactly once");
        Resize(state.decoded, 1);
        Check(state.Synchronize(11), "new session");
        CheckSize(state.decoded, 0);
        Check(!state.terminal_clock && state.low_health_pulse.stable_id == 0 &&
                  state.MarkPersistentBurst(88, 3), "session restart clears episode state");
        Check(state.UpdateTerminalClock(240, SessionPhase::Defeat, start) == 240 &&
                  state.UpdateTerminalClock(240, SessionPhase::Defeat,
                      start - Clock::kFixedStep) == 240,
              "backwards wall time cannot advance terminal time");
        Check(state.UpdateTerminalClock(240, SessionPhase::Victory,
                  start + Clock::kFixedStep * 8) == 240,
              "terminal phase change starts a new clock");
        Check(state.UpdateTerminalClock(241, SessionPhase::Victory,
                  start + Clock::kFixedStep * 9) == 241,
              "new authoritative terminal tick restarts presentation clock");
        Check(state.UpdateTerminalClock(241, SessionPhase::Paused, start) == 241 &&
                  !state.terminal_clock, "nonterminal phases stay on authoritative time");
        const auto maximum = std::numeric_limits<hs::Tick>::max();
        state.UpdateTerminalClock(maximum - 1, SessionPhase::Victory, start);
        Check(state.UpdateTerminalClock(maximum - 1, SessionPhase::Victory,
                  start + Clock::kFixedStep * 4) == maximum,
              "terminal clock saturates without overflow");
        hs::runtime_detail::VfxSessionState::RawEventScheduler scheduler;
        for (std::uint64_t i = 1; i <= scheduler.kCapacity; ++i)
        {
            hs::PresentationEvent ordered;
            ordered.session_id = 1;
            ordered.tick = i <= scheduler.kCapacity / 2 ? 4 : 5;
            ordered.sequence = (i + 1) / 2;
            ordered.asset = hs::AssetId{i};
            Check(scheduler.Enqueue(ordered), "bounded scheduler rejected in-capacity event");
        }
        hs::PresentationEvent overflow;
        overflow.session_id = 1;
        Check(!scheduler.Enqueue(overflow), "bounded scheduler exceeded capacity");
        const auto &ordered = scheduler.Prepare(5, 1);
        Check(ordered.entries.size() == scheduler.kCapacity, "raw FIFO missed ready entries");
        for (std::size_t i = 0; i < ordered.entries.size(); ++i)
            Check(ordered.entries[i].event.asset.value == i + 1,
                  "raw FIFO reordered same-tick projector fanout");
        scheduler.AcknowledgeActivation();
        hs::PresentationEvent future;
        future.session_id = 3; future.tick = 0; future.sequence = 3;
        hs::PresentationEvent current = future;
        current.session_id = 2; current.tick = 120; current.sequence = 2;
        Check(scheduler.Enqueue(current) && scheduler.Enqueue(future), "cross-session enqueue");
        const auto &current_batch = scheduler.Prepare(120, 2);
        Check(current_batch.entries.size() == 1 && current_batch.entries[0].event.sequence == 2 &&
              scheduler.PendingCount() == 1 && scheduler.PendingFront()->event.session_id == 3,
              "future session must neither block nor activate in the current session");
        scheduler.AcknowledgeActivation();
        Check(scheduler.Prepare(200, 2).entries.empty() && scheduler.PendingCount() == 1,
              "future session must retain ownership while the current session advances");
        scheduler.DiscardBeforeSession(3);
        Check(scheduler.Prepare(0, 3).entries.size() == 1,
              "retained future event must activate when its session begins");
        scheduler.AcknowledgeActivation();
        std::puts("VFX session state tests passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
