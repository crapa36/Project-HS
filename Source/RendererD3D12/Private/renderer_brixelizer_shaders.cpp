#include <hs/renderer/brixelizer_shaders.hpp>
#include <hs/core/sha256.hpp>
#include <FidelityFX/host/ffx_brixelizer_raw.h>
#include <FidelityFX/host/ffx_brixelizergi.h>
#include <ffx_shader_blobs.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace hs::brixelizer_native
{
namespace
{
std::string ShaderProduct(std::uint32_t effect,std::uint32_t pass,std::uint32_t permutation)
{
    return "render/native/brixelizer/"+std::to_string(effect)+"/"+
        std::to_string(pass)+"/"+std::to_string(permutation)+".dxil";
}

template<class F>
bool ForEachBlob(F &&consume)
{
    for(auto effect:{FFX_EFFECT_BRIXELIZER,FFX_EFFECT_BRIXELIZER_GI})
    {
        const auto passes=effect==FFX_EFFECT_BRIXELIZER?std::uint32_t(FFX_BRIXELIZER_PASS_COUNT):std::uint32_t(FFX_BRIXELIZER_GI_PASS_COUNT);
        // Official options select four wave/fp16 tables. GI additionally has
        // three boolean shader definitions. Do not invent pass entry points.
        const std::uint32_t permutations=effect==FFX_EFFECT_BRIXELIZER?4u:32u;
        for(std::uint32_t pass=0;pass<passes;++pass)
            for(std::uint32_t permutation=0;permutation<permutations;++permutation)
            {
                FfxShaderBlob blob{};
                if(ffxGetPermutationBlobByIndex(effect,pass,FFX_BIND_COMPUTE_SHADER_STAGE,permutation,&blob)!=FFX_OK ||
                    !blob.data || !blob.size || !consume(effect,pass,permutation,blob)) return false;
            }
    }
    return true;
}

struct Group
{
    const char *const *names;
    const std::uint32_t *slots,*counts,*spaces;
    std::uint32_t count;
};
std::array<Group,7> Groups(const FfxShaderBlob &b)
{
    return {{{b.boundConstantBufferNames,b.boundConstantBuffers,b.boundConstantBufferCounts,b.boundConstantBufferSpaces,b.cbvCount},
        {b.boundSRVTextureNames,b.boundSRVTextures,b.boundSRVTextureCounts,b.boundSRVTextureSpaces,b.srvTextureCount},
        {b.boundUAVTextureNames,b.boundUAVTextures,b.boundUAVTextureCounts,b.boundUAVTextureSpaces,b.uavTextureCount},
        {b.boundSRVBufferNames,b.boundSRVBuffers,b.boundSRVBufferCounts,b.boundSRVBufferSpaces,b.srvBufferCount},
        {b.boundUAVBufferNames,b.boundUAVBuffers,b.boundUAVBufferCounts,b.boundUAVBufferSpaces,b.uavBufferCount},
        {b.boundSamplerNames,b.boundSamplers,b.boundSamplerCounts,b.boundSamplerSpaces,b.samplerCount},
        {b.boundRTAccelerationStructureNames,b.boundRTAccelerationStructures,b.boundRTAccelerationStructureCounts,b.boundRTAccelerationStructureSpaces,b.rtAccelStructCount}}};
}
void EmitU32Array(std::ostream &out,std::string_view name,const std::uint32_t *values,std::uint32_t count)
{
    if(!count) return;
    out<<"static constexpr std::uint32_t "<<name<<"[]{";
    for(std::uint32_t i=0;i<count;++i) { if(i) out<<','; out<<values[i]; }
    out<<"};\n";
}
}

bool EnumerateShaderBlobs(const std::function<bool(std::string_view,std::span<const std::byte>)> &consume)
{
    if(!consume) return false;
    return ForEachBlob([&](std::uint32_t effect,std::uint32_t pass,std::uint32_t permutation,const FfxShaderBlob &blob)
    {
        const auto id=ShaderProduct(effect,pass,permutation);
        return consume(id,{reinterpret_cast<const std::byte *>(blob.data),blob.size});
    });
}

bool EmitShaderMetadataCpp(std::ostream &out)
{
    out<<"// Generated from the pinned FidelityFX Brixelizer SDK tables. Do not edit.\n"
        "#include \"renderer_brixelizer_shader_metadata.hpp\"\n"
        "#include <hs/renderer/brixelizer_shaders.hpp>\n"
        "#include <array>\n#include <cstddef>\n#include <cstdint>\n#include <string>\n"
        "namespace hs::brixelizer_native::detail { namespace {\n";
    std::uint32_t index{};
    const bool complete=ForEachBlob([&](std::uint32_t effect,std::uint32_t pass,std::uint32_t permutation,const FfxShaderBlob &blob)
    {
        const auto digest=hs::Sha256(std::span<const std::byte>(reinterpret_cast<const std::byte *>(blob.data),blob.size));
        const auto groups=Groups(blob);
        for(std::size_t group=0;group<groups.size();++group)
        {
            const auto &g=groups[group];
            if(g.count)
            {
                const auto base="k"+std::to_string(index)+"_"+std::to_string(group)+"_";
                out<<"static const char* "<<base<<"names[]{";
                for(std::uint32_t i=0;i<g.count;++i) {if(i)out<<',';out<<std::quoted(g.names[i]);}
                out<<"};\n";
                EmitU32Array(out,base+"slots",g.slots,g.count);
                EmitU32Array(out,base+"counts",g.counts,g.count);
                EmitU32Array(out,base+"spaces",g.spaces,g.count);
            }
        }
        out<<"static const FfxShaderBlob kBlob"<<index<<"{nullptr,"<<blob.size<<','
            <<blob.cbvCount<<','<<blob.srvTextureCount<<','<<blob.uavTextureCount<<','
            <<blob.srvBufferCount<<','<<blob.uavBufferCount<<','<<blob.samplerCount<<','<<blob.rtAccelStructCount;
        for(std::size_t group=0;group<groups.size();++group)
        {
            const auto &g=groups[group];
            const auto base="k"+std::to_string(index)+"_"+std::to_string(group)+"_";
            out<<',';
            if(g.count) out<<base<<"names"; else out<<"nullptr";
            out<<','<<(g.count?base+"slots":"nullptr")
               <<','<<(g.count?base+"counts":"nullptr")
               <<','<<(g.count?base+"spaces":"nullptr");
        }
        out<<"};\nstatic constexpr std::array<std::byte,32> kDigest"<<index<<"{";
        for(std::size_t b=0;b<digest.size();++b) {if(b)out<<',';out<<"std::byte{0x"<<std::hex<<std::setw(2)<<std::setfill('0')<<std::to_integer<unsigned>(digest[b])<<std::dec<<'}';}
        out<<"};\n";
        out<<"static constexpr std::uint32_t kEffect"<<index<<'{'<<effect<<"},kPass"<<index<<'{'<<pass
           <<"},kPermutation"<<index<<'{'<<permutation<<"};\n";
        ++index;
        return static_cast<bool>(out);
    });
    if(!complete||index!=31u*4u+19u*32u) return false;
    out<<"struct Entry {std::uint32_t effect,pass,permutation;std::string_view id;const FfxShaderBlob* blob;std::array<std::byte,32> digest;};\n"
        "static const Entry kEntries[]{\n";
    for(std::uint32_t i=0;i<index;++i)
    {
        const auto effect=i<31u*4u?FFX_EFFECT_BRIXELIZER:FFX_EFFECT_BRIXELIZER_GI;
        const auto pass=i<31u*4u?i/4u:(i-31u*4u)/32u;
        const auto permutation=i<31u*4u?i%4u:(i-31u*4u)%32u;
        out<<"{kEffect"<<i<<",kPass"<<i<<",kPermutation"<<i<<",\""<<ShaderProduct(effect,pass,permutation)
           <<"\",&kBlob"<<i<<",kDigest"<<i<<"},\n";
    }
    out<<"};\n}\n"
        "const FfxShaderBlob* FindShaderMetadata(std::uint32_t effect,std::uint32_t pass,std::uint32_t permutation,std::array<std::byte,32>& digest) noexcept {\n"
        "std::uint32_t index{};\n"
        "if(effect==kEntries[0].effect) {if(pass>=31u||permutation>=4u)return nullptr;index=pass*4u+permutation;}\n"
        "else if(effect==kEntries[124].effect) {if(pass>=19u||permutation>=32u)return nullptr;index=124u+pass*32u+permutation;}\n"
        "else return nullptr;\n"
        "digest=kEntries[index].digest;return kEntries[index].blob;\n}\n}\n"
        "namespace hs::brixelizer_native { bool EnumerateShaderProducts(const std::function<bool(std::string_view)>& consume) {\n"
        "if(!consume)return false;for(const auto& entry:detail::kEntries)if(!consume(entry.id))return false;return true;} }\n";
    return static_cast<bool>(out);
}
}
