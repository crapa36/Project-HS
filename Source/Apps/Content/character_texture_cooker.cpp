#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "character_cooker.hpp"
#include "character_texture_mips.hpp"
#include "character_bc7_coverage.hpp"
#include "character_bc7_normal.hpp"

#include <hs/render_data/dds_texture.hpp>

#include <d3d11.h>
#include <dxgi.h>
#include <DirectXTex.h>
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hs::content
{
namespace
{
bool AtomicWriteCharacterFile(const std::filesystem::path &path,
                              std::span<const std::byte> bytes,
                              std::string &error_message)
{
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.write(reinterpret_cast<const char *>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size())))
        {
            error_message = "cannot write temporary file";
            return false;
        }
    }
    const auto handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        error_message = "cannot open temporary file for flush";
        DeleteFileW(temporary.c_str());
        return false;
    }
    const auto flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!flushed || !MoveFileExW(temporary.c_str(), path.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error_message = "atomic replacement failed with Win32 error " +
                        std::to_string(GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}
} // namespace
namespace
{
struct CharacterBc7Encoder
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    std::string identity;
    CharacterBc7Encoder()
    {
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        const auto native = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, 1, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr);
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description{};
        LARGE_INTEGER driver{};
        if (FAILED(native) || FAILED(device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
            FAILED(adapter->GetDesc(&description)) || FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver)))
            device.Reset();
        identity = "bc7-semantic-v4-color-linear-normal-base-unit-postmip-v1-normal-local-cpu3subsets-mode6-angular-alpha-nonreg-v1-normal-renorm-roughness-linear-alpha020-098-boundary-u8guard-local-mode5-floatclass-zero173opaque-anchor-v1-";
        if (device)
            identity += "gpu-d3d11-3subsets-alpha1-v" + std::to_string(description.VendorId) +
                "-d" + std::to_string(description.DeviceId) + "-r" + std::to_string(description.Revision) +
                "-driver" + std::to_string(driver.QuadPart);
        else identity += "cpu-parallel-3subsets-threshold05";
    }
};
CharacterBc7Encoder &CharacterEncoder()
{
    static CharacterBc7Encoder encoder;
    return encoder;
}
struct CharacterTextureSources
{
    std::map<std::filesystem::path, std::vector<std::uint8_t>> decoded;
    std::map<std::filesystem::path, std::size_t> remaining;
    explicit CharacterTextureSources(std::span<const std::filesystem::path> paths)
    {
        for (const auto &path : paths) if (!path.empty()) ++remaining[path];
    }
    void Consumed(const std::filesystem::path &path)
    {
        if (--remaining.at(path) == 0) decoded.erase(path);
    }
};

bool RepairCharacterNormalBlocks(const CharacterMip &reference,const DirectX::Image &wire,
    const DirectX::Image &encoded,const DirectX::Image &decoded)
{
    constexpr std::array flags{DirectX::TEX_COMPRESS_BC7_USE_3SUBSETS,DirectX::TEX_COMPRESS_BC7_QUICK};
    std::array<std::size_t,2> selected{};
    for(std::uint32_t by=0;by<reference.height;by+=4) for(std::uint32_t bx=0;bx<reference.width;bx+=4)
    {
        CharacterNormalBlock source{},original{};std::array<std::uint8_t,64> input_bytes{};
        bool needs_repair{};
        for(std::uint32_t y=0;y<4;++y) for(std::uint32_t x=0;x<4;++x)
        {
            const auto sx=std::min(bx+x,reference.width-1),sy=std::min(by+y,reference.height-1);
            const auto index=y*4+x;
            source[index]=reference.pixels[std::size_t(sy)*reference.width+sx];
            const auto *pixel=reinterpret_cast<const float *>(decoded.pixels+sy*decoded.rowPitch)+sx*4;
            std::copy_n(pixel,4,original[index].begin());
            std::memcpy(input_bytes.data()+index*4,wire.pixels+sy*wire.rowPitch+sx*4,4);
            needs_repair|=!CharacterNormalDirectionsCompatible(source[index],original[index]);
        }
        if(!needs_repair) continue;
        DirectX::Image input{};input.width=input.height=4;input.format=DXGI_FORMAT_R8G8B8A8_UNORM;
        input.rowPitch=16;input.slicePitch=input_bytes.size();input.pixels=input_bytes.data();
        CharacterNormalBlockScore best;best.valid=false;std::array<std::uint8_t,16> best_bytes{};
        std::size_t backend=flags.size();
        for(std::size_t candidate=0;candidate<flags.size();++candidate)
        {
            DirectX::ScratchImage compressed_candidate,decoded_candidate;
            auto result=DirectX::Compress(input,DXGI_FORMAT_BC7_UNORM,flags[candidate],0.5f,compressed_candidate);
            const auto *bytes=SUCCEEDED(result)?compressed_candidate.GetImage(0,0,0):nullptr;
            if(bytes) result=DirectX::Decompress(*bytes,DXGI_FORMAT_R32G32B32A32_FLOAT,decoded_candidate);
            const auto *image=SUCCEEDED(result)?decoded_candidate.GetImage(0,0,0):nullptr;
            if(!bytes||bytes->format!=DXGI_FORMAT_BC7_UNORM||bytes->slicePitch!=16||!image||image->width!=4||image->height!=4||
                image->format!=DXGI_FORMAT_R32G32B32A32_FLOAT||image->rowPitch<64)
            {
                continue;
            }
            CharacterNormalBlock actual{};
            for(std::size_t y=0;y<4;++y) for(std::size_t x=0;x<4;++x)
                std::copy_n(reinterpret_cast<const float *>(image->pixels+y*image->rowPitch)+x*4,4,actual[y*4+x].begin());
            auto score=ScoreCharacterNormalBlock(source,actual);
            const bool alpha_ok=CharacterNormalAlphaErrorNotWorse(source,original,actual);
            if(!score.valid||!alpha_ok) continue;
            if(backend==flags.size()||PreferCharacterNormalBlock(score,best))
            { best=score;backend=candidate;std::memcpy(best_bytes.data(),bytes->pixels,best_bytes.size()); }
        }
        if(backend==flags.size()) continue;
        std::memcpy(encoded.pixels+std::size_t(by/4)*encoded.rowPitch+(bx/4)*16,best_bytes.data(),best_bytes.size());
        ++selected[backend];
    }
    return selected[0]+selected[1]!=0;
}

bool WriteDds(IWICImagingFactory &factory, CharacterTextureSources &sources, const std::filesystem::path &path,
    const std::filesystem::path &source, std::array<std::uint8_t, 4> fallback,
    std::string &error_message, std::uint32_t final_size, CharacterTextureSemantic semantic,
    CharacterAlphaPolicy alpha = {})
{
    try
    {
        CharacterMip base{final_size, final_size, {}};
        if (source.empty())
        {
            base = CharacterConstantSlice(final_size, final_size, fallback);
        }
        else
        {
            auto [cached, first_decode] = sources.decoded.try_emplace(source);
            auto &pixels = cached->second;
            if (first_decode)
            {
                Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
                Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
                Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
                auto result = factory.CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ,
                    WICDecodeMetadataCacheOnDemand, &decoder);
                if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
                if (SUCCEEDED(result)) result = factory.CreateFormatConverter(&converter);
                if (SUCCEEDED(result)) result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                    WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
                UINT width{}, height{};
                if (SUCCEEDED(result)) result = converter->GetSize(&width, &height);
                if (FAILED(result) || width != final_size || height != final_size)
                    throw std::runtime_error("character texture extent differs from its final array slice: " + source.string());
                pixels.resize(std::size_t(width) * height * 4);
                result = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data());
                if (FAILED(result)) throw std::runtime_error("cannot copy character texture pixels: " + source.string());
            }
            if (pixels.size() != std::size_t(final_size) * final_size * 4)
                throw std::runtime_error("shared character texture source has an incompatible extent");
            base.pixels.resize(std::size_t(final_size) * final_size);
            for (std::size_t index = 0; index < base.pixels.size(); ++index)
                for (std::size_t channel = 0; channel < 4; ++channel)
                    base.pixels[index][channel] = pixels[index * 4 + channel] / 255.0f;
            sources.Consumed(source); // Release decode-only bytes after their last actual semantic consumer.
        }
        auto mips = BuildCharacterMipChain(std::move(base), semantic, alpha);
        if (semantic == CharacterTextureSemantic::NormalRoughness)
        {
            CanonicalizeCharacterNormalBase(mips.front());
        }
        const bool color = semantic == CharacterTextureSemantic::Color;
        const auto format = color ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
        DirectX::ScratchImage input, compressed;
        auto checked = [&](HRESULT result, const char *operation) {
            if (FAILED(result)) throw std::runtime_error(std::string(operation) + ": " + path.string());
        };
        // Mip RGB is already sRGB encoded; matching input/output tags prevent a second conversion.
        checked(input.Initialize2D(color ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM,
            final_size, final_size, 1, mips.size()), "allocate character semantic mip chain");
        for (std::size_t mip = 0; mip < mips.size(); ++mip)
        {
            const auto *image = input.GetImage(mip, 0, 0);
            for (std::uint32_t row = 0; row < mips[mip].height; ++row)
                for (std::uint32_t column = 0; column < mips[mip].width; ++column)
                    for (std::size_t channel = 0; channel < 4; ++channel)
                        image->pixels[row * image->rowPitch + column * 4 + channel] = static_cast<std::uint8_t>(
                            std::lround(std::clamp(mips[mip].pixels[std::size_t(row) * mips[mip].width + column][channel], 0.0f, 1.0f) * 255));
        }
        const auto &encoder = CharacterEncoder();
        if (encoder.device)
            checked(DirectX::Compress(encoder.device.Get(), input.GetImages(), input.GetImageCount(), input.GetMetadata(),
                format, DirectX::TEX_COMPRESS_BC7_USE_3SUBSETS, 1.0f, compressed), "GPU compress character BC7");
        else
            checked(DirectX::Compress(input.GetImages(), input.GetImageCount(), input.GetMetadata(), format,
                static_cast<DirectX::TEX_COMPRESS_FLAGS>(DirectX::TEX_COMPRESS_PARALLEL | DirectX::TEX_COMPRESS_BC7_USE_3SUBSETS),
                0.5f, compressed), "CPU compress character BC7");
        // Keep decoded QA for every mip, including the minimum-one-block tail.
        // These observations do not declare visual/target-hardware qualification.
        for (std::size_t mip = 0; mip < mips.size(); ++mip)
        {
            const auto *block = compressed.GetImage(mip, 0, 0);
            const auto row = ((mips[mip].width + 3) / 4) * 16;
            const auto rows = (mips[mip].height + 3) / 4;
            if (!block || block->rowPitch != row || block->slicePitch != std::size_t(row) * rows)
                throw std::runtime_error("character BC7 block layout mismatch");
            DirectX::ScratchImage decoded;
            checked(DirectX::Decompress(*block, DXGI_FORMAT_R32G32B32A32_FLOAT, decoded), "decode character BC7 QA");
            const auto *image = decoded.GetImage(0, 0, 0);
            if (!image || image->width != mips[mip].width || image->height != mips[mip].height ||
                image->format != DXGI_FORMAT_R32G32B32A32_FLOAT || image->rowPitch < image->width * 4 * sizeof(float))
                throw std::runtime_error("character BC7 initial QA extent mismatch");
            if(semantic==CharacterTextureSemantic::NormalRoughness &&
                RepairCharacterNormalBlocks(mips[mip],*input.GetImage(mip,0,0),*block,*image))
            {
                decoded.Release();
                checked(DirectX::Decompress(*block,DXGI_FORMAT_R32G32B32A32_FLOAT,decoded),"decode repaired character normal full mip QA");
                image=decoded.GetImage(0,0,0);
                if(!image||image->width!=mips[mip].width||image->height!=mips[mip].height||
                    image->format!=DXGI_FORMAT_R32G32B32A32_FLOAT||image->rowPitch<image->width*4*sizeof(float))
                    throw std::runtime_error("character BC7 repaired normal QA extent mismatch");
            }
            std::size_t repaired_blocks{};
            if (alpha.cutoff > 0)
            {
                // Preserve the semantic float mip's per-texel decision, including
                // decisions changed by the intermediate R8 wire quantization.
                const auto block_columns = (mips[mip].width + 3) / 4;
                std::vector<std::uint8_t> repair(std::size_t(block_columns) * rows);
                for (std::uint32_t y = 0; y < mips[mip].height; ++y)
                    for (std::uint32_t x = 0; x < mips[mip].width; ++x)
                    {
                        const auto &reference = mips[mip].pixels[std::size_t(y) * mips[mip].width + x];
                        const auto *pixel = reinterpret_cast<const float *>(image->pixels + y * image->rowPitch) + x * 4;
                        const bool wanted = reference[3] >= alpha.cutoff;
                        if ((pixel[3] >= alpha.cutoff) != wanted)
                            repair[std::size_t(y / 4) * block_columns + x / 4] = 1;
                    }
                for (std::uint32_t by = 0; by < rows; ++by)
                    for (std::uint32_t bx = 0; bx < block_columns; ++bx)
                    {
                        if (!repair[std::size_t(by) * block_columns + bx]) continue;
                        CharacterBc7Texels source_block{};
                        for (std::uint32_t y = 0; y < 4; ++y) for (std::uint32_t x = 0; x < 4; ++x)
                        {
                            const auto sx = std::min(bx * 4 + x, mips[mip].width - 1);
                            const auto sy = std::min(by * 4 + y, mips[mip].height - 1);
                            source_block[y * 4 + x] = mips[mip].pixels[std::size_t(sy) * mips[mip].width + sx];
                        }
                        auto *destination = block->pixels + std::size_t(by) * block->rowPitch + bx * 16;
                        CharacterBc7Block original{}, replacement{};
                        std::memcpy(original.data(), destination, original.size());
                        if (!EncodeCharacterBc7CoverageBlock(source_block, alpha, original, replacement))
                            throw std::runtime_error("character BC7 cannot preserve this block's float cutoff and zero/body/opaque anchors: " +
                                path.string() + " mip=" + std::to_string(mip) + " block=" + std::to_string(bx) + "," + std::to_string(by));
                        std::memcpy(destination, replacement.data(), replacement.size());
                        ++repaired_blocks;
                    }
                if (repaired_blocks)
                {
                    // The library decoder, not the helper's packing model, is
                    // the final truth for every repaired block and partial tail.
                    decoded.Release();
                    checked(DirectX::Decompress(*block, DXGI_FORMAT_R32G32B32A32_FLOAT, decoded), "decode repaired character BC7 QA");
                    image = decoded.GetImage(0, 0, 0);
                    if (!image || image->width != mips[mip].width || image->height != mips[mip].height ||
                        image->format != DXGI_FORMAT_R32G32B32A32_FLOAT || image->rowPitch < image->width * 4 * sizeof(float))
                        throw std::runtime_error("character BC7 repaired QA extent mismatch");
                    for (std::uint32_t y = 0; y < mips[mip].height; ++y)
                        for (std::uint32_t x = 0; x < mips[mip].width; ++x)
                        {
                            if (!repair[std::size_t(y / 4) * block_columns + x / 4]) continue;
                            const auto &reference = mips[mip].pixels[std::size_t(y) * mips[mip].width + x];
                            const auto *pixel = reinterpret_cast<const float *>(image->pixels + y * image->rowPitch) + x * 4;
                            const bool anchor = reference[3] == 0 || reference[3] == 1 || (alpha.preserve_translucent &&
                                std::abs(reference[3] - 173.0f / 255.0f) < 1.e-7f);
                            if ((reference[3] >= alpha.cutoff) != (pixel[3] >= alpha.cutoff) ||
                                (anchor && std::abs(reference[3] - pixel[3]) > 1.e-7f))
                                throw std::runtime_error("character BC7 repaired block failed actual codec class/anchor verification: " +
                                    path.string() + " mip=" + std::to_string(mip));
                        }
                }
            }
            if (!image || image->width != mips[mip].width || image->height != mips[mip].height ||
                image->format != DXGI_FORMAT_R32G32B32A32_FLOAT)
                throw std::runtime_error("character BC7 QA extent mismatch");
            std::size_t coverage_mismatches{};
            for (std::uint32_t y = 0; y < image->height; ++y)
                for (std::uint32_t x = 0; x < image->width; ++x)
                {
                    const auto &reference = mips[mip].pixels[std::size_t(y) * image->width + x];
                    const auto *pixel = reinterpret_cast<const float *>(image->pixels + y * image->rowPitch) + x * 4;
                    for (std::size_t channel = 0; channel < 4; ++channel)
                    {
                        const auto actual = pixel[channel];
                        const auto difference = actual - (color && channel < 3 ? CharacterSrgbToLinear(reference[channel]) : reference[channel]);
                        if (!std::isfinite(difference) || actual < 0 || actual > 1)
                            throw std::runtime_error("character BC7 decode contains an invalid channel");
                    }
                    if (semantic == CharacterTextureSemantic::NormalRoughness)
                    {
                        double dot{}, reference_length{}, actual_length{};
                        for (std::size_t channel = 0; channel < 3; ++channel)
                        {
                            const auto a = reference[channel] * 2.0 - 1.0, b = pixel[channel] * 2.0 - 1.0;
                            dot += a * b; reference_length += a * a; actual_length += b * b;
                        }
                        if (!(reference_length > 0 && actual_length > 0 && dot > 0))
                        {
                            const auto *wire = input.GetImage(mip, 0, 0);
                            const auto *rgba = wire->pixels + y * wire->rowPitch + x * 4;
                            std::ostringstream details;
                            details << std::setprecision(10)
                                << "character BC7 normal is non-normalizable or crossed its source hemisphere: " << path.string()
                                << " source=" << (source.empty() ? std::string("fallback") : source.string())
                                << " semantic=normal-roughness mip=" << mip << " xy=" << x << ',' << y
                                << " texel_index=" << (std::size_t(y) * image->width + x)
                                << " reference_rgba=" << reference[0] << ',' << reference[1] << ',' << reference[2] << ',' << reference[3]
                                << " wire_rgba=" << unsigned(rgba[0]) << ',' << unsigned(rgba[1]) << ',' << unsigned(rgba[2]) << ',' << unsigned(rgba[3])
                                << " decoded_rgba=" << pixel[0] << ',' << pixel[1] << ',' << pixel[2] << ',' << pixel[3]
                                << " reference_normal=" << (reference[0] * 2.0 - 1.0) << ',' << (reference[1] * 2.0 - 1.0) << ',' << (reference[2] * 2.0 - 1.0)
                                << " decoded_normal=" << (pixel[0] * 2.0 - 1.0) << ',' << (pixel[1] * 2.0 - 1.0) << ',' << (pixel[2] * 2.0 - 1.0)
                                << " reference_length_squared=" << reference_length << " decoded_length_squared=" << actual_length
                                << " dot=" << dot << " block_xy=" << x / 4 << ',' << y / 4;
                            throw std::runtime_error(details.str());
                        }
                    }
                    if (alpha.cutoff > 0)
                    {
                        const bool before = reference[3] >= alpha.cutoff, after = pixel[3] >= alpha.cutoff;
                        if (before != after)
                        {
                            ++coverage_mismatches;
                        }
                    }
                }
            if (alpha.cutoff > 0 && coverage_mismatches)
                throw std::runtime_error("character BC7 changed the spatial cutoff mask after local repair: " + path.string() +
                    " mip=" + std::to_string(mip));
        }
        DirectX::Blob dds;
        checked(DirectX::SaveToDDSMemory(compressed.GetImages(), compressed.GetImageCount(), compressed.GetMetadata(),
            DirectX::DDS_FLAGS_FORCE_DX10_EXT, dds), "serialize character BC7 DDS");
        const auto bytes = std::as_bytes(std::span(dds.GetBufferPointer(), dds.GetBufferSize()));
        const auto decoded = render_data::DecodeDdsTexture(bytes);
        render_data::ValidateDdsTextureRole(decoded, render_data::DdsTextureRole::CharacterFinalBc7,
            {final_size, final_size, 1, static_cast<std::uint32_t>(mips.size()), static_cast<std::uint32_t>(format)});
        return AtomicWriteCharacterFile(path, bytes, error_message);
    }
    catch (const std::exception &exception)
    {
        error_message = exception.what();
        return false;
    }
}
} // namespace
const std::string &CharacterTextureCookRecipeIdentity() { return CharacterEncoder().identity; }


bool WriteCharacterTextures(const std::filesystem::path &output,
    const CharacterIR &character, const CharacterAssetSource &source,
    std::string &error_message)
{
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    {
        error_message = "WIC factory creation failed";
        return false;
    }
    std::vector<std::filesystem::path> source_paths;
    for (const auto &material : character.materials)
    {
        source_paths.push_back(material.diffuse); source_paths.push_back(material.normal);
    }
    CharacterTextureSources sources(source_paths);
    for (std::size_t index = 0; index < character.materials.size(); ++index)
    {
        const auto suffix = std::to_string(index) + ".dds";
        const auto &material = character.materials[index];
        const bool archer = source.output_name == "archer";
        const bool family = source.output_name.starts_with("enemy_");
        const auto size = archer ? 2048u : family ? 1024u : 1u;
        const CharacterAlphaPolicy coverage{archer ? 0.2f : family ? 0.98f : 0.0f, family};
        if (!WriteDds(*factory.Get(), sources, output / (source.output_name + "_diffuse_" + suffix),
                      material.diffuse, {255, 255, 255, 255}, error_message,
                      size, CharacterTextureSemantic::Color, coverage) ||
            !WriteDds(*factory.Get(), sources, output / (source.output_name + "_normal_" + suffix),
                      material.normal, {128, 128, 255, 255}, error_message,
                      size, CharacterTextureSemantic::NormalRoughness))
        {
            return false;
        }
    }
    return true;
}

bool CookMonsterMaterialTextures(const std::filesystem::path &output,
                                 std::string &error_message,
                                 const std::filesystem::path &source)
{
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    {
        error_message = "WIC factory creation failed";
        return false;
    }
    const std::array source_paths{source / "BasecolorDefault_TEX.png", source / "Emissive_TEX.png", source / "RAM_TEX.png"};
    CharacterTextureSources sources(source_paths);
    return WriteDds(*factory.Get(), sources, output / "monster_basecolor.dds",
                    source / "BasecolorDefault_TEX.png", {255, 255, 255, 255},
                    error_message, 512, CharacterTextureSemantic::Color) &&
           WriteDds(*factory.Get(), sources, output / "monster_emissive.dds",
                    source / "Emissive_TEX.png", {0, 0, 0, 255}, error_message,
                    512, CharacterTextureSemantic::Color) &&
           WriteDds(*factory.Get(), sources, output / "monster_ram.dds",
                    source / "RAM_TEX.png", {255, 0, 0, 255}, error_message,
                    512, CharacterTextureSemantic::LinearData);
}


} // namespace hs::content
