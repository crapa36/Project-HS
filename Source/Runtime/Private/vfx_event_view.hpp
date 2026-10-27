#pragma once

#include <hs/render_data/vfx_frame_input.hpp>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <span>
#include <type_traits>
#include <limits>

namespace hs::runtime_detail
{
// Borrowed ordered view over event rows, optionally selected indirectly.
// Backing spans must outlive the view and its consumers.
class VfxEventView
{
  public:
    static constexpr std::uint32_t kNoBinding = std::numeric_limits<std::uint32_t>::max();
    VfxEventView() noexcept = default;
    VfxEventView(std::span<const VfxEventInput> rows,
                 std::span<const std::uint32_t> indices) noexcept
        : rows_(rows), indices_(indices), indexed_(true) {}
    VfxEventView(std::span<const VfxEventInput> canonical_rows,
                 std::span<const std::uint32_t> profile_indices,
                 std::span<const VfxEventInput> frame_bound_rows,
                 std::span<const std::uint32_t> binding_indices) noexcept
        : rows_(canonical_rows), indices_(profile_indices), frame_bound_rows_(frame_bound_rows),
          binding_indices_(binding_indices), indexed_(true), has_bindings_(true) {}

    template <typename Range>
        requires std::ranges::contiguous_range<const Range> &&
                 std::ranges::sized_range<const Range> &&
                 std::same_as<std::remove_cv_t<std::ranges::range_value_t<const Range>>, VfxEventInput>
    VfxEventView(const Range &rows) noexcept
        : rows_(std::ranges::data(rows), std::ranges::size(rows)) {}

    [[nodiscard]] std::size_t size() const noexcept
    {
        return indexed_ ? indices_.size() : rows_.size();
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] const VfxEventInput &operator[](std::size_t index) const noexcept
    {
        const auto canonical_index = indexed_ ? indices_[index] : index;
        if (has_bindings_ && canonical_index < binding_indices_.size())
        {
            const auto binding = binding_indices_[canonical_index];
            if (binding != kNoBinding && binding < frame_bound_rows_.size())
                return frame_bound_rows_[binding];
        }
        return rows_[canonical_index];
    }

    class Iterator
    {
      public:
        using value_type = VfxEventInput;
        using difference_type = std::ptrdiff_t;
        using reference = const VfxEventInput &;
        using pointer = const VfxEventInput *;
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept = std::forward_iterator_tag;

        Iterator() noexcept = default;
        Iterator(const VfxEventView *view, std::size_t index) noexcept : view_(view), index_(index) {}
        [[nodiscard]] reference operator*() const noexcept { return (*view_)[index_]; }
        [[nodiscard]] pointer operator->() const noexcept { return &(*view_)[index_]; }
        Iterator &operator++() noexcept { ++index_; return *this; }
        Iterator operator++(int) noexcept { auto copy = *this; ++*this; return copy; }
        friend bool operator==(Iterator, Iterator) noexcept = default;

      private:
        const VfxEventView *view_{};
        std::size_t index_{};
    };

    [[nodiscard]] Iterator begin() const noexcept { return {this, 0}; }
    [[nodiscard]] Iterator end() const noexcept { return {this, size()}; }

  private:
    std::span<const VfxEventInput> rows_;
    std::span<const std::uint32_t> indices_;
    std::span<const VfxEventInput> frame_bound_rows_;
    std::span<const std::uint32_t> binding_indices_;
    bool indexed_{};
    bool has_bindings_{};
};
}
