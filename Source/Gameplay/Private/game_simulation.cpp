#include <hs/gameplay/game_simulation.hpp>

#include <flecs.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string_view>
#include <utility>
#include <vector>

namespace hs
{
namespace
{

struct Transform2D
{
    float x{};
    float z{};
    float yaw{};
};

struct Velocity2D
{
    float x{};
    float z{};
};

struct RenderComponent
{
    RenderMesh mesh{};
    std::uint32_t color{};
    Float3 scale{1.0f, 1.0f, 1.0f};
};

struct Player
{
};

struct Enemy
{
};

struct ChecksumEntry
{
    std::uint64_t entity{};
    Transform2D transform{};
    Velocity2D velocity{};
    RenderComponent render{};
};

void HashBytes(GameplayChecksum &hash, const void *data, std::size_t size) noexcept
{
    const auto *bytes = static_cast<const std::byte *>(data);
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= std::to_integer<std::uint8_t>(bytes[index]);
        hash *= 1099511628211ull;
    }
}

} // namespace

struct GameSimulation::Impl
{
    Impl() : render_query(world.query<const Transform2D, const RenderComponent>()),
             checksum_query(
                 world.query<const Transform2D, const Velocity2D, const RenderComponent>())
    {
        checksum_entries.reserve(101);
        events.reserve(32);
        particle_spawns.reserve(32);
    }

    flecs::world world;
    flecs::query<const Transform2D, const RenderComponent> render_query;
    flecs::query<const Transform2D, const Velocity2D, const RenderComponent> checksum_query;
    flecs::entity player;
    std::vector<ChecksumEntry> checksum_entries;
    std::vector<PresentationEvent> events;
    std::vector<ParticleSpawnCommand> particle_spawns;
    Tick tick{};
    Sequence event_sequence{};
    GameplayChecksum checksum{};
    std::uint64_t seed{};
    bool initialized{};
};

GameSimulation::GameSimulation() : impl_(std::make_unique<Impl>())
{
}

GameSimulation::~GameSimulation() = default;

Result GameSimulation::Initialize(const SimulationConfig &config)
{
    if (impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                               "GameSimulation already initialized.");
    }

    impl_->seed = config.seed;
    impl_->world.set_threads(1);

    auto previous_phase = impl_->world.entity("InputPhase")
                              .add(flecs::Phase)
                              .depends_on(flecs::OnUpdate);
    constexpr std::string_view phase_names[] = {
        "SessionTimerPhase",  "SpawnPhase",          "AiIntentPhase",
        "CastAttackPhase",    "MovementPhase",       "SpatialGridPhase",
        "CollisionHitPhase",  "DamageStatusPhase",   "DeathDropPhase",
        "XpCardPhase",        "StructuralMergePhase", "ChecksumSnapshotPhase",
    };

    flecs::entity movement_phase;
    for (const auto name : phase_names)
    {
        auto phase = impl_->world.entity(name.data()).add(flecs::Phase).depends_on(previous_phase);
        if (name == "MovementPhase")
        {
            movement_phase = phase;
        }
        previous_phase = phase;
    }

    impl_->world.system<Transform2D, const Velocity2D>("Move")
        .kind(movement_phase)
        .each([](flecs::iter &iterator, std::size_t, Transform2D &transform,
                 const Velocity2D &velocity) {
            const auto delta = iterator.delta_time();
            transform.x += velocity.x * delta;
            transform.z += velocity.z * delta;
            if (velocity.x != 0.0f || velocity.z != 0.0f)
            {
                transform.yaw = std::atan2(velocity.x, velocity.z);
            }
        });

    impl_->player =
        impl_->world.entity("Archer")
            .set<Transform2D>({0.0f, 0.0f, 0.0f})
            .set<Velocity2D>({})
            .set<RenderComponent>(
                {RenderMesh::Archer, 0xFF60B8FFu, Float3{0.8f, 1.8f, 0.8f}})
            .add<Player>();

    constexpr float golden_angle = 2.39996322972865332f;
    for (std::uint32_t index = 0; index < 100; ++index)
    {
        const auto angle = golden_angle * static_cast<float>(index);
        const auto radius = 9.0f + static_cast<float>(index % 10) * 1.25f;
        const auto tangent_speed = 0.35f + static_cast<float>(index % 4) * 0.08f;
        impl_->world.entity()
            .set<Transform2D>({std::cos(angle) * radius, std::sin(angle) * radius, angle})
            .set<Velocity2D>(
                {-std::sin(angle) * tangent_speed, std::cos(angle) * tangent_speed})
            .set<RenderComponent>(
                {RenderMesh::Enemy, 0xFF5D66E8u, Float3{0.75f, 1.1f, 0.75f}})
            .add<Enemy>();
    }

    impl_->initialized = true;
    impl_->checksum = ComputeChecksum();
    return Result::Success();
}

TickResult GameSimulation::TickFixed(const InputFrame &input,
                                     std::chrono::nanoseconds fixed_delta)
{
    if (!impl_->initialized)
    {
        return {};
    }

    auto movement = input.held.normalized_move;
    const auto length_squared = movement.x * movement.x + movement.y * movement.y;
    if (length_squared > 1.0f)
    {
        const auto inverse_length = 1.0f / std::sqrt(length_squared);
        movement.x *= inverse_length;
        movement.y *= inverse_length;
    }
    impl_->player.set<Velocity2D>({movement.x * 5.0f, movement.y * 5.0f});

    impl_->world.progress(std::chrono::duration<float>(fixed_delta).count());
    ++impl_->tick;
    impl_->checksum = ComputeChecksum();

    if (impl_->tick % 60 == 0)
    {
        PresentationEvent event;
        event.sequence = ++impl_->event_sequence;
        event.tick = impl_->tick;
        event.kind = PresentationKind::Vfx;
        event.asset = {0x68735F70756C7365ull};
        impl_->events.push_back(event);

        ParticleSpawnCommand particles;
        particles.sequence = event.sequence;
        particles.tick = impl_->tick;
        particles.position = {0.0f, 0.4f, 0.0f};
        particles.lifetime = 4.0f;
        particles.velocity = {0.0f, 2.5f, 0.0f};
        particles.velocity_spread = 3.5f;
        particles.start_color = {0.15f, 0.75f, 1.0f, 0.32f};
        particles.end_color = {0.45f, 0.2f, 1.0f, 0.0f};
        particles.start_size = 0.08f;
        particles.end_size = 0.01f;
        particles.gravity = -0.35f;
        particles.angular_velocity = 1.5f;
        particles.sprite_count = 4;
        particles.count = impl_->tick % 180 == 0 ? 3'334u : 3'333u;
        impl_->particle_spawns.push_back(particles);
    }

    return {impl_->tick, impl_->checksum};
}

GameplayChecksum GameSimulation::ComputeChecksum() const
{
    GameplayChecksum hash = 14695981039346656037ull;
    HashBytes(hash, &impl_->seed, sizeof(impl_->seed));
    HashBytes(hash, &impl_->tick, sizeof(impl_->tick));

    impl_->checksum_entries.clear();
    impl_->checksum_query.each(
        [this](flecs::entity entity, const Transform2D &transform, const Velocity2D &velocity,
               const RenderComponent &render) {
            impl_->checksum_entries.push_back({entity.id(), transform, velocity, render});
        });
    std::ranges::sort(impl_->checksum_entries, {}, &ChecksumEntry::entity);
    for (const auto &entry : impl_->checksum_entries)
    {
        HashBytes(hash, &entry.entity, sizeof(entry.entity));
        HashBytes(hash, &entry.transform.x, sizeof(entry.transform.x));
        HashBytes(hash, &entry.transform.z, sizeof(entry.transform.z));
        HashBytes(hash, &entry.transform.yaw, sizeof(entry.transform.yaw));
        HashBytes(hash, &entry.velocity.x, sizeof(entry.velocity.x));
        HashBytes(hash, &entry.velocity.z, sizeof(entry.velocity.z));
        HashBytes(hash, &entry.render.mesh, sizeof(entry.render.mesh));
        HashBytes(hash, &entry.render.color, sizeof(entry.render.color));
        HashBytes(hash, &entry.render.scale.x, sizeof(entry.render.scale.x));
        HashBytes(hash, &entry.render.scale.y, sizeof(entry.render.scale.y));
        HashBytes(hash, &entry.render.scale.z, sizeof(entry.render.scale.z));
    }
    return hash;
}

bool GameSimulation::WriteRenderSnapshot(RenderSnapshotStorage &snapshot) const
{
    snapshot.header.tick = impl_->tick;
    snapshot.header.simulation_time = std::chrono::nanoseconds(16'666'667) * impl_->tick;
    snapshot.header.checksum = impl_->checksum;
    snapshot.camera = {};

    bool complete = true;
    std::uint32_t instance_index{};
    impl_->render_query.each([&](flecs::entity entity, const Transform2D &transform,
                                 const RenderComponent &render) {
        complete &= snapshot.AddInstance(
            {{transform.x, 0.0f, transform.z}, transform.yaw, render.scale, render.color,
             render.mesh});
        if (entity == impl_->player)
        {
            complete &= snapshot.AddPose(
                {instance_index, static_cast<float>(impl_->tick % 137) / 137.0f});
        }
        ++instance_index;
    });

    complete &= snapshot.AddLight(
        {{-0.45f, -0.82f, 0.35f}, 3.0f, {1.0f, 0.92f, 0.78f}});

    UiModel ui;
    constexpr char text[] = "\xED\x94\x84\xEB\xA1\x9C\xEC\xA0\x9D\xED\x8A\xB8 HS";
    std::memcpy(ui.utf8_text.data(), text, sizeof(text));
    complete &= snapshot.AddUi(ui);
    return complete;
}

std::span<const PresentationEvent> GameSimulation::PendingPresentationEvents() const noexcept
{
    return impl_->events;
}

void GameSimulation::ClearPresentationEvents() noexcept
{
    impl_->events.clear();
}

std::span<const ParticleSpawnCommand> GameSimulation::PendingParticleSpawns() const noexcept
{
    return impl_->particle_spawns;
}

void GameSimulation::ClearParticleSpawns() noexcept
{
    impl_->particle_spawns.clear();
}

Result GameSimulation::Shutdown()
{
    if (!impl_->initialized)
    {
        return Result::Success();
    }
    impl_->world.quit();
    impl_->initialized = false;
    return Result::Success();
}

} // namespace hs
