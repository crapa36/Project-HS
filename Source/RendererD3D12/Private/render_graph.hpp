#pragma once

#include <hs/core/result.hpp>
#include <hs/core/types.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace hs
{

enum class QueueHint : std::uint8_t
{
    Direct,
    Compute,
    Copy,
};

enum class Access : std::uint8_t
{
    Undefined,
    Common,
    ShaderRead,
    RenderTarget,
    DepthRead,
    DepthWrite,
    UnorderedRead,
    UnorderedWrite,
    CopySource,
    CopyDest,
    IndirectArgs,
    Present,
};

enum class ResourceKind : std::uint8_t
{
    Texture,
    Buffer,
};

struct ResourceHandle
{
    std::uint32_t value{};
    ResourceKind kind{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value != 0;
    }
};

using TextureHandle = ResourceHandle;
using BufferHandle = ResourceHandle;

struct TextureDesc
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t format{};
    std::uint32_t flags{};
};

struct BufferDesc
{
    std::uint64_t size{};
    std::uint32_t flags{};
};

struct ExternalTexture
{
    void *native_resource{};
};

struct ExternalBuffer
{
    void *native_resource{};
};

class RenderPassContext
{
  public:
    [[nodiscard]] void *NativeCommandList() const noexcept;
    [[nodiscard]] void *Resolve(ResourceHandle handle) const noexcept;
    void UavBarrier(ResourceHandle handle) const noexcept;

  private:
    RenderPassContext(void *native_command_list, void *enhanced_command_list,
                      BarrierMode barrier_mode, const void *graph) noexcept;

    void *native_command_list_{};
    void *enhanced_command_list_{};
    BarrierMode barrier_mode_{};
    const void *graph_{};

    friend class RenderGraphBuilder;
};

class PassBuilder
{
  public:
    void Read(ResourceHandle resource, Access access);
    void Write(ResourceHandle resource, Access access);
    void ReadWrite(ResourceHandle resource, Access access);
    void SetExecute(std::move_only_function<void(RenderPassContext &)> execute);

  private:
    PassBuilder(void *graph, std::uint32_t pass) noexcept;

    void *graph_{};
    std::uint32_t pass_{};

    friend class RenderGraphBuilder;
};

class RenderGraphBuilder
{
  public:
    RenderGraphBuilder();
    ~RenderGraphBuilder();

    RenderGraphBuilder(const RenderGraphBuilder &) = delete;
    RenderGraphBuilder &operator=(const RenderGraphBuilder &) = delete;

    void Reset();
    [[nodiscard]] TextureHandle CreateTexture(const TextureDesc &description,
                                              std::string_view name);
    [[nodiscard]] BufferHandle CreateBuffer(const BufferDesc &description,
                                            std::string_view name);
    [[nodiscard]] TextureHandle ImportTexture(const ExternalTexture &texture, Access initial,
                                              std::string_view name);
    [[nodiscard]] BufferHandle ImportBuffer(const ExternalBuffer &buffer, Access initial,
                                            std::string_view name);
    [[nodiscard]] PassBuilder AddPass(std::string_view name, QueueHint queue);
    void SetFinalAccess(ResourceHandle resource, Access access);

    [[nodiscard]] Result Execute(void *native_command_list, void *enhanced_command_list,
                                 BarrierMode barriers, void *timestamp_query_heap = nullptr,
                                 void *timestamp_readback = nullptr,
                                 std::uint32_t timestamp_base = 0);
    [[nodiscard]] std::uint32_t PassCount() const noexcept;
    [[nodiscard]] Access FinalAccess(ResourceHandle resource) const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class PassBuilder;
    friend class RenderPassContext;
};

} // namespace hs
