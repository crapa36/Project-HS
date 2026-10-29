#pragma once

#include <hs/render_data/render_snapshot_storage.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace hs
{

struct UiFrame;

// Single producer and single consumer; slots retain snapshot storage for readers.
class RenderSnapshotExchange
{
  public:
    struct WriteSlot
    {
        std::size_t index{};
        RenderSnapshotStorage *storage{};
    };

    struct ReadPair : RenderSnapshotPair
    {
        std::shared_ptr<const UiFrame> ui_frame;
    };

    class Consumer
    {
      public:
        explicit Consumer(RenderSnapshotExchange &exchange) noexcept;
        ~Consumer();

        Consumer(const Consumer &) = delete;
        Consumer &operator=(const Consumer &) = delete;

        // Repeated reads retain the pair until a strictly newer publication is
        // claimed. Returned spans remain owned until the next accepted update.
        [[nodiscard]] ReadPair AcquireLatest() noexcept;

      private:
        RenderSnapshotExchange *exchange_{};
        std::optional<std::size_t> current_;
    };

    RenderSnapshotExchange(std::size_t instance_capacity, std::size_t pose_capacity,
                           std::size_t light_capacity, std::size_t ui_capacity);

    [[nodiscard]] std::optional<WriteSlot> TryBeginWrite() noexcept;
    // Preserves slot arrays for CopyRetainedDynamicFrom; the caller still updates
    // the current sample before Publish. Normal writers receive cleared storage.
    [[nodiscard]] std::optional<WriteSlot> TryBeginRetainedWrite() noexcept;
    // One producer supplies increasing publication sequences independently of
    // simulation ticks. Reset/wrap requires a new exchange and consumer.
    void Publish(WriteSlot slot, Sequence publish_sequence,
                 const RenderSnapshot *previous_sample = nullptr,
                 std::shared_ptr<const UiFrame> ui_frame = {});
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
            : storage(instance_capacity, pose_capacity, light_capacity, ui_capacity),
              previous(0, 0, 0, 0)
        {
        }

        RenderSnapshotStorage storage;
        RenderSnapshotStorage previous;
        std::shared_ptr<const UiFrame> ui_frame;
        bool has_previous{};
        bool retained_write{};
        std::atomic<SlotState> state{SlotState::Free};
        std::atomic<Sequence> publish_sequence{};
    };

    [[nodiscard]] std::optional<std::size_t> AcquireNewestReady() noexcept;
    [[nodiscard]] std::optional<WriteSlot> AcquireWriteSlot(bool retained) noexcept;
    void ReleaseRead(std::optional<std::size_t> &index) noexcept;

    std::array<Slot, 3> slots_;
    std::atomic<std::uint64_t> dropped_publishes_{};

    friend class Consumer;
};

} // namespace hs
