#include "renderer_impl.hpp"

#include <bit>
#include <cassert>

namespace hs
{

Result D3D12Renderer::Impl::CreateUiTexture(OwnedView &view)
{
    HRESULT result{};
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

    UiResources candidate;
    if (auto prepared = PrepareUiResources(candidate, view.width, view.height); !prepared) return prepared;
    CommitUiResources(view, candidate);
    return Result::Success();
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
    texture_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_B8G8R8A8_UNORM, {}};
    for (auto &texture : resources.textures)
        if (auto created = CreateAllocation(texture, allocation, texture_description,
                D3D12_RESOURCE_STATE_COMMON, &clear); !created) return created;
    return Result::Success();
}

void D3D12Renderer::Impl::CommitUiResources(OwnedView &owner, UiResources &resources) noexcept
{
    // The caller drains GPU users before replacing these view-owned targets.
    resources.text_layouts = std::move(owner.ui.text_layouts);
    owner.ui = std::move(resources);
    owner.ui_draw_content.fill(nullptr);
}

struct D3D12Renderer::Impl::UiTextRenderer final : IDWriteTextRenderer
{
    Impl &renderer;
    UiResources &ui;
    float scale, offset_x, offset_y;
    D2D1_RECT_F clip;
    UINT32 color{};
    std::vector<D2D1_POINT_2F> origins;
    UiTextRenderer(Impl &renderer, OwnedView &view)
        : renderer(renderer), ui(view.ui),
          scale(std::min(float(view.width) / kUiWidth, float(view.height) / kUiHeight)),
          offset_x((view.width - kUiWidth * scale) * 0.5f),
          offset_y((view.height - kUiHeight * scale) * 0.5f),
          clip{0, 0, float(view.width), float(view.height)} {}

    D2D1_RECT_F Pixels(D2D1_RECT_F rect) const
    {
        return {rect.left * scale + offset_x, rect.top * scale + offset_y,
                rect.right * scale + offset_x, rect.bottom * scale + offset_y};
    }

    void Emit(UiQuad quad, UINT32 page)
    {
        if (quad.bounds.right <= quad.bounds.left || quad.bounds.bottom <= quad.bounds.top) return;
        if (ui.batches.empty() || ui.batches.back().page != page)
            ui.batches.push_back({page, static_cast<UINT32>(ui.quads.size()), 0});
        ++ui.batches.back().count;
        ui.quads.push_back(quad);
    }

    void Shape(D2D1_RECT_F rect, UINT32 rgba, float radius = 0)
    {
        Emit({Pixels(rect), clip, rgba, 0, 0, radius * scale},
            ui.batches.empty() ? 0 : ui.batches.back().page);
    }

    HRESULT CacheGlyph(const DWRITE_GLYPH_RUN &run, UINT32 index, float size, UINT8 phase,
                        DWRITE_MEASURING_MODE mode, UiGlyph &glyph)
    {
        DWRITE_GLYPH_RUN single{};
        single.fontFace = run.fontFace;
        single.fontEmSize = size;
        single.glyphCount = 1;
        single.glyphIndices = &run.glyphIndices[index];
        single.isSideways = run.isSideways;
        ComPtr<IDWriteGlyphRunAnalysis> analysis;
        auto hr = renderer.ui_dwrite_factory->CreateGlyphRunAnalysis(&single, nullptr,
            DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC, mode, DWRITE_GRID_FIT_MODE_ENABLED,
            DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, phase * 0.25f, 0, &analysis);
        if (FAILED(hr)) return hr;
        constexpr auto a8 = DWRITE_TEXTURE_ALIASED_1x1; // DirectWrite's 1-byte grayscale coverage plane.
        hr = analysis->GetAlphaTextureBounds(a8, &glyph.bounds);
        if (FAILED(hr)) return hr;
        glyph.face = run.fontFace;
        const auto bytes = static_cast<UINT64>(glyph.bounds.right - glyph.bounds.left) *
            (glyph.bounds.bottom - glyph.bounds.top);
        if (!bytes) return S_OK;
        if (bytes > UINT_MAX - 3) return E_OUTOFMEMORY;
        const auto storage = (static_cast<UINT32>(bytes) + 3u) & ~3u;
        if (ui.atlas.empty() || ui.atlas.back().capacity - ui.atlas.back().used < storage)
        {
            UiAtlasPage page;
            page.capacity = std::max(256u * 1024u, storage);
            D3D12MA::ALLOCATION_DESC allocation{};
            allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
            if (auto created = renderer.CreateAllocation(page.buffer, allocation, BufferDescription(page.capacity),
                    D3D12_RESOURCE_STATE_GENERIC_READ); !created) return E_OUTOFMEMORY;
            ui.atlas.push_back(std::move(page));
        }
        auto &page = ui.atlas.back();
        glyph.page = static_cast<UINT32>(ui.atlas.size() - 1);
        glyph.offset = page.used;
        const D3D12_RANGE no_read{};
        void *mapped{};
        hr = page.buffer.resource->Map(0, &no_read, &mapped);
        if (FAILED(hr)) return hr;
        // Never overwrite bytes referenced by an earlier submitted draw.
        auto *destination = static_cast<BYTE *>(mapped) + page.used;
        hr = analysis->CreateAlphaTexture(a8, &glyph.bounds, destination, static_cast<UINT32>(bytes));
        if (SUCCEEDED(hr))
        {
            std::memset(destination + bytes, 0, storage - bytes);
            page.used += storage;
        }
        const D3D12_RANGE written{glyph.offset, page.used};
        page.buffer.resource->Unmap(0, &written);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void *, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode,
            const DWRITE_GLYPH_RUN *run, const DWRITE_GLYPH_RUN_DESCRIPTION *, IUnknown *) override
    {
        origins.resize(run->glyphCount);
        const DWRITE_MATRIX transform{scale, 0, 0, scale, offset_x, offset_y};
        auto hr = renderer.ui_dwrite_factory->ComputeGlyphOrigins(run, mode, {x,y}, &transform, origins.data());
        if (FAILED(hr)) return hr;
        for (UINT32 i = 0; i < run->glyphCount; ++i)
        {
            const auto subpixel = std::round((origins[i].x * scale + offset_x) * 4) * 0.25f;
            const auto left = std::floor(subpixel);
            const auto phase = static_cast<UINT8>((subpixel - left) * 4);
            const auto size = run->fontEmSize * scale;
            const UiResources::GlyphKey key{run->fontFace, size, run->glyphIndices[i], phase, run->isSideways, mode};
            auto it = ui.glyphs.lower_bound(key);
            if (it == ui.glyphs.end() || it->first != key)
            {
                UiGlyph glyph;
                hr = CacheGlyph(*run, i, size, phase, mode, glyph);
                if (FAILED(hr)) return hr;
                it = ui.glyphs.emplace_hint(it, key, std::move(glyph));
            }
            const auto &glyph = it->second;
            const auto &b = glyph.bounds;
            const auto top = std::round(origins[i].y * scale + offset_y);
            Emit({{left + b.left, top + b.top, left + b.right, top + b.bottom},
                  clip, color, glyph.offset, static_cast<UINT32>(b.right - b.left), 0}, glyph.page);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(void *, FLOAT x, FLOAT y, const DWRITE_UNDERLINE *line, IUnknown *) override
    {
        Shape({x, y + line->offset, x + line->width, y + line->offset + line->thickness}, color);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void *, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH *line, IUnknown *) override
    {
        Shape({x, y + line->offset, x + line->width, y + line->offset + line->thickness}, color);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void *ctx, FLOAT x, FLOAT y, IDWriteInlineObject *object,
            BOOL sideways, BOOL rtl, IUnknown *effect) override
    {
        return object->Draw(ctx, this, x, y, sideways, rtl, effect);
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *, BOOL *value) override { *value = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *value) override { *value = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *, DWRITE_MATRIX *value) override
    {
        *value = {scale, 0, 0, scale, offset_x, offset_y};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IDWritePixelSnapping) && id != __uuidof(IDWriteTextRenderer))
            return E_NOINTERFACE;
        *out = static_cast<IDWriteTextRenderer *>(this);
        AddRef();
        return S_OK;
    }
    // Owned by the synchronous IDWriteTextLayout::Draw call.
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};

Result D3D12Renderer::Impl::PrepareUiDraw(OwnedView &owner, std::uint32_t frame_index,
                                         const std::shared_ptr<const UiDrawContent> &content)
{
    if (!content) return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "UI draw content is required.");
    auto &ui = owner.ui;
    ui.quads.clear();
    ui.batches.clear();
    const auto &models = content->models;
    ui.text_layouts.resize(models.size());
    UiTextRenderer draw(*this, owner);
    const auto viewport_clip = draw.clip;
    for (std::size_t model_index = 0; model_index < models.size(); ++model_index)
    {
        const auto &model = models[model_index];
        const auto element_width = std::max(model.size_pixels.x, 0.0f);
        const auto element_height = std::max(model.size_pixels.y, 0.0f);
        const D2D1_RECT_F rectangle{model.anchor_pixels.x, model.anchor_pixels.y,
                                    model.anchor_pixels.x + element_width,
                                    model.anchor_pixels.y + element_height};
        draw.clip = viewport_clip;
        switch (model.kind)
        {
        case UiModel::Kind::Panel: draw.Shape(rectangle, model.color_rgba); break;
        case UiModel::Kind::Button: draw.Shape(rectangle, model.color_rgba, 11.0f); break;
        case UiModel::Kind::Bar:
        {
            draw.Shape(rectangle, 0xb8090605u);
            auto filled = rectangle;
            filled.right = filled.left + element_width * std::clamp(model.value, 0.0f, 1.0f);
            draw.Shape(filled, model.color_rgba);
            break;
        }
        case UiModel::Kind::Text: break;
        }

        const auto end = std::find(model.utf8_text.begin(), model.utf8_text.end(), '\0');
        const auto byte_count = static_cast<int>(end - model.utf8_text.begin());
        if (byte_count == 0)
        {
            owner.ui.text_layouts[model_index] = {};
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
        draw.color = model.kind == UiModel::Kind::Text ? model.color_rgba : 0xffffffffu;
        const auto text_width = std::max(text_rectangle.right - text_rectangle.left, 1.0f);
        const auto text_height = std::max(text_rectangle.bottom - text_rectangle.top, 1.0f);
        auto &cached_layout = owner.ui.text_layouts[model_index];
        const std::string_view utf8{model.utf8_text.data(), static_cast<std::size_t>(byte_count)};
        if (!cached_layout.layout || cached_layout.utf8 != utf8 || cached_layout.kind != model.kind ||
            cached_layout.font_size != font_size || cached_layout.width != text_width ||
            cached_layout.height != text_height)
        {
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
            cached_layout.utf8.assign(utf8);
            cached_layout.kind = model.kind;
            cached_layout.font_size = font_size;
            cached_layout.width = text_width;
            cached_layout.height = text_height;
            cached_layout.layout = std::move(layout);
        }
        auto *draw_layout = cached_layout.layout.Get();
        assert(draw_layout->GetMaxWidth() == text_width &&
               draw_layout->GetMaxHeight() == text_height);
        draw.clip = draw.Pixels({text_rectangle.left, text_rectangle.top,
            text_rectangle.left + text_width, text_rectangle.top + text_height});
        const auto hr = draw_layout->Draw(nullptr, &draw, text_rectangle.left, text_rectangle.top);
        if (FAILED(hr)) return HResultFailure("Prepare UI glyphs", hr);
    }

    auto &frame = frames[frame_index];
    const auto bytes = ui.quads.size() * sizeof(UiQuad);
    if (bytes > UINT_MAX)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "UI draw stream exceeds the native draw range.");
    if (!frame.ui_upload.resource || frame.ui_upload_bytes < bytes)
    {
        const auto capacity = std::max<std::uint64_t>(4096, std::bit_ceil(static_cast<std::uint64_t>(bytes)));
        AllocationResource upload;
        D3D12MA::ALLOCATION_DESC allocation{};
        allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto created = CreateAllocation(upload, allocation, BufferDescription(capacity),
                D3D12_RESOURCE_STATE_GENERIC_READ); !created) return created;
        const D3D12_RANGE no_read{};
        void *mapped{};
        const auto hr = upload.resource->Map(0, &no_read, &mapped);
        if (FAILED(hr)) return HResultFailure("Map UI geometry", hr);
        if (frame.ui_upload.resource && frame.ui_mapped) frame.ui_upload.resource->Unmap(0, nullptr);
        frame.ui_upload = std::move(upload);
        frame.ui_mapped = static_cast<std::byte *>(mapped);
        frame.ui_upload_bytes = capacity;
    }
    if (bytes) std::memcpy(frame.ui_mapped, ui.quads.data(), bytes);
    return Result::Success();
}


} // namespace hs
