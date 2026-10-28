#include "vfx_session_state.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace
{
void Check(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
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
    Check(state.seen_persistent_bursts.size() == 1, "selection must not publish episode tombstones");
    std::vector<State::PersistentBurstKey> keys;
    for (const auto index : selected)
        keys.push_back({owners[index].stable_id, owners[index].source_visual_kind});
    std::vector<hs::ParticleSpawnCommand> commands;
    std::vector<hs::ParticleSpawnCommand> destination;
    Check(state.CommitPersistentBursts(keys, commands, destination), "selected bursts commit");
    state.SelectUnseenPersistentBurstOwners(owners, selected);
    Check(selected.empty(), "committed burst owners were selected again");
    state.Synchronize(8);
    state.SelectUnseenPersistentBurstOwners(owners, selected);
    Check(selected == original_selection(owners) && !selected.empty(), "new session retained old burst selection");
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
    Check(!state.CommitPersistentBursts(keys, staged, destination) && destination.size() == 2,
          "retrying committed burst inputs does not append commands twice");
    const std::array invalid_keys{Key{44, 5}, Key{44, 5}};
    Check(!state.CommitPersistentBursts(invalid_keys, staged, destination) &&
          !state.HasPersistentBurst(44, 5) && destination.size() == 2,
          "duplicate keys reject the batch before publishing a tombstone or commands");
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
    const std::array first_episode{Key{501, kind}};
    const std::vector<hs::ParticleSpawnCommand> no_commands;
    std::vector<hs::ParticleSpawnCommand> destination;
    Check(state.CommitPersistentBursts(first_episode, no_commands, destination),
          "first status episode commits");
    retire(state, first_episode, {}, true, 100);
    Check(state.HasPersistentBurst(501, kind), "current owner retains its burst tombstone");
    retire(state, {}, first_episode, true, 101);
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
        CheckActiveEffectRetention();
        CheckGeometrySectorIdentity();
        CheckPreparedActiveEffectAppendBatch();
        CheckPreparedGroundOwnerReplacement();
        CheckOwnerBindingStorageReuse();
        CheckTrackedVfxCapacityAccounting();
        CheckOptionalFlashBudget();
        CheckPersistentBurstCommit();
        CheckPersistentBurstOwnerSelection();
        CheckPersistentBurstRetirement();
        CheckPendingEmissionOrder();
        CheckPreparedBirthAppend();
        CheckActivationBeforeNativeEmission();
        CheckFrozenNativeBatches();
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
