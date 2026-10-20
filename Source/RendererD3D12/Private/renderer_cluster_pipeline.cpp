#include "renderer_impl.hpp"

#include <memory>

namespace hs
{
Result D3D12Renderer::Impl::CreateClusterPipelines(const RendererConfig &pipeline_config)
{
    D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kClusterCutSrvCount, 0, 6,
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kClusterCutUavCount, 0, 6,
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, kClusterCutSrvCount};
    D3D12_ROOT_PARAMETER1 parameters[5]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor = {0, 6, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor = {28, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable = {2, ranges};
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[3].Descriptor = {1, 6, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    D3D12_DESCRIPTOR_RANGE1 hlod_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 25,
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0};
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[4].DescriptorTable = {1, &hlod_range};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC description{};
    description.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    description.Desc_1_1 = {5, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized, errors;
    auto result = D3D12SerializeVersionedRootSignature(&description, &serialized, &errors);
    if (FAILED(result)) return HResultFailure("Serialize cluster root signature", result);
    result = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&cluster_root_signature));
    if (FAILED(result)) return HResultFailure("Create cluster root signature", result);
    if (auto registered = pipeline_cache.RegisterRootSignature(cluster_root_signature.Get(),
            {static_cast<const std::byte *>(serialized->GetBufferPointer()), serialized->GetBufferSize()});
        !registered) return registered;
    for (auto [name, target] : {
        std::pair{"cluster_count_cs.dxil", std::addressof(cluster_count_pipeline)},
        std::pair{"cluster_prefix_cs.dxil", std::addressof(cluster_prefix_pipeline)},
        std::pair{"cluster_emit_cs.dxil", std::addressof(cluster_emit_pipeline)},
        std::pair{"cluster_occlusion_cs.dxil", std::addressof(cluster_occlusion_pipeline)},
        std::pair{"cluster_retest_cs.dxil", std::addressof(cluster_retest_pipeline)}})
    {
        std::filesystem::path path;
        if (auto resolved = pipeline_config.ResolveProduct("render/shader/" + std::string(name), path); !resolved)
            return resolved;
        std::vector<std::byte> bytes;
        if (auto read = ReadBinary(path, bytes); !read) return read;
        D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
        state.pRootSignature = cluster_root_signature.Get();
        state.CS = {bytes.data(), bytes.size()};
        result = pipeline_cache.CreateCompute(&state, IID_PPV_ARGS(target->ReleaseAndGetAddressOf()));
        if (FAILED(result)) return HResultFailure("Create cluster refinement pipeline", result);
    }
    D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
    arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    arguments[0].Constant = {6, 0, 1};
    arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signature{24, 2, arguments, 0};
    result = device->CreateCommandSignature(&signature, root_signature.Get(), IID_PPV_ARGS(&cluster_draw_signature));
    if (FAILED(result)) return HResultFailure("Create cluster vertex-pull signature", result);
    if (mesh_shader_enabled)
    {
        arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
        signature.ByteStride = 16;
        result = device->CreateCommandSignature(&signature, root_signature.Get(), IID_PPV_ARGS(&cluster_mesh_signature));
        if (FAILED(result)) return HResultFailure("Create cluster mesh signature", result);
    }
    return Result::Success();
}
} // namespace hs
