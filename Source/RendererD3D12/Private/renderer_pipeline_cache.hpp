#pragma once

#include <hs/core/result.hpp>
#include <d3d12.h>
#include <dxgi.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace hs
{
#if defined(HS_DEVELOPMENT_TOOLS)
struct RendererPsoCacheStats;
#endif

// One device's optional D3D12 pipeline library. Register each root immediately
// after native creation using its actual serialized bytes. Registration stores
// a 32-byte private-data digest on the root itself; no cache registry retains old
// roots. Unregistered roots are rejected even when native caching is unsupported.
// Empty directory selects an in-memory library. Persistent files are disposable,
// adapter/driver/schema-specific, bounded at 256 MiB, and atomically replaced.
// Caller explicitly observes Flush failures before Reset/shutdown. Reset and the
// destructor perform no IO. Reset retains final diagnostics; Initialize starts
// fresh diagnostics. Distinct PSO names may load/create/store concurrently;
// the cache synchronizes same-name loads and lifecycle mutations.
class RendererPipelineCache
{
  public:
    RendererPipelineCache();
    ~RendererPipelineCache();
    RendererPipelineCache(const RendererPipelineCache &)=delete;
    RendererPipelineCache &operator=(const RendererPipelineCache &)=delete;
    [[nodiscard]] Result Initialize(ID3D12Device *device,IDXGIAdapter *adapter,
        std::filesystem::path cache_directory,bool capture_timings=false);
    [[nodiscard]] Result RegisterRootSignature(ID3D12RootSignature *root,
        std::span<const std::byte> serialized);
    // Registers a checked, immutable shader owner for the current pipeline build.
    // Its address is only a temporary lookup; the semantic key stores the digest.
    // Set bytes_already_verified only when the selected asset reader checked this
    // same product digest while reading the bytes.
    [[nodiscard]] Result RegisterShaderOwner(std::span<const std::byte> shader,
        std::string_view expected_digest,bool bytes_already_verified);
    void ClearShaderOwners() noexcept;
    [[nodiscard]] HRESULT CreateGraphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *description,
        REFIID iid,void **output);
    [[nodiscard]] HRESULT CreateCompute(const D3D12_COMPUTE_PIPELINE_STATE_DESC *description,
        REFIID iid,void **output);
    [[nodiscard]] HRESULT CreateMesh(ID3D12Device2 *device,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC &graphics,D3D12_SHADER_BYTECODE mesh_shader,
        ID3D12PipelineState **output);
    [[nodiscard]] Result Flush();
    void Reset() noexcept;
#if defined(HS_DEVELOPMENT_TOOLS)
    [[nodiscard]] RendererPsoCacheStats Stats() const;
#endif
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
