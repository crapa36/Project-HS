#pragma once
#include "renderer_ray_geometry.hpp"
#include "view_state.hpp"
#include <array>
#include <memory>
#include <string>

namespace D3D12MA {class Allocator;}

namespace hs
{
enum class RayNativeSignal : std::uint8_t { Diffuse,Specular };
struct RayNativeTraceSelection
{
    bool use_omms{},use_ser{};
    bool operator==(const RayNativeTraceSelection &)const=default;
};
struct RaySerProfile
{
    bool enabled{},use_omms{},active_gi{},active_hybrid{};
    std::uint32_t frame_slots{};
    std::uint64_t native_byte_budget{},old_service_bytes{};
};
struct RaySerStatus
{
    bool enabled{},ready{},applied{},usable{true};
    bool use_omms{};
    std::uint64_t allocated_bytes{};
    std::string reason{"Shader execution reordering is Off."};
};
struct RaySerStorage;
class RaySerCandidate
{
  public:[[nodiscard]] std::uint64_t AllocatedBytes()const noexcept;
  private:std::shared_ptr<RaySerStorage> storage_;friend class RayShaderExecutionReorderingSystem;
};
class RayShaderExecutionReorderingSystem
{
  public:
    RayShaderExecutionReorderingSystem();~RayShaderExecutionReorderingSystem();
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,ID3D12RootSignature *,
        const RaySerProfile &,const RayShaderReader &,RaySerCandidate &)const;
    [[nodiscard]] Result Commit(RaySerCandidate &&,const CompletionSet &);
    // Adds the real immutable raygen shader-table graph read to an existing
    // trace pass and returns its checked DispatchRays callback. The caller owns
    // the existing exact trace guide/AS/read/write declarations and pass IDs.
    [[nodiscard]] Result RegisterDispatch(RenderGraphBuilder &,PassBuilder &,RayNativeSignal,
        ViewExtent,std::uint32_t frame_slot,const CompletionSet &completed,
        RayShaderBinder, std::function<Result(RenderPassContext &)> &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] RaySerStatus Status()const;
    [[nodiscard]] std::uint64_t AllocatedBytes()const noexcept;
    void SwapState(RayShaderExecutionReorderingSystem &other)noexcept{impl_.swap(other.impl_);}
  private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
