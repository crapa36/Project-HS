#pragma once

#include "render_graph.hpp"
#include <d3d12.h>
#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace hs
{
struct LtcLutStorage;
class LtcLutCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<LtcLutStorage> storage_;
    friend class LtcLutSystem;
};
struct LtcLutOutputs
{
    ResourceVersion matrix, amplitude;
    ID3D12Resource *matrix_resource{}, *amplitude_resource{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE,2> srvs{}; // t4/t5 space19.
};
struct LtcLutLayout
{
    bool valid{}, active{}, upload{};
    std::array<ResourceHandle,3> resources{};
    std::array<ResourceVersion,2> outputs{};
    std::uint32_t pass{};
};
// Immutable produced LUTs, independent of view resolution. Empty package bytes
// prepare an inactive service with no allocation. Register stages one direct
// copy pass (140) on first use, then imports exact state/completion requirements.
// Consumers declare reads of both versions; CompleteSubmission is mandatory
// after successful or partial/failed graph submission. Staging is released only
// after its GPU lease completes. Replacements/reset require completed leases.
class LtcLutSystem
{
  public:
    LtcLutSystem();
    ~LtcLutSystem();
    LtcLutSystem(const LtcLutSystem &)=delete;
    LtcLutSystem &operator=(const LtcLutSystem &)=delete;
    [[nodiscard]] Result Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,
        std::span<const std::byte> package_bytes,LtcLutCandidate &candidate) const;
    [[nodiscard]] Result Commit(LtcLutCandidate &&candidate,const CompletionSet &completed);
    [[nodiscard]] Result Register(RenderGraphBuilder &graph,const CompletionSet &completed,LtcLutOutputs &outputs,
        LtcLutLayout *layout=nullptr);
    [[nodiscard]] bool MatchesLayout(const LtcLutLayout &)const noexcept;
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &,const CompletionSet &,const LtcLutLayout &,LtcLutOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Called only after all old submissions have reliably completed.
    void SwapState(LtcLutSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    [[nodiscard]] Result RegisterFrame(RenderGraphBuilder &,const CompletionSet &,LtcLutOutputs &,
        LtcLutLayout *,const LtcLutLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
