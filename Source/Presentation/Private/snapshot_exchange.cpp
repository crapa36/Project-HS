#include <hs/presentation/snapshot_exchange.hpp>
#include <hs/presentation/projector.hpp>

#include <limits>
#include <utility>

namespace hs
{

RenderSnapshotExchange::WriteSlot::WriteSlot(WriteSlot &&other) noexcept
    : exchange_(std::exchange(other.exchange_, nullptr)), index_(other.index_)
{
}

RenderSnapshotExchange::WriteSlot &RenderSnapshotExchange::WriteSlot::operator=(WriteSlot &&other) noexcept
{
    if (this != &other)
    {
        std::move(*this).Abandon();
        exchange_ = std::exchange(other.exchange_, nullptr);
        index_ = other.index_;
    }
    return *this;
}

RenderSnapshotExchange::WriteSlot::~WriteSlot()
{
    std::move(*this).Abandon();
}

RenderSnapshotStorage &RenderSnapshotExchange::WriteSlot::Storage() const noexcept
{
    return exchange_->slots_[index_].storage;
}

void RenderSnapshotExchange::WriteSlot::Publish(Sequence publish_sequence,
    const RenderSnapshot *previous_sample, std::shared_ptr<const UiFrame> ui_frame) &&
{
    if (!exchange_) return;
    exchange_->PublishSlot(index_, publish_sequence, previous_sample, std::move(ui_frame));
    exchange_ = nullptr;
}

void RenderSnapshotExchange::WriteSlot::Abandon() && noexcept
{
    if (!exchange_) return;
    auto *exchange = std::exchange(exchange_, nullptr);
    exchange->AbandonSlot(index_);
}

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
    return AcquireWriteSlot(false);
}

std::optional<RenderSnapshotExchange::WriteSlot> RenderSnapshotExchange::TryBeginRetainedWrite() noexcept
{
    return AcquireWriteSlot(true);
}

std::optional<RenderSnapshotExchange::WriteSlot> RenderSnapshotExchange::AcquireWriteSlot(bool retained) noexcept
{
    for (std::size_t index = 0; index < slots_.size(); ++index)
    {
        auto expected = SlotState::Free;
        if (slots_[index].state.compare_exchange_strong(expected, SlotState::Writing,
                                                        std::memory_order_acquire))
        {
            if (!retained) slots_[index].storage.Clear();
            slots_[index].retained_write = retained;
            slots_[index].ui_frame.reset();
            return WriteSlot(this, index);
        }
    }

    std::optional<std::size_t> oldest;
    auto oldest_sequence = std::numeric_limits<Sequence>::max();
    for (std::size_t index = 0; index < slots_.size(); ++index)
    {
        if (slots_[index].state.load(std::memory_order_acquire) == SlotState::Ready)
        {
            const auto sequence = slots_[index].publish_sequence.load(std::memory_order_relaxed);
            if (!oldest || sequence < oldest_sequence)
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
            if (!retained) slots_[*oldest].storage.Clear();
            slots_[*oldest].retained_write = retained;
            slots_[*oldest].ui_frame.reset();
            return WriteSlot(this, *oldest);
        }
    }

    dropped_publishes_.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
}

void RenderSnapshotExchange::PublishSlot(std::size_t index, Sequence publish_sequence,
                                         const RenderSnapshot *previous_sample,
                                         std::shared_ptr<const UiFrame> ui_frame)
{
    auto &packet = slots_[index];
    const auto &current = packet.storage.header;
    packet.has_previous = previous_sample &&
        previous_sample->header.session_id == current.session_id &&
        previous_sample->header.tick < current.tick &&
        current.tick - previous_sample->header.tick == 1;
    if (packet.has_previous)
    {
        if (packet.retained_write) (void)packet.previous.CopyRetainedDynamicFrom(*previous_sample);
        else packet.previous.CopyDynamicFrom(*previous_sample);
    }
    else packet.previous.Clear();
    packet.ui_frame = std::move(ui_frame);
    slots_[index].publish_sequence.store(publish_sequence, std::memory_order_relaxed);
    slots_[index].state.store(SlotState::Ready, std::memory_order_release);
}

void RenderSnapshotExchange::AbandonSlot(std::size_t index) noexcept
{
    slots_[index].ui_frame.reset();
    slots_[index].state.store(SlotState::Free, std::memory_order_release);
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
    exchange_->ReleaseRead(current_);
}

RenderSnapshotExchange::ReadPair RenderSnapshotExchange::Consumer::AcquireLatest() noexcept
{
    if (auto newest = exchange_->AcquireNewestReady())
    {
        // Inspect the sequence only after claiming Reading: a Ready slot can
        // be overwritten by the producer, while both retained slots are owned.
        if (current_ &&
            exchange_->slots_[*newest].publish_sequence.load(std::memory_order_relaxed) <=
                exchange_->slots_[*current_].publish_sequence.load(std::memory_order_relaxed))
            exchange_->ReleaseRead(newest);
        else
        {
            exchange_->ReleaseRead(current_);
            current_ = newest;
        }
    }

    ReadPair pair;
    if (current_)
    {
        const auto &packet = exchange_->slots_[*current_];
        pair.current = packet.storage.View();
        pair.ui_frame = packet.ui_frame;
        if (pair.ui_frame && pair.ui_frame->draw_content)
            pair.current.ui = pair.ui_frame->draw_content->models;
        pair.has_previous = packet.has_previous;
        if (pair.has_previous) pair.previous = packet.previous.View();
        pair.current_publication_sequence =
            exchange_->slots_[*current_].publish_sequence.load(std::memory_order_relaxed);
        pair.has_current = true;
    }
    return pair;
}

} // namespace hs
