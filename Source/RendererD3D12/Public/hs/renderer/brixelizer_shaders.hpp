#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <span>
#include <string_view>
namespace hs::brixelizer_native
{
// CPU-only product ID enumeration; no renderer/device/native context.
[[nodiscard]] bool EnumerateShaderProducts(const std::function<bool(std::string_view)> &);
// Build-time content producer input, exposed without SDK types.
[[nodiscard]] bool EnumerateShaderBlobs(const std::function<bool(std::string_view,std::span<const std::byte>)> &);
// Build-time producer helper: writes a private C++ table from the pinned SDK
// binding metadata. Runtime clients should use product ID enumeration only.
[[nodiscard]] bool EmitShaderMetadataCpp(std::ostream &);
}
