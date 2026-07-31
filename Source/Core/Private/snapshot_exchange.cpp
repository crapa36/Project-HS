#include <hs/core/snapshot_exchange.hpp>

#include <limits>

namespace hs
{

RenderSnapshotExchange::RenderSnapshotExchange(std::size_t instance_capacity,
                                               std::size_t pose_capacity,
                                               std::size_t light_capacity,
                                               std::size_t ui_capacity)
    : slots_{Slot(instance_capacity, pose_capacity, light_capacity, ui_capacity),
             Slot(instance_capacity, pose_capacity, light_capacity, ui_capacity),
             Slot(instance_capacity, pose_capacity, light_capacity, ui_capacity)}
{
}

std::optional<RenderSnapshotExchange::WriteSlot> RenderSnapshotExchange::TryBeginWrite() noexcept
{
    for (std::size_t index = 0; index < slots_.size(); ++index)
    {
        auto expected = SlotState::Free;
        if (slots_[index].state.compare_exchange_strong(expected, SlotState::Writing,
                                                        std::memory_order_acquire))
        {
            slots_[index].storage.Clear();
            return WriteSlot{index, &slots_[index].storage};
        }
    }

    std::optional<std::size_t> oldest;
    auto oldest_sequence = std::numeric_limits<Sequence>::max();
    for (std::size_t index = 0; index < slots_.size(); ++index)
    {
        if (slots_[index].state.load(std::memory_order_acquire) == SlotState::Ready)
        {
            const auto sequence = slots_[index].publish_sequence.load(std::memory_order_relaxed);
            if (sequence < oldest_sequence)
            {
                oldest = index;
                oldest_sequence = sequence;
            }
        }
    }

    if (oldest)
    {
        auto expected = SlotState::Ready;
        if (slots_[*oldest].state.compare_exchange_strong(expected, SlotState::Writing,
                                                          std::memory_order_acquire))
        {
            slots_[*oldest].storage.Clear();
            return WriteSlot{*oldest, &slots_[*oldest].storage};
        }
    }

    dropped_publishes_.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
}

void RenderSnapshotExchange::Publish(WriteSlot slot, Sequence publish_sequence) noexcept
{
    slots_[slot.index].publish_sequence.store(publish_sequence, std::memory_order_relaxed);
    slots_[slot.index].state.store(SlotState::Ready, std::memory_order_release);
}

void RenderSnapshotExchange::Abandon(WriteSlot slot) noexcept
{
    slots_[slot.index].state.store(SlotState::Free, std::memory_order_release);
}

std::uint64_t RenderSnapshotExchange::DroppedPublishes() const noexcept
{
    return dropped_publishes_.load(std::memory_order_relaxed);
}

std::optional<std::size_t> RenderSnapshotExchange::AcquireNewestReady() noexcept
{
    for (;;)
    {
        std::optional<std::size_t> newest;
        Sequence newest_sequence{};
        for (std::size_t index = 0; index < slots_.size(); ++index)
        {
            if (slots_[index].state.load(std::memory_order_acquire) == SlotState::Ready)
            {
                const auto sequence =
                    slots_[index].publish_sequence.load(std::memory_order_relaxed);
                if (!newest || sequence > newest_sequence)
                {
                    newest = index;
                    newest_sequence = sequence;
                }
            }
        }

        if (!newest)
        {
            return std::nullopt;
        }

        auto expected = SlotState::Ready;
        if (slots_[*newest].state.compare_exchange_strong(expected, SlotState::Reading,
                                                          std::memory_order_acquire))
        {
            return newest;
        }
    }
}

void RenderSnapshotExchange::ReleaseRead(std::optional<std::size_t> &index) noexcept
{
    if (index)
    {
        slots_[*index].state.store(SlotState::Free, std::memory_order_release);
        index.reset();
    }
}

RenderSnapshotExchange::Consumer::Consumer(RenderSnapshotExchange &exchange) noexcept
    : exchange_(&exchange)
{
}

RenderSnapshotExchange::Consumer::~Consumer()
{
    exchange_->ReleaseRead(previous_);
    exchange_->ReleaseRead(current_);
}

RenderSnapshotExchange::ReadPair RenderSnapshotExchange::Consumer::AcquireLatest() noexcept
{
    if (const auto newest = exchange_->AcquireNewestReady())
    {
        exchange_->ReleaseRead(previous_);
        previous_ = current_;
        current_ = newest;
    }

    ReadPair pair;
    if (previous_)
    {
        pair.previous = exchange_->slots_[*previous_].storage.View();
        pair.has_previous = true;
    }
    if (current_)
    {
        pair.current = exchange_->slots_[*current_].storage.View();
        pair.has_current = true;
    }
    return pair;
}

} // namespace hs
