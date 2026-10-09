#include "renderer_brixelizer_shaders.hpp"
#include <FidelityFX/host/ffx_brixelizer_raw.h>
#include <FidelityFX/host/ffx_brixelizergi.h>
#include <ffx_shader_blobs.h>

namespace hs::brixelizer_native
{
std::string ShaderProduct(std::uint32_t effect,std::uint32_t pass,std::uint32_t permutation)
{
    return "render/native/brixelizer/"+std::to_string(effect)+"/"+
        std::to_string(pass)+"/"+std::to_string(permutation)+".dxil";
}
bool EnumerateShaders(const std::function<bool(std::string_view,std::span<const std::byte>)> &consume)
{
    if(!consume) return false;
    for(auto effect:{FFX_EFFECT_BRIXELIZER,FFX_EFFECT_BRIXELIZER_GI})
    {
        const auto passes=effect==FFX_EFFECT_BRIXELIZER?std::uint32_t(FFX_BRIXELIZER_PASS_COUNT):std::uint32_t(FFX_BRIXELIZER_GI_PASS_COUNT);
        // Official options select four wave/fp16 tables. GI additionally has
        // three boolean shader definitions. Do not invent pass entry points.
        const std::uint32_t permutations=effect==FFX_EFFECT_BRIXELIZER?4u:32u;
        for(std::uint32_t pass=0;pass<std::uint32_t(passes);++pass)
            for(std::uint32_t permutation=0;permutation<permutations;++permutation)
            {
                FfxShaderBlob blob{};
                if(ffxGetPermutationBlobByIndex(effect,pass,FFX_BIND_COMPUTE_SHADER_STAGE,permutation,&blob)!=FFX_OK ||
                    !blob.data || !blob.size) return false;
                if(!consume(ShaderProduct(effect,pass,permutation),
                    {reinterpret_cast<const std::byte *>(blob.data),blob.size})) return false;
            }
    }
    return true;
}
}
