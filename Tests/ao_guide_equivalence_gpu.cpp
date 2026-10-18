// Standalone native comparison of the actual original and candidate AO entrypoints.
// Usage: ao_guide_equivalence_gpu.exe <directory containing before/after DXIL> [warp]
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <array>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <cstring>
#include <cstdint>
using Microsoft::WRL::ComPtr;
using namespace DirectX;
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion=619;
__declspec(dllexport) extern const char* D3D12SDKPath=".\\D3D12\\"; }
static void Check(bool ok,const char* text) { if(!ok)throw std::runtime_error(text); }
static void Hr(HRESULT hr,const char* text) { if(FAILED(hr))throw std::runtime_error(std::string(text)+": "+std::to_string(hr)); }
struct Constants {
 XMFLOAT4X4 inverse,world,projection;
 XMFLOAT4 render,size,history,previous,jitter,parameters,filter,contact,light;
 XMUINT4 samples,frame;
};
static_assert(sizeof(Constants)==368);
struct Event { HANDLE h=CreateEventW(nullptr,FALSE,FALSE,nullptr); ~Event(){if(h)CloseHandle(h);} };
struct Gpu {
 ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
 ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
 ComPtr<ID3D12Fence> fence; ComPtr<ID3D12RootSignature> root;
 ComPtr<ID3D12DescriptorHeap> descriptors; std::array<ComPtr<ID3D12PipelineState>,4> psos;
 UINT stride{}; UINT64 serial{}; Event event;
 Gpu(const std::filesystem::path& path,bool warp) {
  ComPtr<ID3D12Debug> debug;Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"debug");debug->EnableDebugLayer();
  ComPtr<ID3D12Debug1> gbv;Hr(debug.As(&gbv),"GBV");gbv->SetEnableGPUBasedValidation(TRUE);
  ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
  ComPtr<IDXGIAdapter1> adapter;
  if(warp)Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"warp");
  else for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> next;if(factory->EnumAdapters1(i,&next)==DXGI_ERROR_NOT_FOUND)break;
   DXGI_ADAPTER_DESC1 d{};Hr(next->GetDesc1(&d),"adapter desc");if(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
   if(SUCCEEDED(D3D12CreateDevice(next.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))){adapter=next;break;}}
  Check(adapter!=nullptr,"requested adapter unavailable");
  if(!device)Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
  DXGI_ADAPTER_DESC1 d{};Hr(adapter->GetDesc1(&d),"adapter desc");std::wcout<<L"AO adapter: "<<d.Description<<L"; GBV enabled\n";
  D3D12_COMMAND_QUEUE_DESC q{};Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
  Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
  Hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");Hr(list->Close(),"close");
  Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");Check(event.h!=nullptr,"event");
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,32,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
  Hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&descriptors)),"heap");stride=device->GetDescriptorHandleIncrementSize(hd.Type);
  std::array<D3D12_DESCRIPTOR_RANGE,3> ranges{{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,9,0,12,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,6,33,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,12,0}}};
  std::array<D3D12_ROOT_PARAMETER,4> rp{};rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;rp[0].Descriptor={0,12};
  for(UINT i=0;i<3;++i){rp[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[i+1].DescriptorTable={1,&ranges[i]};}
  D3D12_ROOT_SIGNATURE_DESC rd{4,rp.data(),0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
  Hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"serialize root");
  Hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
  for(UINT i=0;i<4;++i){auto file=path/(i%2?"after":"before")/(i<2?"AdaptiveGtaoHorizonCS.dxil":"AdaptiveGtaoResolveCS.dxil");
   std::ifstream input(file,std::ios::binary);Check(input.good(),"missing actual entrypoint DXIL");std::vector<char> bytes{std::istreambuf_iterator<char>(input),{}};
   D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={bytes.data(),bytes.size()};
   Hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&psos[i])),"actual AO PSO");}
 }
 D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT i){auto h=descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*stride;return h;}
 D3D12_GPU_DESCRIPTOR_HANDLE Handle(UINT i){auto h=descriptors->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(i)*stride;return h;}
 ComPtr<ID3D12Resource> Resource(D3D12_RESOURCE_DESC d,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state){
  D3D12_HEAP_PROPERTIES hp{};hp.Type=type;hp.CreationNodeMask=hp.VisibleNodeMask=1;ComPtr<ID3D12Resource> r;
  Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"resource");return r;}
 ComPtr<ID3D12Resource> Buffer(UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state){D3D12_RESOURCE_DESC d{};
  d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return Resource(d,heap,state);}
 void Transition(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&barrier);}
 void Srv(UINT index,ID3D12Resource* r,DXGI_FORMAT format){D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
  d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;device->CreateShaderResourceView(r,&d,Cpu(index));}
 void Submit(){Hr(list->Close(),"close");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
  Hr(queue->Signal(fence.Get(),++serial),"signal");Hr(fence->SetEventOnCompletion(serial,event.h),"completion");Check(WaitForSingleObject(event.h,60000)==WAIT_OBJECT_0,"GPU timeout");
  Hr(device->GetDeviceRemovedReason(),"device removed");ComPtr<ID3D12InfoQueue> info;Hr(device.As(&info),"info queue");
  for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size{};Hr(info->GetMessage(i,nullptr,&size),"message size");std::vector<char> bytes(size);
   auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());Hr(info->GetMessage(i,m,&size),"message");if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)throw std::runtime_error(m->pDescription);}info->ClearStoredMessages();}
};
int main(int argc,char** argv)try{
 Check(argc>=2,"usage: ao_guide_equivalence_gpu <prototype root> [warp]");Gpu gpu(argv[1],argc>2&&std::string(argv[2])=="warp");
 UINT cases=0;UINT64 texels=0;bool saw_nonwhite=false,saw_fallback=false;UINT64 bit_differences=0;float max_difference=0;
 const std::array<const char*,12> names{"flat-density2","density0","density1","mixed-density","sky","invalid-material","material-edge","depth-below","depth-above","normal-below","normal-above","incompatible-raw"};
 for(auto extent:std::array<std::array<UINT,2>,4>{{{1,1},{7,9},{9,7},{17,17}}})for(UINT half=0;half<2;++half)for(UINT jitter=0;jitter<2;++jitter)for(UINT pattern=0;pattern<names.size();++pattern){
  const UINT w=extent[0],h=extent[1],rw=w*(half+1),rh=h*(half+1),dw=(rw+7)/8,dh=(rh+7)/8;
  Constants c{};const auto projection=XMMatrixPerspectiveFovLH(1.05f,float(rw)/rh,100.0f,0.1f);
  XMStoreFloat4x4(&c.projection,projection);XMStoreFloat4x4(&c.inverse,XMMatrixInverse(nullptr,projection));XMStoreFloat4x4(&c.world,XMMatrixIdentity());
  c.render={float(rw),float(rh),1.0f/rw,1.0f/rh};c.size={float(w),float(h),1.0f/w,1.0f/h};c.history=c.size;
  c.jitter={jitter?0.37f/rw:0,jitter?-0.29f/rh:0,0,0};c.parameters={2.0f,1.0f,0.8f,0};c.samples={4,4,0,0};c.frame={0,17,1,0};
  std::vector<float> depth(rw*rh);std::vector<std::array<int16_t,2>> normal(rw*rh,{32767,32767});
  std::vector<std::array<UINT,2>> metadata(rw*rh,{1,3});std::vector<uint8_t> density(dw*dh,2);
  for(UINT y=0;y<rh;++y)for(UINT x=0;x<rw;++x){const UINT i=y*rw+x;float z=2.0f;
   if(pattern==7&&x%2)z=2.0098f;if(pattern==8&&x%2)z=2.0102f;
   if(pattern==3)z=1.9f+0.02f*float((x*3+y)%7);
   depth[i]=XMVectorGetZ(XMVector3TransformCoord(XMVectorSet(0,0,z,1),projection));
   if(pattern==4&&(x+y)%3==0)depth[i]=0;if(pattern==5&&(x+y)%3==0)metadata[i][0]=0;
   if(pattern==6&&x%2)metadata[i][1]=4;
   if((pattern==9||pattern==10)&&x%2){const float a=pattern==9?0.099f:0.101f;const float nx=std::sin(a),nz=-std::cos(a);const float norm=std::abs(nx)+std::abs(nz);
    normal[i]={32767,int16_t(std::lround((1-nx/norm)*32767))};}}
  for(UINT i=0;i<density.size();++i)density[i]=uint8_t(pattern==1?0:pattern==2?1:pattern==3?i%3:2);
  std::vector<XMFLOAT4> bad_raw(w*h,XMFLOAT4{0.125f,0,0,0});
  Hr(gpu.allocator->Reset(),"reset allocator");Hr(gpu.list->Reset(gpu.allocator.Get(),nullptr),"reset list");
  std::vector<ComPtr<ID3D12Resource>> uploads;
  const auto texture=[&](UINT width,UINT height,DXGI_FORMAT format,UINT bytes,const void* data){
   D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;d.DepthOrArraySize=d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;
   d.Flags=data?D3D12_RESOURCE_FLAG_NONE:D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
   auto t=gpu.Resource(d,D3D12_HEAP_TYPE_DEFAULT,data?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   if(data){D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows{};UINT64 rowbytes{},size{};gpu.device->GetCopyableFootprints(&d,0,1,0,&fp,&rows,&rowbytes,&size);
    auto upload=gpu.Buffer(size,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* p{};D3D12_RANGE none{};Hr(upload->Map(0,&none,&p),"texture map");
    for(UINT y=0;y<height;++y)std::memcpy(static_cast<char*>(p)+fp.Offset+y*fp.Footprint.RowPitch,static_cast<const char*>(data)+size_t(y)*width*bytes,size_t(width)*bytes);upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
    D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=t.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;gpu.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    gpu.Transition(t.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);uploads.push_back(upload);}return t;};
  auto dt=texture(rw,rh,DXGI_FORMAT_R32_FLOAT,4,depth.data());auto nt=texture(rw,rh,DXGI_FORMAT_R16G16_SNORM,4,normal.data());
  auto mt=texture(rw,rh,DXGI_FORMAT_R32G32_UINT,8,metadata.data());auto den=texture(dw,dh,DXGI_FORMAT_R8_UINT,1,density.data());
  auto raw=texture(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,16,bad_raw.data());std::array<ComPtr<ID3D12Resource>,4> outputs;
  for(auto& out:outputs)out=texture(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,16,nullptr);
  auto cb=gpu.Buffer(512,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped{};D3D12_RANGE none{};Hr(cb->Map(0,&none,&mapped),"constants map");std::memcpy(mapped,&c,sizeof(c));cb->Unmap(0,nullptr);
  for(UINT bank=0;bank<2;++bank){const UINT base=bank*10;for(UINT i=0;i<9;++i)gpu.Srv(base+i,nullptr,i==2||i==7?DXGI_FORMAT_R32G32_UINT:DXGI_FORMAT_R32G32B32A32_FLOAT);
   gpu.Srv(base,dt.Get(),DXGI_FORMAT_R32_FLOAT);gpu.Srv(base+1,nt.Get(),DXGI_FORMAT_R16G16_SNORM);gpu.Srv(base+2,mt.Get(),DXGI_FORMAT_R32G32_UINT);
   gpu.Srv(base+5,bank==0||pattern==11?raw.Get():outputs[0].Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);}
  gpu.Srv(20,den.Get(),DXGI_FORMAT_R8_UINT);
  for(UINT i=0;i<4;++i){D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;gpu.device->CreateUnorderedAccessView(outputs[i].Get(),nullptr,&u,gpu.Cpu(21+i));}
  auto desc=outputs[0]->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};gpu.device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
  std::array<ComPtr<ID3D12Resource>,4> readbacks;for(auto& r:readbacks)r=gpu.Buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  gpu.list->SetComputeRootSignature(gpu.root.Get());ID3D12DescriptorHeap* heaps[]{gpu.descriptors.Get()};gpu.list->SetDescriptorHeaps(1,heaps);
  gpu.list->SetComputeRootConstantBufferView(0,cb->GetGPUVirtualAddress());gpu.list->SetComputeRootDescriptorTable(2,gpu.Handle(20));
  for(UINT i=0;i<4;++i){gpu.list->SetPipelineState(gpu.psos[i].Get());gpu.list->SetComputeRootDescriptorTable(1,gpu.Handle(i<2?0:10));gpu.list->SetComputeRootDescriptorTable(3,gpu.Handle(21+i));gpu.list->Dispatch((w+7)/8,(h+7)/8,1);
   gpu.Transition(outputs[i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
   D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=outputs[i].Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readbacks[i].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;gpu.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
   if(i==0)gpu.Transition(outputs[i].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
  gpu.Submit();std::array<void*,4> data{};D3D12_RANGE read{0,SIZE_T(bytes)};for(UINT i=0;i<4;++i)Hr(readbacks[i]->Map(0,&read,&data[i]),"readback map");
  for(UINT stage=0;stage<2;++stage)for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){const auto* a=reinterpret_cast<const float*>(static_cast<const char*>(data[stage*2])+y*fp.Footprint.RowPitch)+x*4;
   const auto* b=reinterpret_cast<const float*>(static_cast<const char*>(data[stage*2+1])+y*fp.Footprint.RowPitch)+x*4;
   if(stage==0&&a[1]>0&&a[0]<0.99f)saw_nonwhite=true;if(stage==1&&pattern==11&&(x%2||y%2)&&a[1]>0&&a[0]!=0.125f)saw_fallback=true;
   for(UINT k=0;k<4;++k){Check(std::isfinite(a[k])&&std::isfinite(b[k]),"nonfinite AO result");if(std::memcmp(a+k,b+k,4)!=0){++bit_differences;max_difference=std::max(max_difference,std::abs(a[k]-b[k]));
    std::cerr<<"Mismatch "<<names[pattern]<<" extent="<<w<<"x"<<h<<" half="<<half<<" jitter="<<jitter<<" stage="<<stage<<" xy="<<x<<","<<y<<" component="<<k<<" before="<<a[k]<<" after="<<b[k]<<"\n";}}}
  for(UINT i=0;i<4;++i)readbacks[i]->Unmap(0,&none);++cases;texels+=UINT64(w)*h*2;
 }
 std::cout<<"AO comparisons: "<<cases<<" cases, "<<texels<<" texel pairs, bit differences="<<bit_differences<<", max absolute difference="<<max_difference<<", nonwhite="<<saw_nonwhite<<", incompatible fallback="<<saw_fallback<<"\n";
 Check(saw_nonwhite&&saw_fallback,"AO fixtures failed to exercise kernel and fallback");Check(bit_differences==0,"AO output differs; inspect evidence before accepting");return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
