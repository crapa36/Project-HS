#include "renderer_impl.hpp"

namespace hs
{

Result D3D12Renderer::Impl::CreateUiTexture()
{
    D3D12_RESOURCE_DESC texture_description{};
    texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_description.Width = kUiWidth;
    texture_description.Height = kUiHeight;
    texture_description.DepthOrArraySize = 1;
    texture_description.MipLevels = 1;
    texture_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_description.SampleDesc = {1, 0};
    texture_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12MA::ALLOCATION_DESC default_allocation{};
    default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    UINT64 row_size{};
    UINT64 total_size{};
    device->GetCopyableFootprints(&texture_description, 0, 1, 0, &ui_footprint, &ui_rows,
                                  &row_size, &total_size);
    D3D12MA::ALLOCATION_DESC upload_allocation{};
    upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    for (std::size_t frame_index = 0; frame_index < frames.size(); ++frame_index)
    {
        if (auto created = CreateAllocation(ui_textures[frame_index], default_allocation,
                                            texture_description, D3D12_RESOURCE_STATE_COMMON);
            !created)
            return created;
        auto &frame = frames[frame_index];
        if (auto created = CreateAllocation(frame.ui_upload, upload_allocation,
                                            BufferDescription(total_size),
                                            D3D12_RESOURCE_STATE_GENERIC_READ);
            !created)
            return created;
        D3D12_RANGE no_read{};
        const auto map_result = frame.ui_upload.resource->Map(
            0, &no_read, reinterpret_cast<void **>(&frame.ui_mapped));
        if (FAILED(map_result)) return HResultFailure("Map UI upload", map_result);
    }

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

    const auto font_path = CurrentExecutableDirectory() / L"Fonts" / L"NotoSansKR.ttf";
    ComPtr<IDWriteFontFile> font_file;
    ComPtr<IDWriteFontSetBuilder1> font_builder;
    ComPtr<IDWriteFontSet> font_set;
    result = ui_dwrite_factory->CreateFontFileReference(font_path.c_str(), nullptr, &font_file);
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

    D2D1_RENDER_TARGET_PROPERTIES properties{};
    properties.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    properties.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    properties.dpiX = properties.dpiY = 96.0f;
    properties.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
    for (auto &surface : ui_surfaces)
    {
        result = ui_wic_factory->CreateBitmap(kUiWidth, kUiHeight,
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

Result D3D12Renderer::Impl::RasterizeUi(std::uint32_t frame_index,
                                        std::span<const UiModel> models)
{
    auto &surface = ui_surfaces[frame_index];
    surface.target->BeginDraw();
    surface.target->SetTransform(D2D1_MATRIX_3X2_F{1, 0, 0, 1, 0, 0});
    surface.target->Clear(D2D1_COLOR_F{0, 0, 0, 0});

    const auto color_of = [](std::uint32_t packed) {
        constexpr float inverse_byte = 1.0f / 255.0f;
        return D2D1_COLOR_F{static_cast<float>(packed & 0xff) * inverse_byte,
                            static_cast<float>((packed >> 8) & 0xff) * inverse_byte,
                            static_cast<float>((packed >> 16) & 0xff) * inverse_byte,
                            static_cast<float>((packed >> 24) & 0xff) * inverse_byte};
    };
    for (const auto &model : models)
    {
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
        if (byte_count == 0) continue;
        const auto wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                     model.utf8_text.data(), byte_count,
                                                     nullptr, 0);
        if (wide_count <= 0) continue;
        std::wstring text(static_cast<std::size_t>(wide_count), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, model.utf8_text.data(),
                            byte_count, text.data(), wide_count);
        ComPtr<IDWriteTextFormat> format;
        const auto font_size = static_cast<float>(
            std::clamp<std::uint16_t>(model.font_pixels, 8, 128));
        const auto format_result = ui_dwrite_factory->CreateTextFormat(
            ui_font_family.c_str(), ui_font_collection.Get(), DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, font_size, L"ko-kr",
            &format);
        if (FAILED(format_result))
            return HResultFailure("Create UI text format", format_result);
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
        ComPtr<IDWriteTextLayout> layout;
        const auto layout_result = ui_dwrite_factory->CreateTextLayout(
            text.data(), static_cast<UINT32>(text.size()), format.Get(),
            text_width, static_cast<float>(kUiHeight), &layout);
        if (FAILED(layout_result))
            return HResultFailure("Create UI text layout", layout_result);
        DWRITE_TEXT_METRICS metrics{};
        auto fitted_size = font_size;
        while (SUCCEEDED(layout->GetMetrics(&metrics)) && fitted_size > 8.0f &&
               (metrics.height > text_height ||
                metrics.widthIncludingTrailingWhitespace > text_width))
        {
            fitted_size -= 1.0f;
            layout->SetFontSize(fitted_size, {0, static_cast<UINT32>(text.size())});
        }
        layout->GetMetrics(&metrics);
        layout->SetMaxHeight(text_height);
        if (metrics.height > text_height ||
            metrics.widthIncludingTrailingWhitespace > text_width)
        {
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(ui_dwrite_factory->CreateEllipsisTrimmingSign(format.Get(),
                                                                        &ellipsis)))
            {
                const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                layout->SetTrimming(&trimming, ellipsis.Get());
            }
        }
        surface.target->DrawTextLayout(
            {text_rectangle.left, text_rectangle.top}, layout.Get(), surface.brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    const auto draw_result = surface.target->EndDraw();
    if (FAILED(draw_result)) return HResultFailure("Rasterize UI", draw_result);

    WICRect lock_rectangle{0, 0, static_cast<INT>(kUiWidth), static_cast<INT>(kUiHeight)};
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
                    pixels + static_cast<std::size_t>(row) * stride, kUiWidth * 4);
    }
    return Result::Success();
}


} // namespace hs
