#include "renderer_impl.hpp"

#include <cassert>

namespace hs
{

Result D3D12Renderer::Impl::CreateUiTexture()
{
    auto result = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&ui_wic_factory));
    if (FAILED(result)) return HResultFailure("Create WIC UI factory", result);
    result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               ui_d2d_factory.ReleaseAndGetAddressOf());
    if (FAILED(result)) return HResultFailure("Create Direct2D UI factory", result);

    ComPtr<IDWriteFactory> base_dwrite_factory;
    result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown **>(base_dwrite_factory.ReleaseAndGetAddressOf()));
    if (FAILED(result) || FAILED(base_dwrite_factory.As(&ui_dwrite_factory)))
        return HResultFailure("Create DirectWrite UI factory", FAILED(result) ? result : E_FAIL);

    ComPtr<IDWriteFontFile> font_file;
    ComPtr<IDWriteFontSetBuilder1> font_builder;
    ComPtr<IDWriteFontSet> font_set;
    if (!config.font_bytes.empty())
    {
        if (config.font_bytes.size() > UINT32_MAX)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Font exceeds DirectWrite byte limit.");
        result = ui_dwrite_factory->CreateInMemoryFontFileLoader(&ui_font_loader);
        if (SUCCEEDED(result))
        {
            result = ui_dwrite_factory->RegisterFontFileLoader(ui_font_loader.Get());
            if (FAILED(result)) ui_font_loader.Reset();
        }
        if (SUCCEEDED(result))
            result = ui_font_loader->CreateInMemoryFontFileReference(ui_dwrite_factory.Get(),
                config.font_bytes.data(), static_cast<UINT32>(config.font_bytes.size()), nullptr, &font_file);
        // Null owner requests DirectWrite's owned copy (dwrite_3.h).
        config.font_bytes = {};
    }
    else
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Font input was not supplied.");
    if (SUCCEEDED(result)) result = ui_dwrite_factory->CreateFontSetBuilder(&font_builder);
    if (SUCCEEDED(result)) result = font_builder->AddFontFile(font_file.Get());
    if (SUCCEEDED(result)) result = font_builder->CreateFontSet(&font_set);
    if (SUCCEEDED(result))
        result = ui_dwrite_factory->CreateFontCollectionFromFontSet(font_set.Get(),
                                                                    &ui_font_collection);
    if (FAILED(result))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Bundled Noto Sans KR font is missing or invalid.");

    ComPtr<IDWriteFontFamily> font_family;
    ComPtr<IDWriteLocalizedStrings> family_names;
    if (ui_font_collection->GetFontFamilyCount() == 0 ||
        FAILED(ui_font_collection->GetFontFamily(0, &font_family)) ||
        FAILED(font_family->GetFamilyNames(&family_names)))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Bundled Noto Sans KR font has no family name.");
    UINT32 family_name_index{};
    BOOL family_name_exists{};
    family_names->FindLocaleName(L"ko-kr", &family_name_index, &family_name_exists);
    if (!family_name_exists) family_name_index = 0;
    UINT32 family_name_length{};
    family_names->GetStringLength(family_name_index, &family_name_length);
    ui_font_family.resize(family_name_length + 1);
    family_names->GetString(family_name_index, ui_font_family.data(), family_name_length + 1);
    ui_font_family.resize(family_name_length);

    UiResources resources;
    if (auto prepared = PrepareUiResources(resources, width, height); !prepared) return prepared;
    CommitUiResources(resources);
    return Result::Success();
}

D3D12Renderer::Impl::UiResources::~UiResources()
{
    for (std::size_t frame = 0; frame < uploads.size(); ++frame)
        if (uploads[frame].resource && mapped[frame])
            uploads[frame].resource->Unmap(0, nullptr);
}

Result D3D12Renderer::Impl::PrepareUiResources(UiResources &resources,
                                              std::uint32_t output_width,
                                              std::uint32_t output_height)
{
    if (!output_width || !output_height ||
        output_width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        output_height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "UI raster extent is outside the native texture limits.");
    D3D12_RESOURCE_DESC texture_description{};
    texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_description.Width = output_width;
    texture_description.Height = output_height;
    texture_description.DepthOrArraySize = texture_description.MipLevels = 1;
    texture_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_description.SampleDesc.Count = 1;
    texture_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resources.raster_width = output_width;
    resources.raster_height = output_height;
    device->GetCopyableFootprints(&texture_description, 0, 1, 0, &resources.footprint,
                                  &resources.rows, &resources.row_bytes, &resources.upload_bytes);
    D3D12MA::ALLOCATION_DESC default_allocation{};
    default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12MA::ALLOCATION_DESC upload_allocation{};
    upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    D2D1_RENDER_TARGET_PROPERTIES properties{};
    properties.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    properties.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    // WIC/D2D coordinates are raster pixels. The authored logical scale is
    // applied once while drawing, so monitor DPI is not applied a second time.
    properties.dpiX = properties.dpiY = 96.0f;
    properties.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
    for (std::size_t frame_index = 0; frame_index < resources.textures.size(); ++frame_index)
    {
        if (auto created = CreateAllocation(resources.textures[frame_index], default_allocation,
                texture_description, D3D12_RESOURCE_STATE_COMMON); !created)
            return created;
        if (auto created = CreateAllocation(resources.uploads[frame_index], upload_allocation,
                BufferDescription(resources.upload_bytes), D3D12_RESOURCE_STATE_GENERIC_READ); !created)
            return created;
        const D3D12_RANGE no_read{};
        void *mapped{};
        auto result = resources.uploads[frame_index].resource->Map(0, &no_read, &mapped);
        if (FAILED(result)) return HResultFailure("Map UI upload", result);
        resources.mapped[frame_index] = static_cast<std::byte *>(mapped);
        auto &surface = resources.surfaces[frame_index];
        result = ui_wic_factory->CreateBitmap(output_width, output_height,
                                              GUID_WICPixelFormat32bppPBGRA,
                                              WICBitmapCacheOnLoad, &surface.bitmap);
        if (SUCCEEDED(result))
            result = ui_d2d_factory->CreateWicBitmapRenderTarget(
                surface.bitmap.Get(), properties, &surface.target);
        if (SUCCEEDED(result))
            result = surface.target->CreateSolidColorBrush(
                D2D1_COLOR_F{1, 1, 1, 1}, &surface.brush);
        if (FAILED(result)) return HResultFailure("Create DirectWrite UI surface", result);
        surface.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    return Result::Success();
}

void D3D12Renderer::Impl::CommitUiResources(UiResources &resources) noexcept
{
    // The caller drains GPU users before replacing resources/descriptors. The
    // candidate then owns the retired resources and their mapped upload pointers.
    using std::swap;
    swap(ui_textures, resources.textures);
    swap(ui_surfaces, resources.surfaces);
    swap(ui_footprint, resources.footprint);
    swap(ui_rows, resources.rows);
    swap(ui_raster_width, resources.raster_width);
    swap(ui_raster_height, resources.raster_height);
    swap(ui_row_bytes, resources.row_bytes);
    swap(ui_upload_bytes, resources.upload_bytes);
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        swap(frames[index].ui_upload, resources.uploads[index]);
        swap(frames[index].ui_mapped, resources.mapped[index]);
        frames[index].ui_initialized = false;
        ui_raster_models[index].clear();
        ui_dirty[index] = false;
    }
    if (!srv_cpu_heap) return; // Startup post tables are written after heap creation.
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
        for (const auto table : {0u, kToneMapInputDescriptorOffset, kFxaaInputDescriptorOffset})
        {
            auto descriptor = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            descriptor.ptr += (static_cast<SIZE_T>(frame) * kTextureDescriptorCount + table + 9) * srv_stride;
            device->CreateShaderResourceView(ui_textures[frame].resource.Get(), &view, descriptor);
        }
}

Result D3D12Renderer::Impl::RasterizeUi(std::uint32_t frame_index,
                                        std::span<const UiModel> models)
{
    const auto equal = [](const UiModel &left, const UiModel &right) {
        return left.kind == right.kind && left.anchor_pixels.x == right.anchor_pixels.x &&
            left.anchor_pixels.y == right.anchor_pixels.y &&
            left.size_pixels.x == right.size_pixels.x && left.size_pixels.y == right.size_pixels.y &&
            left.color_rgba == right.color_rgba && left.value == right.value &&
            left.font_pixels == right.font_pixels && left.utf8_text == right.utf8_text;
    };
    const auto &cached = ui_raster_models[frame_index];
    auto &surface = ui_surfaces[frame_index];
    if (surface.copy_completion.IsSatisfiedBy(graph_queue_executor.Completed()))
        surface.completed_version = surface.submitted_version;
    if (ui_surfaces[frame_index].bitmap_valid &&
        (surface.completed_version == surface.bitmap_version || ui_dirty[frame_index]) &&
        cached.size() == models.size() && std::equal(cached.begin(), cached.end(), models.begin(), equal))
        return Result::Success();
    // Candidate layouts and model identity publish only after the whole raster succeeds.
    ui_text_layouts.resize(std::max(ui_text_layouts.size(), models.size()));
    std::vector<UiTextLayout> candidate_layouts(models.size());
    std::vector<bool> replace_layout(models.size());
    std::vector<UiModel> candidate_models(models.begin(), models.end());
    // A failed raster cannot make the old model a valid bitmap on the next retry.
    surface.bitmap_valid = false;
    ui_dirty[frame_index] = false;
    // shortcut: whole-surface raster/upload stays until GR25 proves partial bitmap and native-copy equivalence.
    surface.target->BeginDraw();
    surface.target->SetTransform(D2D1_MATRIX_3X2_F{1, 0, 0, 1, 0, 0});
    surface.target->Clear(D2D1_COLOR_F{0, 0, 0, 0});
    const auto logical_scale = std::min(static_cast<float>(ui_raster_width) / kUiWidth,
                                       static_cast<float>(ui_raster_height) / kUiHeight);
    const auto offset_x = (static_cast<float>(ui_raster_width) - kUiWidth * logical_scale) * 0.5f;
    const auto offset_y = (static_cast<float>(ui_raster_height) - kUiHeight * logical_scale) * 0.5f;
    surface.target->SetTransform(D2D1_MATRIX_3X2_F{
        logical_scale, 0, 0, logical_scale, offset_x, offset_y});

    const auto color_of = [](std::uint32_t packed) {
        constexpr float inverse_byte = 1.0f / 255.0f;
        return D2D1_COLOR_F{static_cast<float>(packed & 0xff) * inverse_byte,
                            static_cast<float>((packed >> 8) & 0xff) * inverse_byte,
                            static_cast<float>((packed >> 16) & 0xff) * inverse_byte,
                            static_cast<float>((packed >> 24) & 0xff) * inverse_byte};
    };
    for (std::size_t model_index = 0; model_index < models.size(); ++model_index)
    {
        const auto &model = models[model_index];
        const auto element_width = std::max(model.size_pixels.x, 0.0f);
        const auto element_height = std::max(model.size_pixels.y, 0.0f);
        const D2D1_RECT_F rectangle{model.anchor_pixels.x, model.anchor_pixels.y,
                                    model.anchor_pixels.x + element_width,
                                    model.anchor_pixels.y + element_height};
        surface.brush->SetColor(color_of(model.color_rgba));
        switch (model.kind)
        {
        case UiModel::Kind::Panel:
            if (element_width > 0 && element_height > 0)
                surface.target->FillRectangle(rectangle, surface.brush.Get());
            break;
        case UiModel::Kind::Button:
            if (element_width > 0 && element_height > 0)
            {
                const D2D1_ROUNDED_RECT rounded{rectangle, 11.0f, 11.0f};
                surface.target->FillRoundedRectangle(rounded, surface.brush.Get());
            }
            break;
        case UiModel::Kind::Bar:
            if (element_width > 0 && element_height > 0)
            {
                surface.brush->SetColor(D2D1_COLOR_F{0.02f, 0.025f, 0.035f, 0.72f});
                surface.target->FillRectangle(rectangle, surface.brush.Get());
                auto filled = rectangle;
                filled.right = filled.left +
                               element_width * std::clamp(model.value, 0.0f, 1.0f);
                surface.brush->SetColor(color_of(model.color_rgba));
                surface.target->FillRectangle(filled, surface.brush.Get());
            }
            break;
        case UiModel::Kind::Text: break;
        }

        const auto end = std::find(model.utf8_text.begin(), model.utf8_text.end(), '\0');
        const auto byte_count = static_cast<int>(end - model.utf8_text.begin());
        if (byte_count == 0)
        {
            replace_layout[model_index] = true;
            continue;
        }
        const auto font_size = static_cast<float>(
            std::clamp<std::uint16_t>(model.font_pixels, 8, 128));
        const auto inset_x = model.kind == UiModel::Kind::Button ||
                                     model.kind == UiModel::Kind::Panel
                                 ? 14.0f
                                 : model.kind == UiModel::Kind::Bar ? 8.0f : 0.0f;
        const auto inset_y = model.kind == UiModel::Kind::Button ||
                                     model.kind == UiModel::Kind::Panel
                                 ? 9.0f
                                 : 0.0f;
        const D2D1_RECT_F text_rectangle{
            model.anchor_pixels.x + inset_x, model.anchor_pixels.y + inset_y,
            model.anchor_pixels.x +
                (element_width > 0 ? element_width - inset_x : kUiWidth - model.anchor_pixels.x),
            model.anchor_pixels.y +
                (element_height > 0 ? element_height - inset_y : font_size * 1.6f)};
        surface.brush->SetColor(model.kind == UiModel::Kind::Text
                                    ? color_of(model.color_rgba)
                                    : D2D1_COLOR_F{1, 1, 1, 1});
        const auto text_width = std::max(text_rectangle.right - text_rectangle.left, 1.0f);
        const auto text_height = std::max(text_rectangle.bottom - text_rectangle.top, 1.0f);
        auto &cached_layout = ui_text_layouts[model_index];
        const std::string_view utf8{model.utf8_text.data(), static_cast<std::size_t>(byte_count)};
        if (!cached_layout.layout || cached_layout.utf8 != utf8 || cached_layout.kind != model.kind ||
            cached_layout.font_size != font_size || cached_layout.width != text_width ||
            cached_layout.height != text_height)
        {
            replace_layout[model_index] = true;
            const auto wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                         model.utf8_text.data(), byte_count,
                                                         nullptr, 0);
            if (wide_count <= 0) continue;
            std::wstring text(static_cast<std::size_t>(wide_count), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, model.utf8_text.data(),
                                byte_count, text.data(), wide_count);
            ComPtr<IDWriteTextFormat> format;
            const auto format_result = ui_dwrite_factory->CreateTextFormat(
                ui_font_family.c_str(), ui_font_collection.Get(), DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, font_size, L"ko-kr",
                &format);
            if (FAILED(format_result))
            {
                (void)surface.target->EndDraw();
                return HResultFailure("Create UI text format", format_result);
            }
            const auto centered_control = model.kind == UiModel::Kind::Button ||
                                          model.kind == UiModel::Kind::Bar;
            format->SetWordWrapping(model.kind == UiModel::Kind::Bar
                                        ? DWRITE_WORD_WRAPPING_NO_WRAP
                                        : DWRITE_WORD_WRAPPING_WRAP);
            if (centered_control)
            {
                format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            ComPtr<IDWriteTextLayout> layout;
            const auto layout_result = ui_dwrite_factory->CreateTextLayout(
                text.data(), static_cast<UINT32>(text.size()), format.Get(),
                text_width, static_cast<float>(kUiHeight), &layout);
            if (FAILED(layout_result))
            {
                (void)surface.target->EndDraw();
                return HResultFailure("Create UI text layout", layout_result);
            }
            DWRITE_TEXT_METRICS metrics{};
            auto fitted_size = font_size;
            while (SUCCEEDED(layout->GetMetrics(&metrics)) && fitted_size > 8.0f &&
                   (metrics.height > text_height ||
                    metrics.width > text_width))
            {
                fitted_size -= 1.0f;
                layout->SetFontSize(fitted_size, {0, static_cast<UINT32>(text.size())});
            }
            layout->GetMetrics(&metrics);
            layout->SetMaxHeight(text_height);
            if (metrics.height > text_height ||
                metrics.width > text_width)
            {
                ComPtr<IDWriteInlineObject> ellipsis;
                if (SUCCEEDED(ui_dwrite_factory->CreateEllipsisTrimmingSign(format.Get(),
                                                                            &ellipsis)))
                {
                    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                    layout->SetTrimming(&trimming, ellipsis.Get());
                }
            }
            auto &candidate = candidate_layouts[model_index];
            candidate.utf8.assign(utf8);
            candidate.kind = model.kind;
            candidate.font_size = font_size;
            candidate.width = text_width;
            candidate.height = text_height;
            candidate.layout = std::move(layout);
        }
        auto *draw_layout = replace_layout[model_index]
                                ? candidate_layouts[model_index].layout.Get()
                                : cached_layout.layout.Get();
        assert(draw_layout->GetMaxWidth() == text_width &&
               draw_layout->GetMaxHeight() == text_height);
        surface.target->DrawTextLayout(
            {text_rectangle.left, text_rectangle.top}, draw_layout, surface.brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    const auto draw_result = surface.target->EndDraw();
    if (FAILED(draw_result)) return HResultFailure("Rasterize UI", draw_result);

    WICRect lock_rectangle{0, 0, static_cast<INT>(ui_raster_width), static_cast<INT>(ui_raster_height)};
    ComPtr<IWICBitmapLock> bitmap_lock;
    auto result = surface.bitmap->Lock(&lock_rectangle, WICBitmapLockRead, &bitmap_lock);
    UINT stride{};
    UINT byte_count{};
    BYTE *pixels{};
    if (SUCCEEDED(result)) result = bitmap_lock->GetStride(&stride);
    if (SUCCEEDED(result)) result = bitmap_lock->GetDataPointer(&byte_count, &pixels);
    if (FAILED(result)) return HResultFailure("Lock UI pixels", result);
    auto &frame = frames[frame_index];
    for (std::uint32_t row = 0; row < ui_rows; ++row)
    {
        std::memcpy(frame.ui_mapped + ui_footprint.Offset + row * ui_footprint.Footprint.RowPitch,
                    pixels + static_cast<std::size_t>(row) * stride, static_cast<std::size_t>(ui_row_bytes));
    }
    for (std::size_t index = 0; index < models.size(); ++index)
        if (replace_layout[index]) ui_text_layouts[index] = std::move(candidate_layouts[index]);
    ui_text_layouts.resize(models.size());
    ui_raster_models[frame_index].swap(candidate_models);
    ui_dirty[frame_index] = true;
    ++surface.bitmap_version;
    surface.bitmap_valid = true;
    return Result::Success();
}


} // namespace hs
