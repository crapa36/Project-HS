#include "vfx_session_state.hpp"
#include "vfx_birth_admission.hpp"
#include "vfx_birth_retirement.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

void TestVfxProfileAdmission();

namespace
{
void Check(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

void CheckVfxBirthRowSelection()
{
    using hs::runtime_detail::vfx_retained_size::BirthRequest;
    using hs::runtime_detail::vfx_retained_size::SelectBirthRows;
    const std::array<std::size_t, 3> row_bytes{4, 12, 8};
    std::size_t particle{}, line{}, flash{};
    std::array<BirthRequest, 3> all_fit_requests{{
        {2, &particle, 1, 0}, {2, &line, 3, 1}, {2, &flash, 2, 2}}};
    std::vector<std::size_t> order{77};
    Check(SelectBirthRows(all_fit_requests, row_bytes, 100, 148, order) == 48 &&
          particle == 2 && line == 2 && flash == 2 && order == std::vector<std::size_t>{77},
        "all-fit birth selection miscounted logical rows or touched sort scratch");

    const std::array<std::size_t, 2> partial_row_bytes{4, 8};
    std::array<std::size_t, 3> partial_admitted{};
    std::array<BirthRequest, 3> partial_requests{{
        {3, &partial_admitted[0], 5, 0},
        {2, &partial_admitted[1], 5, 1},
        {2, &partial_admitted[2], 9, 0}}};
    order.clear(); order.reserve(partial_requests.size());
    Check(SelectBirthRows(partial_requests, partial_row_bytes, 10, 30, order) == 20 &&
          partial_admitted == std::array<std::size_t, 3>{3, 0, 2} &&
          order == std::vector<std::size_t>{2, 0, 1},
        "partial birth selection lost importance ordering or original-index ties");
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

void CheckSurfaceFrameCaptureAdmission()
{
    using Store = hs::runtime_detail::ActiveEffectStore;
    using Profile = Store::Profile;
    const auto bit = [](Profile profile) {
        return static_cast<std::uint16_t>(1u << static_cast<unsigned>(profile));
    };
    hs::VfxEventInput input{};
    input.sequence = 689; input.effect_handle = 1;
    Store::FrameRows rows{};
    rows[static_cast<std::size_t>(Profile::EntitySurface)] = 2;
    rows[static_cast<std::size_t>(Profile::Light)] = 1;
    const std::array requests{
        Store::AppendRequest{&input, bit(Profile::EntitySurface), false, 0, 0, &rows},
        Store::AppendRequest{&input, bit(Profile::EntitySurface), false, 0, 0, &rows},
        Store::AppendRequest{&input, bit(Profile::Light), false, bit(Profile::Light), 1, &rows}
    };
    constexpr std::size_t mib = 1024 * 1024;
    for (const bool warm : {false, true})
    {
        Store store;
        Store::PreparedAppendBatch prepared;
        std::vector<std::size_t> order;
        order.reserve(warm ? 128 : requests.size());
        const auto held = store.LogicalBytes();
        Store::ProfileAdmission admission{held, 0, order};
        admission.frame_storage[static_cast<std::size_t>(Profile::Light)] = {mib / 4, 0};
        admission.surface_capture_storage = {mib, 2};
        admission.surface_capture_input_storage = {mib, 2};
        const auto required_bytes = sizeof(hs::VfxEventInput) + sizeof(std::uint16_t) +
            sizeof(std::uint32_t) + sizeof(Store::Row) + 2 * mib;
        admission.budget_bytes = held + admission.FrameBaseBytes() + required_bytes +
            sizeof(Store::Row) + mib / 4 - 1;
        Check(store.PrepareAppendBatch(requests, prepared, &admission) &&
              admission.reserved_surface_captures == 3 &&
              prepared.AdditionalProfileRows(Profile::EntitySurface) == 1 &&
              admission.admitted_bytes == required_bytes &&
              admission.admitted_cosmetic_memberships == 0 &&
              store.CommitAppendBatch(prepared) &&
              store.ProfileEvents(Profile::EntitySurface).size() == 1 &&
              store.ProfileEvents(Profile::Light).empty(),
              "required surface captures precede optional Light regardless of scratch capacity");
    }
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

void CheckOwnerRefreshExpiry()
{
    using Store = hs::runtime_detail::ActiveEffectStore;
    struct Counts { std::size_t refresh{}, expiry{}; } counts;
    Store store;
    const auto add = [&](hs::Sequence sequence, std::uint64_t owner, hs::Tick end,
                         std::initializer_list<Store::Profile> profiles) {
        hs::VfxEventInput input{};
        input.sequence = sequence;
        input.geometry_owner_id = owner;
        input.geometry_end_tick = end;
        for (const auto profile : profiles) store.Append(profile, input);
    };
    add(201, 101, 0, {Store::Profile::Ground, Store::Profile::Light});
    add(202, 102, 20, {Store::Profile::Ground, Store::Profile::Light}); // owner refresh drops
    add(203, 103, 20, {Store::Profile::Ground, Store::Profile::Light, Store::Profile::Ribbon}); // bound expiry
    add(204, 0, 1, {Store::Profile::Ground, Store::Profile::Light}); // canonical expiry

    const auto run_frame = [&](unsigned frame, hs::Tick current_tick)
    {
        const auto refresh = [&](hs::VfxEventInput &bound) {
            ++counts.refresh;
            if (bound.geometry_owner_id == 102) return false;
            bound.geometry_end_tick = bound.geometry_owner_id == 103 ? 1u : (frame == 1 ? 10u : 3u);
            return true;
        };
        const auto expired = [&](const hs::VfxEventInput &input) {
            ++counts.expiry;
            return input.geometry_end_tick != 0 && input.geometry_end_tick <= current_tick;
        };
        store.BeginFrameBindings();
        for (const auto profile : {Store::Profile::Ground, Store::Profile::Light})
            store.RefreshProfile(profile, refresh, expired);
    };

    run_frame(1, 3);
    Check(counts.refresh == 3 && counts.expiry == 6,
        "shared owner refresh or expiry callback ran an unexpected number of times");
    const auto ground = store.ProfileEvents(Store::Profile::Ground);
    const auto light = store.ProfileEvents(Store::Profile::Light);
    const auto ribbon = store.ProfileEvents(Store::Profile::Ribbon);
    Check(ground.size() == 1 && ground[0].sequence == 201 && ground[0].geometry_end_tick == 10,
        "first owner refresh must retain the expected Ground row and bound end tick");
    Check(light.size() == 1 && light[0].sequence == 201 && light[0].geometry_end_tick == 10,
        "first owner refresh must retain the expected Light row and bound end tick");
    Check(ribbon.size() == 1 && ribbon[0].sequence == 203,
        "first owner refresh must retain the Ribbon-only row");

    store.Compact();
    Check(store.Size() == 2, "compaction must preserve the shared live row and Ribbon-only row");
    Check(store.ProfileEvents(Store::Profile::Ground).size() == 1 &&
          store.ProfileEvents(Store::Profile::Ground)[0].sequence == 201 &&
          store.ProfileEvents(Store::Profile::Light).size() == 1 &&
          store.ProfileEvents(Store::Profile::Light)[0].sequence == 201 &&
          store.ProfileEvents(Store::Profile::Ribbon).size() == 1 &&
          store.ProfileEvents(Store::Profile::Ribbon)[0].sequence == 203,
        "compaction must preserve expected profile rows");

    run_frame(2, 3);
    Check(counts.refresh == 4 && counts.expiry == 8,
        "frame reset duplicated or skipped a shared owner refresh/expiry");
    store.Compact();
    Check(store.Size() == 1 && store.ProfileEvents(Store::Profile::Ground).empty() &&
          store.ProfileEvents(Store::Profile::Light).empty() &&
          store.ProfileEvents(Store::Profile::Ribbon).size() == 1 &&
          store.ProfileEvents(Store::Profile::Ribbon)[0].sequence == 203,
        "compaction must retain only the expected Ribbon-owned row");
}

void CheckPersistentBurstCommit()
{
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
          state.HasPersistentBurst(49, 1),
          "separate batches accept out-of-order keys while keeping the retained set searchable");
    const std::array warm_key{Key{48, 3}};
    Check(state.CommitPersistentBursts(warm_key, no_commands, destination) &&
          state.HasPersistentBurst(48, 3), "new burst remains searchable");
    Check(!state.CommitPersistentBursts(keys, staged, destination) && destination.size() == 2,
          "retrying committed burst inputs does not append commands twice");
    const std::array invalid_keys{Key{44, 5}, Key{44, 5}};
    Check(!state.CommitPersistentBursts(invalid_keys, staged, destination) &&
          !state.HasPersistentBurst(44, 5) && destination.size() == 2,
          "duplicate keys reject the batch before publishing a tombstone or commands");
    const std::array zero_key{Key{0, 5}};
    Check(!state.CommitPersistentBursts(zero_key, no_commands, destination) &&
          !state.HasPersistentBurst(0, 5),
          "zero stable ids reject the batch without changing retained tombstones");

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
    state.decoded.flashes.push_back({{.stable_seed = 30}});

    prepared.particle_spawns.push_back({.sequence = 40, .tick = 3});
    prepared.particle_spawns.push_back({.sequence = 50, .tick = 4});
    prepared.effect_lines.push_back({.sequence = 60, .tick = 5});
    prepared.flashes.push_back({{.stable_seed = 70}});
    Check(state.AppendPreparedBirths(prepared), "prepared births must append when scratch is independent");
    Check(state.decoded.particle_spawns.size() == 3 &&
          state.decoded.particle_spawns[0].sequence == 10 &&
          state.decoded.particle_spawns[1].sequence == 40 &&
          state.decoded.particle_spawns[2].sequence == 50 &&
          state.decoded.effect_lines.size() == 2 && state.decoded.effect_lines[1].sequence == 60 &&
          state.decoded.flashes.size() == 2 && state.decoded.flashes[1].stable_seed == 70,
          "prepared append must preserve retained prefixes and staged order");
    Check(prepared.particle_spawns.empty() && prepared.effect_lines.empty() && prepared.flashes.empty(),
          "successful commit clears staged births");
    Check(!state.AppendPreparedBirths(state.decoded) && state.decoded.particle_spawns.size() == 3,
          "retained output cannot be used as its own prepared source");

    hs::runtime_detail::VfxSessionState vacant;
    hs::runtime_detail::DecodedVfxState owned_stage;
    owned_stage.particle_spawns.push_back({.sequence = 120});
    owned_stage.effect_lines.push_back({.sequence = 130});
    owned_stage.flashes.push_back({{.stable_seed = 140}});
    owned_stage.entity_surface_captures.push_back({.row = 9});
    Check(vacant.AppendPreparedBirths(owned_stage) &&
          vacant.decoded.particle_spawns.size() == 1 && vacant.decoded.effect_lines.size() == 1 &&
          vacant.decoded.flashes.size() == 1 &&
          vacant.decoded.particle_spawns[0].sequence == 120 &&
          vacant.decoded.effect_lines[0].sequence == 130 &&
          vacant.decoded.flashes[0].stable_seed == 140 &&
          owned_stage.entity_surface_captures.size() == 1 &&
          owned_stage.entity_surface_captures.front().row == 9,
          "empty queues accept staged births without consuming unrelated captures");

    hs::runtime_detail::VfxSessionState empty;
    hs::runtime_detail::DecodedVfxState no_births;
    Check(empty.AppendPreparedBirths(no_births) && empty.decoded.particle_spawns.empty() &&
          empty.decoded.effect_lines.empty() && empty.decoded.flashes.empty(),
          "empty prepared stage is a successful no-op");

    hs::runtime_detail::VfxSessionState coordinated;
    using Store = hs::runtime_detail::ActiveEffectStore;
    hs::VfxEventInput event;
    event.sequence = 150;
    const std::array requests{Store::AppendRequest{&event,
        static_cast<std::uint16_t>((1u << static_cast<unsigned>(Store::Profile::EntitySurface)) |
                                  (1u << static_cast<unsigned>(Store::Profile::Light)))}};
    Store::PreparedAppendBatch profiles;
    hs::runtime_detail::DecodedVfxState births;
    births.flashes.push_back({{.stable_seed = 160}});
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
    state.decoded.particle_spawns.push_back({.sequence = 80, .tick = 2});

    hs::PresentationEvent event;
    event.session_id = 7;
    event.tick = 1;
    event.sequence = 1;
    Check(state.raw_events.Enqueue(event) && state.raw_events.Prepare(1, 7).events.size() == 1,
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

    Check(state.CommitPreparedActivation(births, profiles, true) &&
          state.decoded.particle_spawns.size() == 2 &&
          state.decoded.particle_spawns[0].sequence == 80 &&
          state.decoded.particle_spawns[1].sequence == 100 &&
          state.active_effects.Size() == 1 && state.raw_events.Prepare(1, 7).events.empty(),
        "successful activation publishes births and profiles before acknowledging raw input");
    Check(state.native_batch_active && state.native_batch.particle_spawns.size() == 1 &&
          state.native_batch.particle_spawns[0].sequence == 70 &&
          !state.AcknowledgeNativeBatch(native_identity, false) && state.native_batch_active,
        "CPU activation acknowledgement must leave unsubmitted native work pending");

    State stale;
    stale.Synchronize(8);
    event.session_id = 8;
    Check(stale.raw_events.Enqueue(event) && stale.raw_events.Prepare(1, 8).events.size() == 1,
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
    stale_births.flashes.push_back({{.stable_seed = 93}});
    Check(!stale.CommitPreparedActivation(stale_births, stale_profiles, true) &&
          stale.active_effects.Size() == 1 && stale.decoded.flashes.empty() &&
          stale_births.flashes.size() == 1 && stale.raw_events.Prepare(1, 8).events.size() == 1,
        "stale profiles must reject activation before births or raw acknowledgement");

    State cancelled;
    cancelled.Synchronize(9);
    event.session_id = 9;
    Check(cancelled.raw_events.Enqueue(event) && cancelled.raw_events.Prepare(1, 9).events.size() == 1,
        "cancel fixture raw activation must be ready");
    Store::PreparedAppendBatch cancelled_profiles;
    hs::runtime_detail::DecodedVfxState cancelled_births;
    cancelled_births.flashes.push_back({{.stable_seed = 94}});
    cancelled.active_effects.CancelAppendBatch(cancelled_profiles);
    Check(!cancelled.CommitPreparedActivation(cancelled_births, cancelled_profiles, true) &&
          cancelled.decoded.flashes.empty() && cancelled_births.flashes.size() == 1 &&
          cancelled.raw_events.Prepare(1, 9).events.size() == 1,
        "cancelled profiles must reject activation before births or raw acknowledgement");

    State alias;
    alias.Synchronize(10);
    event.session_id = 10;
    Check(alias.raw_events.Enqueue(event) && alias.raw_events.Prepare(1, 10).events.size() == 1,
        "alias fixture raw activation must be ready");
    Check(!alias.CommitPreparedActivation(alias.decoded, cancelled_profiles, false) &&
          alias.raw_events.Prepare(1, 10).events.size() == 1,
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
    Check(activation.events.size() == 1, "raw event did not reach CPU activation");
    hs::ParticleSpawnCommand delayed;
    delayed.sequence = activation.events[0].sequence;
    delayed.tick = 120; delayed.authored_birth_tick = 120;
    state.decoded.particle_spawns.push_back(delayed);
    state.raw_events.AcknowledgeActivation();
    Check(state.raw_events.Prepare(100, 7).events.empty() &&
        state.decoded.particle_spawns.size() == 1 && state.decoded.particle_spawns[0].tick == 120,
        "native delay replayed raw activation or lost its owned emission");
    auto second = first;
    second.tick = 101; second.sequence = 2;
    Check(state.raw_events.Enqueue(second) && state.raw_events.Prepare(101, 7).events.size() == 1,
        "pending native emission blocked the next CPU activation");
    auto immediate = delayed;
    immediate.tick = 101; immediate.authored_birth_tick = 101; immediate.sequence = 2;
    state.decoded.particle_spawns.push_back(immediate);
    state.decoded.SortNewParticleSpawns(1);
    state.raw_events.AcknowledgeActivation();
    Check(state.decoded.particle_spawns[0].sequence == 2 &&
        state.decoded.particle_spawns[1].sequence == 1 && state.raw_events.Prepare(101, 7).events.empty(),
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

    for (const auto [due_count, future_count] :
         std::array<std::pair<std::size_t, std::size_t>, 4>{{{0, 2}, {2, 0}, {1, 3}, {3, 1}}})
    {
        hs::runtime_detail::VfxSessionState with_snapshot;
        with_snapshot.Synchronize(10);
        for (std::size_t index = 0; index < due_count + future_count; ++index)
        {
            hs::ParticleSpawnCommand row;
            row.tick = index < due_count ? 10 : 20;
            row.sequence = index + 1;
            with_snapshot.decoded.particle_spawns.push_back(row);
        }
        hs::ParticleSpawnCommand snapshot_birth;
        snapshot_birth.sequence = 777;
        const std::array suffix{snapshot_birth};
        auto *first = with_snapshot.PrepareNativeBatch(10, true, nullptr, suffix);
        Check(first && first->decoded_particle_count == due_count &&
              first->particle_spawns.size() == due_count + 1 &&
              first->particle_spawns.back().sequence == 777 &&
              with_snapshot.decoded.particle_spawns.size() == future_count,
              "snapshot births follow the frozen decoded prefix in every freeze partition");
        for (std::size_t index = 0; index < due_count; ++index)
            Check(first->particle_spawns[index].sequence == index + 1, "decoded birth order is retained");
        const auto receipt = first->identity;
        const auto *retry_batch = with_snapshot.PrepareNativeBatch(11, true, nullptr, suffix);
        Check(retry_batch == first && retry_batch->identity.sequence == receipt.sequence &&
              retry_batch->particle_spawns.size() == due_count + 1 &&
              retry_batch->particle_spawns.back().sequence == 777,
              "snapshot suffix is frozen once across retry");
        Check(with_snapshot.AcknowledgeNativeBatch(receipt, true) &&
              with_snapshot.last_committed_stateful_target == 10 && !with_snapshot.native_batch_active,
              "snapshot batch acknowledges its original target");
    }
}

void CheckRawIngressDrain()
{
    using Scheduler = hs::runtime_detail::VfxSessionState::RawEventScheduler;
    using Queue = hs::BoundedSpscQueue<hs::PresentationEvent, 1024>;
    auto ingress = std::make_unique<Queue>();
    Scheduler scheduler;
    for (std::uint64_t sequence = 1; sequence <= 600; ++sequence)
        Check(ingress->TryPush({.sequence = sequence, .tick = 1,
            .kind = hs::PresentationKind::Vfx, .session_id = 1}), "burst ingress enqueue");
    std::uint64_t observed{};
    const auto observe = [&](const hs::PresentationEvent &event) {
        Check(event.sequence == ++observed, "ingress drain changed source order");
    };
    scheduler.DrainIngress(*ingress, observe);
    Check(observed == 600 && ingress->Size() == 0 && scheduler.PendingCount() == 600,
        "a published burst was throttled to an arbitrary frame quota");
    const auto &ready = scheduler.Prepare(1, 1);
    Check(ready.events.size() == 600, "ingress burst lost activation rows");
    for (std::size_t row = 0; row < ready.events.size(); ++row)
        Check(ready.events[row].sequence == row + 1, "ingress burst reordered activation");
    scheduler.AcknowledgeActivation();

    for (std::uint64_t sequence = 1; sequence <= Scheduler::kCapacity; ++sequence)
        Check(scheduler.Enqueue({.sequence = sequence, .tick = 1, .session_id = 1}), "fill raw ingress");
    Check(scheduler.Prepare(1, 1).events.size() == Scheduler::kCapacity && !scheduler.HasCapacity(),
        "in-flight raw rows must continue to occupy ingress capacity");
    for (const auto kind : {hs::PresentationKind::Ui, hs::PresentationKind::Audio,
             hs::PresentationKind::Vfx, hs::PresentationKind::Ui})
        Check(ingress->TryPush({.sequence = 601 + ingress->Size(), .tick = 1,
            .kind = kind, .session_id = 1}), "mixed ingress enqueue");
    scheduler.DrainIngress(*ingress, observe);
    Check(observed == 602 && ingress->Size() == 2 && !scheduler.HasCapacity(),
        "full raw ingress must drain a non-VFX prefix and retain the first blocked VFX");
    scheduler.DrainIngress(*ingress, observe);
    Check(observed == 602 && ingress->Size() == 2,
        "a blocked VFX must not be dropped, duplicated or bypassed");
    scheduler.AcknowledgeActivation();
    scheduler.DrainIngress(*ingress, observe);
    Check(observed == 604 && ingress->Size() == 0 && scheduler.PendingCount() == 1 &&
        scheduler.PendingFront()->event.sequence == 603,
        "acknowledged capacity must resume the original FIFO prefix");

    Scheduler growing;
    Check(ingress->TryPush({.sequence = 605, .tick = 2,
        .kind = hs::PresentationKind::Vfx, .session_id = 1}), "retry ingress enqueue");
    growing.DrainIngress(*ingress, observe);
    Check(observed == 605 && ingress->Size() == 0 && growing.PendingCount() == 1,
        "raw drain lost or duplicated an event");

    Check(ingress->TryPush({.sequence = 606, .kind = hs::PresentationKind::Ui}),
        "bounded drain fixture enqueue");
    growing.DrainIngress(*ingress, [&](const hs::PresentationEvent &event) {
        observe(event);
        Check(ingress->TryPush({.sequence = 607, .kind = hs::PresentationKind::Ui}),
            "concurrent publication fixture enqueue");
    });
    Check(observed == 606 && ingress->Size() == 1,
        "a drain must finish its initial prefix despite newer publications");
    growing.DrainIngress(*ingress, observe);
    Check(observed == 607 && ingress->Size() == 0, "later publication did not survive the bounded drain");
}

void CheckRawSchedulerFutureHold()
{
    using Scheduler = hs::runtime_detail::VfxSessionState::RawEventScheduler;
    Scheduler scheduler;
    Check(scheduler.Enqueue({.sequence = 1, .tick = 1, .session_id = 1}) &&
          scheduler.Enqueue({.sequence = 2, .tick = 2, .session_id = 1}),
          "ready/future raw events enqueue");
    const auto &first = scheduler.Prepare(1, 1);
    Check(first.events.size() == 1 && first.events.front().sequence == 1 &&
          scheduler.PendingCount() == 1 && scheduler.PendingFront()->event.sequence == 2,
          "future event must remain behind the ready FIFO prefix");
    Check(scheduler.Prepare(2, 1).events.front().sequence == 1 && scheduler.PendingCount() == 1,
          "unacknowledged activation must remain frozen while future input waits");
    scheduler.AcknowledgeActivation();
    const auto &next = scheduler.Prepare(2, 1);
    Check(next.events.size() == 1 && next.events.front().sequence == 2,
          "acknowledgment must release the future FIFO event");
}

void CheckRawSchedulerRing()
{
    using Scheduler = hs::runtime_detail::VfxSessionState::RawEventScheduler;
    Scheduler scheduler;
    Check(scheduler.PendingCount() == 0 && !scheduler.PendingFront(), "new raw FIFO must be empty");
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
    Check(first.events.size() == half && first.events.front().sequence == 1 &&
        first.events.back().sequence == half && scheduler.PendingCount() == half,
        "raw ring first ready prefix must preserve event order");
    Check(!scheduler.HasCapacity(), "in-flight ownership must retain its ingress slots");
    Check(scheduler.Prepare(99, 1).events.size() == half && scheduler.PendingCount() == half,
        "unacknowledged batch must not drain newer raw events");
    scheduler.AcknowledgeActivation();
    for (hs::Tick tick = 2; tick <= 7; ++tick)
    {
        enqueue(tick + 1, half);
        const auto &batch = scheduler.Prepare(tick, 1);
        Check(batch.events.size() == half && batch.events.front().sequence ==
            (tick - 1) * half + 1 && batch.events.back().sequence == tick * half,
            "raw ring wraparound changed FIFO order");
        Check(scheduler.PendingCount() == half, "raw FIFO must retain the pending suffix");
        scheduler.AcknowledgeActivation();
    }
    scheduler.DiscardBeforeSession(2);
    Check(!scheduler.PendingFront() && scheduler.PendingCount() == 0,
        "session discard must clear the pending FIFO");
    session = 2;
    enqueue(9, 1);
    Check(scheduler.Prepare(9, 2).events.front().sequence == next_sequence - 1,
        "discarded wrapped FIFO must accept the next event without stale slots");
    Scheduler moved(std::move(scheduler));
    Check(scheduler.PendingCount() == 0 && !scheduler.PendingFront() &&
        scheduler.Prepare(9, 2).events.empty() && moved.Prepare(9, 2).events.size() == 1,
        "moving a wrapped FIFO must transfer both backings and reset source indices");
    Check(scheduler.Enqueue({.tick = 10, .session_id = 2}) &&
        scheduler.Prepare(10, 2).events.size() == 1,
        "moved-from FIFO must remain reusable");
    moved = std::move(scheduler);
    Check(moved.Prepare(10, 2).events.front().tick == 10 &&
        scheduler.Prepare(9, 2).events.front().tick == 9,
        "FIFO move assignment must keep each swapped backing and its indices together");
}

int main()
{
    try
    {
        TestVfxProfileAdmission();
        CheckVfxBirthRowSelection();
        CheckVfxBirthRetirement();
        CheckGeometrySectorIdentity();
        CheckPreparedActiveEffectAppendBatch();
        CheckSurfaceFrameCaptureAdmission();
        CheckPreparedGroundOwnerReplacement();
        CheckOwnerRefreshExpiry();
        CheckPersistentBurstCommit();
        CheckPersistentBurstRetirement();
        CheckPendingEmissionOrder();
        CheckPreparedBirthAppend();
        CheckPreparedActivationCommit();
        CheckActivationBeforeNativeEmission();
        CheckFrozenNativeBatches();
        CheckRawIngressDrain();
        CheckRawSchedulerFutureHold();
        CheckRawSchedulerRing();
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
        Check(state.UpdateTerminalClock(120, SessionPhase::Victory,
                  start + Clock::kFixedStep * 5) == 125,
              "terminal presentation clock advances from the held snapshot tick");
        state.ClearDecoded();
        CheckSize(state.decoded, 0);
        Check(state.terminal_clock.has_value() && state.raw_events.PendingCount() == 1 &&
                  !state.MarkPersistentBurst(88, 3),
              "decoded clear must preserve pending events, clock and burst tombstones");
        Check(state.raw_events.Prepare(139, 10).events.empty(), "future event dispatched early");
        const auto &batch = state.raw_events.Prepare(140, 10);
        Check(batch.events.size() == 1 && batch.events[0].sequence == 9,
              "pending event becomes ready at its authoritative tick");
        Check(state.raw_events.Prepare(140, 10).events.size() == 1,
              "unacknowledged ready batch remains owned for retry");
        state.raw_events.AcknowledgeActivation();
        Check(state.raw_events.Prepare(140, 10).events.empty(),
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
        Check(ordered.events.size() == scheduler.kCapacity, "raw FIFO missed ready entries");
        for (std::size_t i = 0; i < ordered.events.size(); ++i)
            Check(ordered.events[i].asset.value == i + 1,
                  "raw FIFO reordered same-tick projector fanout");
        scheduler.AcknowledgeActivation();
        hs::PresentationEvent future;
        future.session_id = 3; future.tick = 0; future.sequence = 3;
        hs::PresentationEvent current = future;
        current.session_id = 2; current.tick = 120; current.sequence = 2;
        Check(scheduler.Enqueue(current) && scheduler.Enqueue(future), "cross-session enqueue");
        const auto &current_batch = scheduler.Prepare(120, 2);
        Check(current_batch.events.size() == 1 && current_batch.events[0].sequence == 2 &&
              scheduler.PendingCount() == 1 && scheduler.PendingFront()->event.session_id == 3,
              "future session must neither block nor activate in the current session");
        scheduler.AcknowledgeActivation();
        Check(scheduler.Prepare(200, 2).events.empty() && scheduler.PendingCount() == 1,
              "future session must retain ownership while the current session advances");
        scheduler.DiscardBeforeSession(3);
        Check(scheduler.Prepare(0, 3).events.size() == 1,
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
