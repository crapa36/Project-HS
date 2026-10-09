#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
namespace hs::brixelizer_native
{
[[nodiscard]] std::string ShaderProduct(std::uint32_t effect,std::uint32_t pass,std::uint32_t permutation);
// CPU-only official table enumeration; no renderer/device/native context.
[[nodiscard]] bool EnumerateShaders(const std::function<bool(std::string_view,std::span<const std::byte>)> &);
}
