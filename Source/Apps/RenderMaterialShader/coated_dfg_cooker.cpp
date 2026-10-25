#include "coated_dfg_cooker.hpp"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace hs::material_content
{
namespace
{
// Recipe identity: hs.ggx-vndf-schlick-fullcoat/v1;extent=128x128;samples=4096;
// alpha=max(r*r,0.002);moments=ABCD-fullFcNoL;sample=hammersley-base2
// (FNV-1a64 2675eb81b8ba765d). The formula mirrors
// HsBsdfAlpha/D/Smith and Schlick F in bsdf_metallic_roughness.hlsli plus
// HsBsdfCoatFresnel in bsdf_layers.hlsli. XY are the original (A,B) moments;
// ZW use the same integrand multiplied by full coat Fresnel at NoL.
constexpr std::uint32_t kExtent = 128;
constexpr std::uint32_t kSamples = 4096;
constexpr double kInvU32 = 1.0 / 4294967296.0;
constexpr double kPi = 3.141592653589793238462643383279502884;

struct Vec3
{
    double x{}, y{}, z{};
};

struct Sample
{
    double radius{}, cos_phi{}, sin_phi{};
};

struct KahanSum
{
    double value{}, error{};
    void Add(double term) noexcept
    {
        const double corrected = term - error;
        const double next = value + corrected;
        error = (next - value) - corrected;
        value = next;
    }
};

double Dot(Vec3 a, Vec3 b) noexcept { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 Cross(Vec3 a, Vec3 b) noexcept
{
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
Vec3 Normalize(Vec3 value)
{
    const double length = std::sqrt(Dot(value, value));
    if (!(length > 0.0) || !std::isfinite(length))
        throw std::runtime_error("Coated DFG VNDF produced an invalid vector");
    return {value.x/length, value.y/length, value.z/length};
}

std::uint32_t ReverseBits(std::uint32_t bits) noexcept
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xaaaaaaaau) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xccccccccu) >> 2u);
    bits = ((bits & 0x0f0f0f0fu) << 4u) | ((bits & 0xf0f0f0f0u) >> 4u);
    bits = ((bits & 0x00ff00ffu) << 8u) | ((bits & 0xff00ff00u) >> 8u);
    return bits;
}

std::array<Sample, kSamples> MakeSamples()
{
    std::array<Sample, kSamples> samples{};
    for (std::uint32_t i = 0; i < kSamples; ++i)
    {
        const double u = (static_cast<double>(i) + 0.5) / kSamples;
        const double phi = 2.0 * kPi * (ReverseBits(i) * kInvU32);
        samples[i] = {std::sqrt(u), std::cos(phi), std::sin(phi)};
    }
    return samples;
}

void PutU32(std::vector<std::byte> &bytes, std::size_t offset, std::uint32_t value)
{
    for (std::uint32_t byte = 0; byte < 4; ++byte)
        bytes[offset + byte] = std::byte((value >> (byte * 8u)) & 0xffu);
}

void PutU16(std::vector<std::byte> &bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset] = std::byte(value & 0xffu);
    bytes[offset + 1] = std::byte(value >> 8u);
}

std::uint16_t ToHalf(double value)
{
    if (!std::isfinite(value) || value < 0.0)
        throw std::runtime_error("Coated DFG coefficient is invalid");
    return DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(value));
}

std::array<double, 4> IntegrateTexel(double no_v, double roughness,
    const std::array<Sample, kSamples> &samples)
{
    const double alpha = std::max(roughness * roughness, 0.002);
    const double alpha_squared = alpha * alpha;
    const Vec3 view{std::sqrt(std::max(0.0, 1.0 - no_v * no_v)), 0.0, no_v};
    const Vec3 stretched_view = Normalize({alpha * view.x, alpha * view.y, view.z});
    const double lens_squared = stretched_view.x*stretched_view.x + stretched_view.y*stretched_view.y;
    const Vec3 tangent_one = lens_squared > 0.0
        ? Vec3{-stretched_view.y/std::sqrt(lens_squared), stretched_view.x/std::sqrt(lens_squared), 0.0}
        : Vec3{1.0, 0.0, 0.0};
    const Vec3 tangent_two = Cross(stretched_view, tangent_one);
    const double interpolation = 0.5 * (1.0 + stretched_view.z);
    std::array<KahanSum, 4> accumulated{};

    for (const auto &sample : samples)
    {
        const double disk_x = sample.radius * sample.cos_phi;
        double disk_y = sample.radius * sample.sin_phi;
        disk_y = (1.0 - interpolation) * std::sqrt(std::max(0.0, 1.0 - disk_x * disk_x)) +
            interpolation * disk_y;
        const double z = std::sqrt(std::max(0.0, 1.0 - disk_x*disk_x - disk_y*disk_y));
        const Vec3 stretched_normal{
            disk_x*tangent_one.x + disk_y*tangent_two.x + z*stretched_view.x,
            disk_x*tangent_one.y + disk_y*tangent_two.y + z*stretched_view.y,
            disk_x*tangent_one.z + disk_y*tangent_two.z + z*stretched_view.z};
        const Vec3 half = Normalize({alpha*stretched_normal.x, alpha*stretched_normal.y,
                                     std::max(0.0, stretched_normal.z)});
        const double view_half = std::clamp(Dot(view, half), 0.0, 1.0);
        if (!(view_half > 0.0)) continue;
        const Vec3 light = Normalize({2.0*view_half*half.x-view.x,
                                      2.0*view_half*half.y-view.y,
                                      2.0*view_half*half.z-view.z});
        const double no_l = light.z;
        if (!(no_l > 0.0)) continue; // Keep null VNDF samples in the denominator.

        // HsBsdfD * NoL / HsBsdfBasePdf simplifies exactly to 2*NoL/lightDenominator.
        const double light_denominator = no_l + std::sqrt(alpha_squared +
            (1.0 - alpha_squared) * no_l * no_l);
        const double weight = 2.0 * no_l / light_denominator;
        const double q_view = std::pow(1.0 - view_half, 5.0);
        const double q_light = std::pow(1.0 - no_l, 5.0);
        const double coat_fresnel = 0.04 + 0.96 * q_light;
        accumulated[0].Add(weight * (1.0 - q_view));
        accumulated[1].Add(weight * q_view);
        accumulated[2].Add(weight * (1.0 - q_view) * coat_fresnel);
        accumulated[3].Add(weight * q_view * coat_fresnel);
    }

    const double inverse_samples = 1.0 / kSamples;
    return {accumulated[0].value * inverse_samples, accumulated[1].value * inverse_samples,
            accumulated[2].value * inverse_samples, accumulated[3].value * inverse_samples};
}
} // namespace

std::vector<std::byte> CookCoatedDfgDds()
{
    constexpr std::size_t header_size = 148;
    constexpr std::size_t bytes_per_pixel = 8;
    std::vector<std::byte> bytes(header_size + std::size_t(kExtent) * kExtent * bytes_per_pixel);

    // DDS_HEADER + DX10 header: 2D RGBA16F, one array slice and one mip.
    PutU32(bytes, 0, 0x20534444u); // "DDS "
    PutU32(bytes, 4, 124u);
    PutU32(bytes, 8, 0x100fu); // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT
    PutU32(bytes, 12, kExtent);
    PutU32(bytes, 16, kExtent);
    PutU32(bytes, 20, static_cast<std::uint32_t>(kExtent * bytes_per_pixel));
    PutU32(bytes, 28, 1u);
    PutU32(bytes, 76, 32u);
    PutU32(bytes, 80, 4u); // DDPF_FOURCC
    PutU32(bytes, 84, 0x30315844u); // "DX10"
    PutU32(bytes, 108, 0x1000u); // DDSCAPS_TEXTURE
    PutU32(bytes, 128, 10u); // DXGI_FORMAT_R16G16B16A16_FLOAT
    PutU32(bytes, 132, 3u);  // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    PutU32(bytes, 140, 1u);  // arraySize

    const auto samples = MakeSamples();
    for (std::uint32_t y = 0; y < kExtent; ++y)
        for (std::uint32_t x = 0; x < kExtent; ++x)
        {
            const auto dfg = IntegrateTexel((x + 0.5) / kExtent, (y + 0.5) / kExtent, samples);
            const auto pixel = std::size_t(y) * kExtent + x;
            for (std::size_t channel = 0; channel < dfg.size(); ++channel)
                PutU16(bytes, header_size + pixel * bytes_per_pixel + channel * 2, ToHalf(dfg[channel]));
        }
    return bytes;
}
} // namespace hs::material_content
