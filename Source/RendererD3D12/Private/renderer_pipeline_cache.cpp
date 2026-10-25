#include "renderer_pipeline_cache.hpp"
#include <hs/core/sha256.hpp>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <iterator>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hs
{
namespace
{
using CacheClock = std::chrono::steady_clock;
CacheClock::time_point CacheStart(bool enabled) noexcept
{ return enabled ? CacheClock::now() : CacheClock::time_point{}; }
void RecordCacheDuration(CacheClock::time_point start, std::uint64_t &total,
                         std::uint64_t *maximum = nullptr) noexcept
{
    if (start == CacheClock::time_point{}) return;
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(CacheClock::now() - start).count());
    total += ns;
    if (maximum) *maximum = std::max(*maximum, ns);
}
struct CacheDuration
{
    CacheClock::time_point start;
    std::uint64_t &total;
    std::uint64_t *maximum;
    CacheDuration(bool enabled, std::uint64_t &sum, std::uint64_t *peak = nullptr) noexcept
        : start(CacheStart(enabled)), total(sum), maximum(peak) {}
    ~CacheDuration() { RecordCacheDuration(start, total, maximum); }
};

using Microsoft::WRL::ComPtr;
constexpr std::uint32_t kSchema=1;
constexpr std::uint64_t kMaximumFileBytes=256ull*1024*1024;
constexpr std::size_t kHeaderBytes=128;
constexpr std::array<std::byte,8> kMagic{std::byte{'H'},std::byte{'S'},std::byte{'P'},std::byte{'S'},
    std::byte{'O'},std::byte{'C'},std::byte{'0'},std::byte{'1'}};
// Dedicated registration property. Its bytes are only a digest, never a root
// pointer or ownership reference; native root destruction releases it.
constexpr GUID kRootDigestGuid{0x52b5d9b8,0xe8f6,0x48ec,{0x9d,0xa4,0x9f,0x40,0x75,0x19,0x42,0xc1}};
Result Failure(ErrorCode code,std::string message)
{
    return Result::Failure(code,"hs_renderer_pipeline_cache",std::move(message));
}
Result NativeFailure(std::string_view operation,HRESULT native)
{
    return Failure(ErrorCode::TaskFailed,std::string(operation)+" failed (HRESULT "+
        std::to_string(std::int32_t(native))+").");
}
struct KeyError {};
// Explicit little-endian field serialization into SHA256: no object padding,
// descriptor/string addresses or COM identities participate in persistent keys.
class Key
{
  public:
    void U32(std::uint32_t value)
    {
        std::array<std::byte,4> bytes;
        for(std::size_t i=0;i<4;++i) bytes[i]=std::byte((value>>(i*8))&255);
        hash_.Add(bytes);
    }
    void U64(std::uint64_t value) { U32(std::uint32_t(value)); U32(std::uint32_t(value>>32)); }
    void Float(float value) { U32(std::bit_cast<std::uint32_t>(value)); }
    void Bytes(std::span<const std::byte> bytes) { U64(bytes.size()); hash_.Add(bytes); }
    void String(const char *text)
    {
        U32(text?1u:0u);
        if(text) Bytes(std::as_bytes(std::span(text,std::strlen(text))));
    }
    void Shader(D3D12_SHADER_BYTECODE shader)
    {
        if(shader.BytecodeLength && !shader.pShaderBytecode) throw KeyError{};
        Bytes({static_cast<const std::byte *>(shader.pShaderBytecode),shader.BytecodeLength});
    }
    std::array<std::byte,32> Finish() { return hash_.Finish(); }
  private:
    Sha256Stream hash_;
};
std::wstring Hex(std::span<const std::byte> bytes)
{
    constexpr wchar_t digits[]=L"0123456789abcdef";
    std::wstring result; result.reserve(bytes.size()*2);
    for(auto value:bytes)
    {
        const auto byte=std::to_integer<unsigned>(value);
        result.push_back(digits[byte>>4]); result.push_back(digits[byte&15]);
    }
    return result;
}
HRESULT RootDigest(ID3D12RootSignature *root,std::array<std::byte,32> &digest)
{
    if(!root) return E_INVALIDARG;
    UINT size=UINT(digest.size()); const auto native=root->GetPrivateData(kRootDigestGuid,&size,digest.data());
    if(native==DXGI_ERROR_NOT_FOUND || native==DXGI_ERROR_MORE_DATA || (SUCCEEDED(native) && size!=digest.size())) return E_INVALIDARG;
    return native;
}
void Blend(Key &key,const D3D12_BLEND_DESC &blend)
{
    key.U32(blend.AlphaToCoverageEnable); key.U32(blend.IndependentBlendEnable);
    for(const auto &target:blend.RenderTarget)
    {
        key.U32(target.BlendEnable); key.U32(target.LogicOpEnable);
        key.U32(target.SrcBlend); key.U32(target.DestBlend); key.U32(target.BlendOp);
        key.U32(target.SrcBlendAlpha); key.U32(target.DestBlendAlpha); key.U32(target.BlendOpAlpha);
        key.U32(target.LogicOp); key.U32(target.RenderTargetWriteMask);
    }
}
void Raster(Key &key,const D3D12_RASTERIZER_DESC &raster)
{
    key.U32(raster.FillMode); key.U32(raster.CullMode); key.U32(raster.FrontCounterClockwise);
    key.U32(std::bit_cast<std::uint32_t>(raster.DepthBias)); key.Float(raster.DepthBiasClamp); key.Float(raster.SlopeScaledDepthBias);
    key.U32(raster.DepthClipEnable); key.U32(raster.MultisampleEnable); key.U32(raster.AntialiasedLineEnable);
    key.U32(raster.ForcedSampleCount); key.U32(raster.ConservativeRaster);
}
void Stencil(Key &key,const D3D12_DEPTH_STENCILOP_DESC &face)
{
    key.U32(face.StencilFailOp); key.U32(face.StencilDepthFailOp); key.U32(face.StencilPassOp); key.U32(face.StencilFunc);
}
void Depth(Key &key,const D3D12_DEPTH_STENCIL_DESC &depth)
{
    key.U32(depth.DepthEnable); key.U32(depth.DepthWriteMask); key.U32(depth.DepthFunc);
    key.U32(depth.StencilEnable); key.U32(depth.StencilReadMask); key.U32(depth.StencilWriteMask);
    Stencil(key,depth.FrontFace); Stencil(key,depth.BackFace);
}
void OutputState(Key &key,const D3D12_GRAPHICS_PIPELINE_STATE_DESC &description)
{
    Blend(key,description.BlendState); key.U32(description.SampleMask);
    Raster(key,description.RasterizerState); Depth(key,description.DepthStencilState);
    key.U32(description.PrimitiveTopologyType);
    if(description.NumRenderTargets>8) throw KeyError{};
    key.U32(description.NumRenderTargets);
    for(const auto format:description.RTVFormats) key.U32(format);
    key.U32(description.DSVFormat); key.U32(description.SampleDesc.Count); key.U32(description.SampleDesc.Quality);
    key.U32(description.NodeMask); key.U32(description.Flags);
}
void Inputs(Key &key,const D3D12_INPUT_LAYOUT_DESC &layout)
{
    if(layout.NumElements && !layout.pInputElementDescs) throw KeyError{};
    key.U32(layout.NumElements);
    for(UINT i=0;i<layout.NumElements;++i)
    {
        const auto &entry=layout.pInputElementDescs[i];
        if(!entry.SemanticName) throw KeyError{};
        key.String(entry.SemanticName); key.U32(entry.SemanticIndex); key.U32(entry.Format);
        key.U32(entry.InputSlot); key.U32(entry.AlignedByteOffset); key.U32(entry.InputSlotClass); key.U32(entry.InstanceDataStepRate);
    }
}
void StreamOutput(Key &key,const D3D12_STREAM_OUTPUT_DESC &stream)
{
    if((stream.NumEntries && !stream.pSODeclaration) || (stream.NumStrides && !stream.pBufferStrides)) throw KeyError{};
    key.U32(stream.NumEntries);
    for(UINT i=0;i<stream.NumEntries;++i)
    {
        const auto &entry=stream.pSODeclaration[i];
        key.U32(entry.Stream); key.String(entry.SemanticName); key.U32(entry.SemanticIndex);
        key.U32(entry.StartComponent); key.U32(entry.ComponentCount); key.U32(entry.OutputSlot);
    }
    key.U32(stream.NumStrides);
    for(UINT i=0;i<stream.NumStrides;++i) key.U32(stream.pBufferStrides[i]);
    key.U32(stream.RasterizedStream);
}
std::wstring GraphicsName(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &description,
    std::span<const std::byte> root,bool mesh,D3D12_SHADER_BYTECODE mesh_shader={})
{
    Key key; key.U32(kSchema); key.U32(mesh?3:1); key.Bytes(root);
    if(mesh) { key.Shader(mesh_shader); key.Shader(description.PS); }
    else
    {
        key.Shader(description.VS); key.Shader(description.PS); key.Shader(description.DS);
        key.Shader(description.HS); key.Shader(description.GS); StreamOutput(key,description.StreamOutput);
        Inputs(key,description.InputLayout); key.U32(description.IBStripCutValue);
    }
    OutputState(key,description);
    // CachedPSO is a native compilation hint, not a pipeline semantic. It is
    // excluded from identity; the unchanged native creation descriptor honors it.
    return Hex(key.Finish());
}
std::wstring ComputeName(const D3D12_COMPUTE_PIPELINE_STATE_DESC &description,std::span<const std::byte> root)
{
    Key key; key.U32(kSchema); key.U32(2); key.Bytes(root); key.Shader(description.CS);
    key.U32(description.NodeMask); key.U32(description.Flags); return Hex(key.Finish());
}
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type,class Value>
struct alignas(void *) MeshSubobject { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{Type}; Value value{}; };
struct MeshStream
{
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,ID3D12RootSignature *> root;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS,D3D12_SHADER_BYTECODE> mesh;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,D3D12_SHADER_BYTECODE> pixel;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,D3D12_BLEND_DESC> blend;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,UINT> mask;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,D3D12_RASTERIZER_DESC> raster;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,D3D12_DEPTH_STENCIL_DESC> depth;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,D3D12_RT_FORMAT_ARRAY> targets;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT,DXGI_FORMAT> depth_format;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,DXGI_SAMPLE_DESC> samples;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK,UINT> node;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS,D3D12_PIPELINE_STATE_FLAGS> flags;
    MeshStream(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &graphics,D3D12_SHADER_BYTECODE mesh_shader)
    {
        root.value=graphics.pRootSignature; mesh.value=mesh_shader; pixel.value=graphics.PS;
        blend.value=graphics.BlendState; mask.value=graphics.SampleMask; raster.value=graphics.RasterizerState;
        depth.value=graphics.DepthStencilState; topology.value=graphics.PrimitiveTopologyType;
        targets.value.NumRenderTargets=graphics.NumRenderTargets;
        std::copy(std::begin(graphics.RTVFormats),std::end(graphics.RTVFormats),std::begin(targets.value.RTFormats));
        depth_format.value=graphics.DSVFormat; samples.value=graphics.SampleDesc; node.value=graphics.NodeMask; flags.value=graphics.Flags;
    }
};
struct File
{
    HANDLE handle{INVALID_HANDLE_VALUE};
    explicit File(HANDLE native):handle(native) {}
    ~File() { if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); }
    File(const File &)=delete;
    File &operator=(const File &)=delete;
};
void Put(std::span<std::byte> bytes,std::size_t offset,std::uint64_t value,std::size_t count)
{
    for(std::size_t i=0;i<count;++i) bytes[offset+i]=std::byte((value>>(i*8))&255);
}
std::uint64_t Get(std::span<const std::byte> bytes,std::size_t offset,std::size_t count)
{
    std::uint64_t result=0;
    for(std::size_t i=0;i<count;++i) result|=std::uint64_t(std::to_integer<unsigned>(bytes[offset+i]))<<(i*8);
    return result;
}
bool Unsupported(HRESULT native) { return native==DXGI_ERROR_UNSUPPORTED || native==E_NOINTERFACE || native==E_NOTIMPL; }
} // namespace

struct RendererPipelineCache::Impl
{
    mutable std::mutex mutex;
    std::atomic_bool timing_enabled{};
    ComPtr<ID3D12Device> device;
    // Declaration/destruction order preserves the borrowed input blob until both
    // library references release. Flush serialization never modifies this vector.
    std::vector<std::byte> loaded_blob;
    ComPtr<ID3D12PipelineLibrary> library;
    ComPtr<ID3D12PipelineLibrary1> library1;
    std::filesystem::path file;
    std::array<std::byte,32> identity{};
    RendererPipelineCacheStats stats;
    bool dirty{};
    void Clear(bool clear_stats) noexcept
    {
        library1.Reset(); library.Reset(); loaded_blob.clear(); device.Reset(); file.clear();
        identity={}; dirty=false; timing_enabled.store(false, std::memory_order_relaxed);
        if(clear_stats) stats={};
        else
        {
            stats.initialized=false; stats.library_supported=false;
            stats.mesh_library_supported=false; stats.persistence_enabled=false;
        }
    }
    void IoFailure(DWORD error) { ++stats.io_failures; stats.last_io_error=std::int32_t(error); }
    void Discard()
    {
        ++stats.discarded_files; loaded_blob.clear();
        if(!DeleteFileW(file.c_str()))
        {
            const auto error=GetLastError();
            if(error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND) IoFailure(error);
        }
    }
    void ReadFile()
    {
        File source(CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(source.handle==INVALID_HANDLE_VALUE)
        {
            const auto error=GetLastError();
            if(error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND) IoFailure(error);
            return;
        }
        LARGE_INTEGER size;
        if(!GetFileSizeEx(source.handle,&size)) { IoFailure(GetLastError()); return; }
        if(size.QuadPart<std::int64_t(kHeaderBytes) || std::uint64_t(size.QuadPart)>kMaximumFileBytes) { Discard(); return; }
        std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart)); DWORD read=0;
        if(!::ReadFile(source.handle,bytes.data(),DWORD(bytes.size()),&read,nullptr)) { IoFailure(GetLastError()); return; }
        if(read!=bytes.size()) { Discard(); return; }
        const auto blob=std::span(bytes).subspan(kHeaderBytes);
        bool valid=std::equal(kMagic.begin(),kMagic.end(),bytes.begin()) && Get(bytes,8,4)==kSchema &&
            Get(bytes,12,4)==kHeaderBytes && Get(bytes,48,8)==blob.size() && !blob.empty() &&
            std::equal(identity.begin(),identity.end(),bytes.begin()+16);
        for(std::size_t i=88;i<kHeaderBytes;++i) valid=valid && bytes[i]==std::byte{};
        if(valid)
        {
            const auto digest=Sha256(blob); valid=std::equal(digest.begin(),digest.end(),bytes.begin()+56);
        }
        if(!valid) { Discard(); return; }
        loaded_blob.assign(blob.begin(),blob.end()); stats.loaded_bytes=loaded_blob.size();
    }
    void Store(const std::wstring &name,IUnknown *created) noexcept
    {
        CacheDuration timing(stats.capture_timings, stats.library_store_ns, &stats.library_store_max_ns);
        ComPtr<ID3D12PipelineState> pipeline;
        auto native=created->QueryInterface(IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
        if(SUCCEEDED(native)) native=library->StorePipeline(name.c_str(),pipeline.Get());
        if(FAILED(native)) { ++stats.store_failures; stats.last_cache_hresult=std::int32_t(native); }
        else { ++stats.stores; dirty=true; }
    }
};
RendererPipelineCache::RendererPipelineCache():impl_(std::make_unique<Impl>()) {}
RendererPipelineCache::~RendererPipelineCache()=default;
void RendererPipelineCache::Reset() noexcept { std::lock_guard lock(impl_->mutex); impl_->Clear(false); }
RendererPipelineCacheStats RendererPipelineCache::Stats() const { std::lock_guard lock(impl_->mutex); return impl_->stats; }

Result RendererPipelineCache::Initialize(ID3D12Device *device,IDXGIAdapter *adapter,std::filesystem::path directory,bool capture_timings)
{
    std::lock_guard lock(impl_->mutex); impl_->Clear(true);
    impl_->stats.capture_timings=capture_timings;
    impl_->timing_enabled.store(capture_timings, std::memory_order_relaxed);
    if(!device || !adapter) return Failure(ErrorCode::InvalidArgument,"Pipeline cache requires a device and adapter.");
    impl_->device=device;
    try
    {
        D3D12_FEATURE_DATA_SHADER_CACHE support{};
        auto native=device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE,&support,sizeof(support));
        if(native==E_INVALIDARG || (SUCCEEDED(native) && !(support.SupportFlags&D3D12_SHADER_CACHE_SUPPORT_LIBRARY)))
        {
            ++impl_->stats.unsupported_libraries; impl_->stats.initialized=true;
            impl_->stats.last_cache_hresult=std::int32_t(native); return Result::Success();
        }
        if(FAILED(native)) return NativeFailure("Query pipeline library support",native);
        ComPtr<ID3D12Device1> device1; native=device->QueryInterface(IID_PPV_ARGS(device1.ReleaseAndGetAddressOf()));
        if(Unsupported(native))
        {
            ++impl_->stats.unsupported_libraries; impl_->stats.initialized=true;
            impl_->stats.last_cache_hresult=std::int32_t(native); return Result::Success();
        }
        if(FAILED(native)) return NativeFailure("Query Device1 for pipeline library",native);
        if(!directory.empty())
        {
            DXGI_ADAPTER_DESC description{}; native=adapter->GetDesc(&description);
            if(FAILED(native)) return NativeFailure("Query cache adapter identity",native);
            LARGE_INTEGER driver{}; native=adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver);
            if(Unsupported(native))
            {
                // Driver identity is required for persistence. Memory caching
                // remains valid; a guessed zero driver is never a persistent key.
                impl_->IoFailure(ERROR_NOT_SUPPORTED); impl_->stats.last_cache_hresult=std::int32_t(native);
            }
            else if(FAILED(native)) return NativeFailure("Query cache driver identity",native);
            else
            {
                Key identity; identity.U32(kSchema); identity.U32(description.VendorId); identity.U32(description.DeviceId);
                identity.U32(description.SubSysId); identity.U32(description.Revision);
                // A LUID is only locally stable until reboot. Use the hardware
                // identity and driver here; the native blob validates the exact
                // adapter compatibility before any cached PSO can be loaded.
                identity.U64(std::bit_cast<std::uint64_t>(driver.QuadPart)); impl_->identity=identity.Finish();
                impl_->file=directory/(L"renderer-pso-v1-"+Hex(impl_->identity)+L".bin");
                impl_->stats.persistence_enabled=true; impl_->ReadFile();
            }
        }
        native=device1->CreatePipelineLibrary(impl_->loaded_blob.empty()?nullptr:impl_->loaded_blob.data(),
            impl_->loaded_blob.size(),IID_PPV_ARGS(impl_->library.ReleaseAndGetAddressOf()));
        if(!impl_->loaded_blob.empty() && (native==E_INVALIDARG || native==D3D12_ERROR_DRIVER_VERSION_MISMATCH || native==D3D12_ERROR_ADAPTER_NOT_FOUND))
        {
            impl_->stats.last_cache_hresult=std::int32_t(native);
            impl_->library.Reset(); impl_->Discard(); impl_->stats.loaded_bytes=0;
            native=device1->CreatePipelineLibrary(nullptr,0,IID_PPV_ARGS(impl_->library.ReleaseAndGetAddressOf()));
        }
        if(Unsupported(native))
        {
            impl_->library.Reset(); impl_->loaded_blob.clear(); ++impl_->stats.unsupported_libraries;
            impl_->stats.last_cache_hresult=std::int32_t(native); impl_->stats.persistence_enabled=false;
            impl_->stats.initialized=true; return Result::Success();
        }
        if(FAILED(native)) return NativeFailure("Create pipeline library",native);
        native=impl_->library.As(&impl_->library1);
        if(FAILED(native) && native!=E_NOINTERFACE) return NativeFailure("Query pipeline Library1",native);
        impl_->stats.library_supported=true; impl_->stats.mesh_library_supported=impl_->library1!=nullptr;
        impl_->stats.initialized=true; return Result::Success();
    }
    catch(const std::bad_alloc &) { return NativeFailure("Allocate pipeline cache",E_OUTOFMEMORY); }
    catch(const std::exception &error) { return Failure(ErrorCode::TaskFailed,error.what()); }
}
Result RendererPipelineCache::RegisterRootSignature(ID3D12RootSignature *root,std::span<const std::byte> serialized)
{
    std::lock_guard lock(impl_->mutex);
    if(!impl_->stats.initialized) return Failure(ErrorCode::InvalidState,"Initialize pipeline cache before registering roots.");
    if(!root || serialized.empty()) return Failure(ErrorCode::InvalidArgument,"Root registration requires native root and exact serialized bytes.");
    try
    {
        const auto digest=Sha256(serialized); const auto native=root->SetPrivateData(kRootDigestGuid,UINT(digest.size()),digest.data());
        if(FAILED(native)) { impl_->stats.last_cache_hresult=std::int32_t(native); return NativeFailure("Register root digest",native); }
        ++impl_->stats.root_registrations; return Result::Success();
    }
    catch(const std::bad_alloc &) { return NativeFailure("Allocate root digest",E_OUTOFMEMORY); }
    catch(const std::exception &error) { return Failure(ErrorCode::TaskFailed,error.what()); }
}
HRESULT RendererPipelineCache::CreateGraphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *description,REFIID iid,void **output)
{
    const auto waiting = CacheStart(impl_->timing_enabled.load(std::memory_order_relaxed));
    std::lock_guard lock(impl_->mutex); if(output) *output=nullptr;
    RecordCacheDuration(waiting, impl_->stats.mutex_wait_ns, &impl_->stats.mutex_wait_max_ns);
    if(!impl_->stats.initialized) return E_UNEXPECTED;
    if(!description) return E_INVALIDARG;
    std::array<std::byte,32> root; auto native=RootDigest(description->pRootSignature,root);
    if(FAILED(native)) { ++impl_->stats.key_failures; impl_->stats.last_cache_hresult=std::int32_t(native); return native; }
    try
    {
        const auto key_start=CacheStart(impl_->stats.capture_timings);
        const auto name=impl_->library && output?GraphicsName(*description,root,false):std::wstring{};
        RecordCacheDuration(key_start, impl_->stats.key_build_ns, &impl_->stats.key_build_max_ns);
        if(impl_->library && output)
        {
            {
                CacheDuration timing(impl_->stats.capture_timings, impl_->stats.library_load_ns, &impl_->stats.library_load_max_ns);
                native=impl_->library->LoadGraphicsPipeline(name.c_str(),description,iid,output);
            }
            if(SUCCEEDED(native)) { ++impl_->stats.hits; return native; }
            if(native!=E_INVALIDARG) { ++impl_->stats.load_failures; impl_->stats.last_cache_hresult=std::int32_t(native); return native; }
            ++impl_->stats.misses;
        }
        ++impl_->stats.native_creates;
        {
            CacheDuration timing(impl_->stats.capture_timings, impl_->stats.native_create_ns, &impl_->stats.native_create_max_ns);
            native=impl_->device->CreateGraphicsPipelineState(description,iid,output);
        }
        if(SUCCEEDED(native) && output && *output && impl_->library) impl_->Store(name,static_cast<IUnknown *>(*output));
        return native;
    }
    catch(const KeyError &) { ++impl_->stats.key_failures; impl_->stats.last_cache_hresult=std::int32_t(E_INVALIDARG); return E_INVALIDARG; }
    catch(const std::bad_alloc &) { return E_OUTOFMEMORY; }
    catch(...) { return E_FAIL; }
}
HRESULT RendererPipelineCache::CreateCompute(const D3D12_COMPUTE_PIPELINE_STATE_DESC *description,REFIID iid,void **output)
{
    const auto waiting = CacheStart(impl_->timing_enabled.load(std::memory_order_relaxed));
    std::lock_guard lock(impl_->mutex); if(output) *output=nullptr;
    RecordCacheDuration(waiting, impl_->stats.mutex_wait_ns, &impl_->stats.mutex_wait_max_ns);
    if(!impl_->stats.initialized) return E_UNEXPECTED;
    if(!description) return E_INVALIDARG;
    std::array<std::byte,32> root; auto native=RootDigest(description->pRootSignature,root);
    if(FAILED(native)) { ++impl_->stats.key_failures; impl_->stats.last_cache_hresult=std::int32_t(native); return native; }
    try
    {
        const auto key_start=CacheStart(impl_->stats.capture_timings);
        const auto name=impl_->library && output?ComputeName(*description,root):std::wstring{};
        RecordCacheDuration(key_start, impl_->stats.key_build_ns, &impl_->stats.key_build_max_ns);
        if(impl_->library && output)
        {
            {
                CacheDuration timing(impl_->stats.capture_timings, impl_->stats.library_load_ns, &impl_->stats.library_load_max_ns);
                native=impl_->library->LoadComputePipeline(name.c_str(),description,iid,output);
            }
            if(SUCCEEDED(native)) { ++impl_->stats.hits; return native; }
            if(native!=E_INVALIDARG) { ++impl_->stats.load_failures; impl_->stats.last_cache_hresult=std::int32_t(native); return native; }
            ++impl_->stats.misses;
        }
        ++impl_->stats.native_creates;
        {
            CacheDuration timing(impl_->stats.capture_timings, impl_->stats.native_create_ns, &impl_->stats.native_create_max_ns);
            native=impl_->device->CreateComputePipelineState(description,iid,output);
        }
        if(SUCCEEDED(native) && output && *output && impl_->library) impl_->Store(name,static_cast<IUnknown *>(*output));
        return native;
    }
    catch(const KeyError &) { ++impl_->stats.key_failures; impl_->stats.last_cache_hresult=std::int32_t(E_INVALIDARG); return E_INVALIDARG; }
    catch(const std::bad_alloc &) { return E_OUTOFMEMORY; }
    catch(...) { return E_FAIL; }
}
HRESULT RendererPipelineCache::CreateMesh(ID3D12Device2 *device,const D3D12_GRAPHICS_PIPELINE_STATE_DESC &graphics,
    D3D12_SHADER_BYTECODE mesh_shader,ID3D12PipelineState **output)
{
    const auto waiting = CacheStart(impl_->timing_enabled.load(std::memory_order_relaxed));
    std::lock_guard lock(impl_->mutex); if(output) *output=nullptr;
    RecordCacheDuration(waiting, impl_->stats.mutex_wait_ns, &impl_->stats.mutex_wait_max_ns);
    if(!impl_->stats.initialized) return E_UNEXPECTED;
    if(!device) return E_INVALIDARG;
    // The explicit Device2 argument must be the device owning this library.
    // Different interface pointers of the same COM object are allowed.
    ComPtr<IUnknown> supplied_identity,cache_identity;
    auto native=device->QueryInterface(IID_PPV_ARGS(supplied_identity.ReleaseAndGetAddressOf()));
    if(FAILED(native)) return native;
    native=impl_->device->QueryInterface(IID_PPV_ARGS(cache_identity.ReleaseAndGetAddressOf()));
    if(FAILED(native)) return native;
    if(supplied_identity.Get()!=cache_identity.Get()) return E_INVALIDARG;
    std::array<std::byte,32> root; native=RootDigest(graphics.pRootSignature,root);
    if(FAILED(native)) { ++impl_->stats.key_failures; impl_->stats.last_cache_hresult=std::int32_t(native); return native; }
    try
    {
        const auto key_start=CacheStart(impl_->stats.capture_timings);
        const auto name=impl_->library1 && output?GraphicsName(graphics,root,true,mesh_shader):std::wstring{};
        RecordCacheDuration(key_start, impl_->stats.key_build_ns, &impl_->stats.key_build_max_ns);
        MeshStream stream(graphics,mesh_shader);
        const D3D12_PIPELINE_STATE_STREAM_DESC description{sizeof(stream),&stream};
        if(impl_->library1 && output)
        {
            {
                CacheDuration timing(impl_->stats.capture_timings, impl_->stats.library_load_ns, &impl_->stats.library_load_max_ns);
                native=impl_->library1->LoadPipeline(name.c_str(),&description,__uuidof(ID3D12PipelineState),reinterpret_cast<void **>(output));
            }
            if(SUCCEEDED(native)) { ++impl_->stats.hits; return native; }
            if(native!=E_INVALIDARG) { ++impl_->stats.load_failures; impl_->stats.last_cache_hresult=std::int32_t(native); return native; }
            ++impl_->stats.misses;
        }
        else ++impl_->stats.uncached_mesh_creates;
        ++impl_->stats.native_creates;
        {
            CacheDuration timing(impl_->stats.capture_timings, impl_->stats.native_create_ns, &impl_->stats.native_create_max_ns);
            native=device->CreatePipelineState(&description,__uuidof(ID3D12PipelineState),reinterpret_cast<void **>(output));
        }
        if(SUCCEEDED(native) && output && *output && impl_->library1) impl_->Store(name,*output);
        return native;
    }
    catch(const KeyError &) { ++impl_->stats.key_failures; impl_->stats.last_cache_hresult=std::int32_t(E_INVALIDARG); return E_INVALIDARG; }
    catch(const std::bad_alloc &) { return E_OUTOFMEMORY; }
    catch(...) { return E_FAIL; }
}
Result RendererPipelineCache::Flush()
{
    const auto waiting = CacheStart(impl_->timing_enabled.load(std::memory_order_relaxed));
    std::lock_guard lock(impl_->mutex);
    RecordCacheDuration(waiting, impl_->stats.mutex_wait_ns, &impl_->stats.mutex_wait_max_ns);
    if(!impl_->stats.initialized) return Failure(ErrorCode::InvalidState,"Initialize pipeline cache before flushing.");
    if(!impl_->library || !impl_->stats.persistence_enabled || !impl_->dirty) return Result::Success();
    try
    {
        const auto size=impl_->library->GetSerializedSize();
        if(!size || size>kMaximumFileBytes-kHeaderBytes)
        {
            impl_->IoFailure(ERROR_FILE_TOO_LARGE); return Failure(ErrorCode::TaskFailed,"Pipeline library exceeds the 256 MiB cache-file bound.");
        }
        std::vector<std::byte> bytes(kHeaderBytes+size);
        HRESULT native;
        {
            CacheDuration timing(impl_->stats.capture_timings, impl_->stats.serialize_ns);
            native=impl_->library->Serialize(bytes.data()+kHeaderBytes,size);
        }
        if(FAILED(native)) { impl_->stats.last_cache_hresult=std::int32_t(native); return NativeFailure("Serialize pipeline library",native); }
        impl_->stats.serialized_bytes=size;
        std::copy(kMagic.begin(),kMagic.end(),bytes.begin()); Put(bytes,8,kSchema,4); Put(bytes,12,kHeaderBytes,4);
        std::copy(impl_->identity.begin(),impl_->identity.end(),bytes.begin()+16); Put(bytes,48,size,8);
        const auto digest=Sha256(std::span(bytes).subspan(kHeaderBytes)); std::copy(digest.begin(),digest.end(),bytes.begin()+56);
        CacheDuration write_timing(impl_->stats.capture_timings, impl_->stats.cache_write_ns);
        std::error_code error; std::filesystem::create_directories(impl_->file.parent_path(),error);
        if(error) { impl_->IoFailure(DWORD(error.value())); return Failure(ErrorCode::TaskFailed,"Create pipeline cache directory: "+error.message()); }
        static std::atomic<std::uint64_t> sequence{};
        auto temporary=impl_->file; temporary+=L".tmp-"+std::to_wstring(GetCurrentProcessId())+L"-"+
            std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(sequence.fetch_add(1,std::memory_order_relaxed));
        DWORD io_error=0; bool owns_temporary=false;
        {
            File target(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
            if(target.handle==INVALID_HANDLE_VALUE) io_error=GetLastError();
            else
            {
                owns_temporary=true;
                DWORD written=0;
                if(!WriteFile(target.handle,bytes.data(),DWORD(bytes.size()),&written,nullptr)) io_error=GetLastError();
                else if(written!=bytes.size()) io_error=ERROR_WRITE_FAULT;
                else if(!FlushFileBuffers(target.handle)) io_error=GetLastError();
            }
        }
        if(!io_error && !MoveFileExW(temporary.c_str(),impl_->file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) io_error=GetLastError();
        if(io_error)
        {
            // Remove only the exact temporary file owned by this Flush call.
            if(owns_temporary && !DeleteFileW(temporary.c_str()))
            {
                const auto cleanup=GetLastError();
                if(cleanup!=ERROR_FILE_NOT_FOUND && cleanup!=ERROR_PATH_NOT_FOUND) impl_->IoFailure(cleanup);
            }
            impl_->IoFailure(io_error); return NativeFailure("Persist pipeline cache",HRESULT_FROM_WIN32(io_error));
        }
        impl_->dirty=false; return Result::Success();
    }
    catch(const std::bad_alloc &) { return NativeFailure("Allocate serialized pipeline library",E_OUTOFMEMORY); }
    catch(const std::exception &error) { return Failure(ErrorCode::TaskFailed,error.what()); }
}
} // namespace hs
