#pragma once

#include <hs/core/render_snapshot.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace hs
{

class RenderSnapshotExchange
{
  public:
    struct WriteSlot
    {
        std::size_t index{};
        RenderSnapshotStorage *storage{};
    };

    struct ReadPair
    {
        RenderSnapshot previous{};
        RenderSnapshot current{};
        bool has_previous{};
        bool has_current{};
    };

    class Consumer
    {
      public:
        explicit Consumer(RenderSnapshotExchange &exchange) noexcept;
        ~Consumer();

        Consumer(const Consumer &) = delete;
        Consumer &operator=(const Consumer &) = delete;

        [[nodiscard]] ReadPair AcquireLatest() noexcept;

      private:
        RenderSnapshotExchange *exchange_{};
        std::optional<std::size_t> previous_;
        std::optional<std::size_t> current_;
    };

    RenderSnapshotExchange(std::size_t instance_capacity, std::size_t pose_capacity,
                           std::size_t light_capacity, std::size_t ui_capacity);

    [[nodiscard]] std::optional<WriteSlot> TryBeginWrite() noexcept;
    void Publish(WriteSlot slot, Sequence publish_sequence) noexcept;
    void Abandon(WriteSlot slot) noexcept;
    [[nodiscard]] std::uint64_t DroppedPublishes() const noexcept;

  private:
    enum class SlotState : std::uint8_t
    {
        Free,
        Writing,
        Ready,
        Reading,
    };

    struct Slot
    {
        Slot(std::size_t instance_capacity, std::size_t pose_capacity, std::size_t light_capacity,
             std::size_t ui_capacity)
            : storage(instance_capacity, pose_capacity, light_capacity, ui_capacity)
        {
        }

        RenderSnapshotStorage storage;
        std::atomic<SlotState> state{SlotState::Free};
        std::atomic<Sequence> publish_sequence{};
    };

    [[nodiscard]] std::optional<std::size_t> AcquireNewestReady() noexcept;
    void ReleaseRead(std::optional<std::size_t> &index) noexcept;

    std::array<Slot, 3> slots_;
    std::atomic<std::uint64_t> dropped_publishes_{};

    friend class Consumer;
};

} // namespace hs
