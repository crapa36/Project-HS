#include "renderer_impl.hpp"

#include <format>

namespace hs
{

Result HResultFailure(std::string_view operation, HRESULT result)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                           std::format("{} failed: 0x{:08X}", operation,
                                       static_cast<std::uint32_t>(result)));
}

Result D3D12Renderer::Impl::CheckDevice(HRESULT result, std::string_view operation)
{
    if (SUCCEEDED(result))
    {
        return Result::Success();
    }
    const auto reason = device ? device->GetDeviceRemovedReason() : result;
    if (reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET ||
        result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
    {
        WriteDred(reason);
    }
    return HResultFailure(operation, result);
}


void D3D12Renderer::Impl::CountValidationErrors()
{
    if (!config.validation || !device)
    {
        return;
    }
    ComPtr<ID3D12InfoQueue> info_queue;
    if (FAILED(device.As(&info_queue)))
    {
        return;
    }
    const auto count = info_queue->GetNumStoredMessages();
    std::ofstream log(config.artifact_directory / "d3d12_validation.log", std::ios::app);
    for (std::uint64_t index = 0; index < count; ++index)
    {
        SIZE_T size{};
        info_queue->GetMessage(index, nullptr, &size);
        std::vector<std::byte> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (SUCCEEDED(info_queue->GetMessage(index, message, &size)) &&
            (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
             message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION))
        {
            ++validation_errors;
            log << "id=" << message->ID << " severity=" << message->Severity << ' '
                << message->pDescription << '\n';
        }
    }
    info_queue->ClearStoredMessages();
}

void D3D12Renderer::Impl::WriteDred(HRESULT reason) const
{
    std::error_code error;
    std::filesystem::create_directories(config.artifact_directory, error);
    std::ofstream stream(config.artifact_directory / "dred.txt", std::ios::trunc);
    stream << std::format("device_removed_reason=0x{:08X}\n",
                          static_cast<std::uint32_t>(reason));
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (device && SUCCEEDED(device.As(&dred)))
    {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
        D3D12_DRED_PAGE_FAULT_OUTPUT1 page_fault{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
        {
            stream << "breadcrumbs=" << (breadcrumbs.pHeadAutoBreadcrumbNode ? "present" : "none")
                   << '\n';
        }
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&page_fault)))
        {
            stream << "page_fault_va=" << page_fault.PageFaultVA << '\n';
        }
    }
}


} // namespace hs
