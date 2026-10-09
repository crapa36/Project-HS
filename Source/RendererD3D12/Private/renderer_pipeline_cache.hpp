#pragma once

#include <hs/core/result.hpp>
#include <d3d12.h>
#include <dxgi.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace hs
{
// Copied under the cache mutex. No COM handles, native descriptors or borrowed
// strings escape. Failures of optional persistence/storage remain observable.
struct RendererPipelineCacheStats
{
    std::uint64_t hits{},misses{},native_creates{},stores{},store_failures{},load_failures{};
    std::uint64_t io_failures{},discarded_files{},unsupported_libraries{},uncached_mesh_creates{};
    std::uint64_t root_registrations{},key_failures{},loaded_bytes{},serialized_bytes{};
    std::int32_t last_cache_hresult{},last_io_error{};
    bool initialized{},library_supported{},mesh_library_supported{},persistence_enabled{};
};

// One device's optional D3D12 pipeline library. Register each root immediately
// after native creation using its actual serialized bytes. Registration stores
// a 32-byte private-data digest on the root itself; no cache registry retains old
// roots. Unregistered roots are rejected even when native caching is unsupported.
// Empty directory selects an in-memory library. Persistent files are disposable,
// adapter/driver/schema-specific, bounded at 256 MiB, and atomically replaced.
// Caller explicitly observes Flush failures before Reset/shutdown. Reset and the
// destructor perform no IO. Reset retains final diagnostics; Initialize starts
// fresh diagnostics. All library loads/stores/serialization are serialized
// so two preparations cannot concurrently Load the same library name.
class RendererPipelineCache
{
  public:
    RendererPipelineCache();
    ~RendererPipelineCache();
    RendererPipelineCache(const RendererPipelineCache &)=delete;
    RendererPipelineCache &operator=(const RendererPipelineCache &)=delete;
    [[nodiscard]] Result Initialize(ID3D12Device *device,IDXGIAdapter *adapter,
        std::filesystem::path cache_directory);
    [[nodiscard]] Result RegisterRootSignature(ID3D12RootSignature *root,
        std::span<const std::byte> serialized);
    [[nodiscard]] HRESULT CreateGraphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *description,
        REFIID iid,void **output);
    [[nodiscard]] HRESULT CreateCompute(const D3D12_COMPUTE_PIPELINE_STATE_DESC *description,
        REFIID iid,void **output);
    [[nodiscard]] HRESULT CreateMesh(ID3D12Device2 *device,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC &graphics,D3D12_SHADER_BYTECODE mesh_shader,
        ID3D12PipelineState **output);
    [[nodiscard]] Result Flush();
    void Reset() noexcept;
    [[nodiscard]] RendererPipelineCacheStats Stats() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
