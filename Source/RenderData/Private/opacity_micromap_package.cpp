#include <hs/render_data/opacity_micromap_package.hpp>
#include <algorithm>
#include <map>
#include <stdexcept>

namespace hs::render_data
{
namespace
{
constexpr std::size_t kHeader=192;
void Require(bool v,const char *message){if(!v)throw std::runtime_error(std::string("OMM package: ")+message);}
void Put(std::span<std::byte> bytes,std::size_t offset,std::uint64_t value,std::size_t size)
{Require(offset<=bytes.size()&&size<=bytes.size()-offset,"write range");
    for(std::size_t i=0;i<size;++i)bytes[offset+i]=std::byte((value>>(8*i))&255u);}
std::uint64_t Get(std::span<const std::byte> bytes,std::size_t offset,std::size_t size)
{Require(offset<=bytes.size()&&size<=bytes.size()-offset,"read range");std::uint64_t v{};
    for(std::size_t i=0;i<size;++i)v|=std::uint64_t(std::to_integer<unsigned>(bytes[offset+i]))<<(8*i);return v;}
void Digest(std::span<std::byte> bytes,std::size_t offset,const OpacityMicromapDigest &digest)
{std::copy(digest.begin(),digest.end(),bytes.begin()+offset);}
OpacityMicromapDigest Digest(std::span<const std::byte> bytes,std::size_t offset)
{OpacityMicromapDigest out;std::copy_n(bytes.begin()+offset,32,out.begin());return out;}
void Validate(OpacityMicromapPackageView &v)
{
    Require(v.identity.geometry_asset.value&&v.identity.triangle_count&&v.identity.triangle_count<=0x1000000u&&
        v.indices.size()==v.identity.triangle_count&&!v.descriptors.empty()&&!v.histogram.empty()&&
        !v.recipe.empty()&&v.recipe.size()<=16u*1024u*1024u&&Sha256(v.recipe)==v.identity.coverage_key,"identity/recipe/triangle contract");
    Require(v.states.size()<=256u*1024u*1024u&&v.descriptors.size()<=UINT32_MAX,"input byte range");
    Require(v.recipe.size()>=12&&Get(v.recipe,0,4)==0x31524d4f,"immutable recipe ABI");
    const auto words=Get(v.recipe,4,4);
    Require(words&&words<=(v.recipe.size()-8)/4,"actual opacity words");
    v.opacity_word_bytes=v.recipe.subspan(8,std::size_t(words)*4);
    std::map<std::uint32_t,std::uint64_t> actual_histogram;
    v.maximum_subdivision=0;
    for(const auto &d:v.descriptors)
    {
        Require(d.format==kOpacityMicromapFourStateFormat&&d.subdivision_level>=1&&d.subdivision_level<=4&&
            !(d.byte_offset%4),"four-state descriptor format/level/alignment");
        const auto bytes=((1u<<(2*d.subdivision_level))*2u+7u)/8u;
        Require(d.byte_offset<=v.states.size()&&bytes<=v.states.size()-d.byte_offset,"state descriptor range");
        ++actual_histogram[d.subdivision_level];v.maximum_subdivision=std::max(v.maximum_subdivision,std::uint32_t(d.subdivision_level));
    }
    for(const auto &h:v.histogram)
    {
        Require(h.format==kOpacityMicromapFourStateFormat&&h.count&&actual_histogram.contains(h.subdivision_level)&&
            actual_histogram.at(h.subdivision_level)==h.count,"native histogram");
        actual_histogram.erase(h.subdivision_level);
    }
    Require(actual_histogram.empty(),"missing native histogram level");
    std::uint64_t known{},unknown{};
    for(auto index:v.indices)
    {
        Require(index<v.descriptors.size(),"native triangle index");const auto &d=v.descriptors[index];
        const auto cells=1u<<(2*d.subdivision_level);
        for(std::uint32_t i=0;i<cells;++i)
        {
            const auto state=(std::to_integer<unsigned>(v.states[d.byte_offset+i/4])>>(2*(i%4)))&3u;
            (state<2?known:unknown)++;
        }
    }
    Require(known==v.known_cells&&unknown==v.unknown_cells,"actual weighted state counts");
}
}
std::vector<std::uint32_t> BuildOpacityMicromapWords(std::span<const OpacityMicromapMaterialWords> materials)
{
    Require(!materials.empty()&&materials.size()<=4096,"coverage material identities");
    std::vector<const OpacityMicromapMaterialWords*> sorted;for(const auto &m:materials)sorted.push_back(&m);
    std::sort(sorted.begin(),sorted.end(),[](const auto *a,const auto *b){return a->material_asset.value<b->material_asset.value;});
    std::vector<std::uint32_t> words{0x3157504fu,std::uint32_t(sorted.size())};std::uint64_t previous{};
    for(const auto *m:sorted)
    {
        Require(m->material_asset.value&&m->material_asset.value!=previous&&m->parameter_words.size()<=65536&&
            words.size()+12ull+m->parameter_words.size()<=4u*1024u*1024u-2,"coverage material words range/identity");
        previous=m->material_asset.value;words.push_back(std::uint32_t(previous));words.push_back(std::uint32_t(previous>>32));
        for(unsigned i=0;i<8;++i)
        {std::uint32_t value{};for(unsigned j=0;j<4;++j)value|=std::uint32_t(std::to_integer<unsigned>(m->program_layout_digest[i*4+j]))<<(j*8);words.push_back(value);}
        words.push_back(std::uint32_t(m->parameter_words.size()));words.insert(words.end(),m->parameter_words.begin(),m->parameter_words.end());
        words.push_back(m->cutoff_bits);
    }return words;
}
std::vector<std::byte> BuildOpacityMicromapRecipe(std::span<const std::uint32_t> words,
    std::span<const std::byte> proof)
{
    Require(!words.empty()&&words.size()<=UINT32_MAX&&8ull+words.size()*4ull+proof.size()<=16u*1024u*1024u,
        "recipe opacity words/proof range");
    std::vector<std::byte> bytes(8+words.size()*4+proof.size());Put(bytes,0,0x31524d4f,4);Put(bytes,4,words.size(),4);
    for(std::size_t i=0;i<words.size();++i)Put(bytes,8+i*4,words[i],4);
    std::copy(proof.begin(),proof.end(),bytes.begin()+8+words.size()*4);return bytes;
}
std::vector<std::byte> BuildOpacityMicromapPackage(AssetId asset,std::span<const std::string> dependencies,
    const OpacityMicromapIdentity &identity,std::span<const std::byte> recipe,const OpacityMicromapData &data)
{
    OpacityMicromapPackageView view;view.identity=identity;view.recipe=recipe;view.states=data.states;
    view.descriptors=data.descriptors;view.indices=data.indices;view.histogram=data.histogram;
    view.known_cells=data.known_cells;view.unknown_cells=data.unknown_cells;Validate(view);
    std::vector<std::byte> metadata(kHeader),descriptors(data.descriptors.size()*8),indices(data.indices.size()*4),histogram(data.histogram.size()*12);
    Put(metadata,0,0x314d4d4f,4);Put(metadata,4,kOpacityMicromapBakerAbi,4);Put(metadata,8,identity.triangle_count,4);
    Put(metadata,12,data.descriptors.size(),4);Put(metadata,16,data.histogram.size(),4);Put(metadata,20,view.maximum_subdivision,4);
    Put(metadata,24,data.states.size(),8);Put(metadata,32,data.known_cells,8);Put(metadata,40,data.unknown_cells,8);
    Put(metadata,48,identity.geometry_asset.value,8);Digest(metadata,56,identity.geometry_digest);Digest(metadata,88,identity.topology_digest);
    Digest(metadata,120,identity.material_digest);Digest(metadata,152,identity.coverage_key);
    for(std::size_t i=0;i<data.descriptors.size();++i)
    {const auto &d=data.descriptors[i];Put(descriptors,i*8,d.byte_offset,4);Put(descriptors,i*8+4,d.subdivision_level,2);Put(descriptors,i*8+6,d.format,2);}
    for(std::size_t i=0;i<data.indices.size();++i)Put(indices,i*4,data.indices[i],4);
    for(std::size_t i=0;i<data.histogram.size();++i)
    {const auto &h=data.histogram[i];Put(histogram,i*12,h.count,4);Put(histogram,i*12+4,h.subdivision_level,4);Put(histogram,i*12+8,h.format,4);}
    const std::array chunks{
        ChunkSource{kOmmMetadataChunk,1,true,metadata,1,kHeader},
        ChunkSource{kOmmRecipeChunk,1,true,recipe,recipe.size(),1},
        ChunkSource{kOmmStatesChunk,1,true,data.states,data.states.size(),1},
        ChunkSource{kOmmDescriptorsChunk,1,true,descriptors,data.descriptors.size(),8},
        ChunkSource{kOmmIndicesChunk,1,true,indices,data.indices.size(),4},
        ChunkSource{kOmmHistogramChunk,1,true,histogram,data.histogram.size(),12}};
    return BuildPackage(asset,dependencies,chunks);
}
OpacityMicromapPackageView ReadOpacityMicromapPackage(std::span<const std::byte> bytes,AssetId expected)
{
    const auto package=ReadPackage(bytes,expected);Require(package.chunks.size()==6,"six required chunks");
    std::array<std::span<const std::byte>,6> chunks;
    for(std::size_t i=0;i<6;++i)
    {const auto &c=package.chunks[i];Require(std::uint32_t(c.type)==kOmmMetadataChunk+i&&c.required&&c.version==1,"chunk identity");chunks[i]=c.bytes;}
    const auto metadata=chunks[0];Require(metadata.size()==kHeader&&Get(metadata,0,4)==0x314d4d4f&&
        Get(metadata,4,4)==kOpacityMicromapBakerAbi&&Get(metadata,184,8)==0,"header ABI/reserved words");
    OpacityMicromapPackageView v;v.dependencies=package.dependencies;v.identity.triangle_count=std::uint32_t(Get(metadata,8,4));
    v.identity.geometry_asset.value=Get(metadata,48,8);v.identity.geometry_digest=Digest(metadata,56);v.identity.topology_digest=Digest(metadata,88);
    v.identity.material_digest=Digest(metadata,120);v.identity.coverage_key=Digest(metadata,152);
    v.recipe=chunks[1];v.states=chunks[2];v.descriptor_bytes=chunks[3];v.index_bytes=chunks[4];
    Require(v.descriptor_bytes.size()==Get(metadata,12,4)*8&&v.index_bytes.size()==std::uint64_t(v.identity.triangle_count)*4&&
        chunks[5].size()==Get(metadata,16,4)*12&&v.states.size()==Get(metadata,24,8),"payload sizes");
    for(std::size_t i=0;i<v.descriptor_bytes.size();i+=8)v.descriptors.push_back({std::uint32_t(Get(v.descriptor_bytes,i,4)),
        std::uint16_t(Get(v.descriptor_bytes,i+4,2)),std::uint16_t(Get(v.descriptor_bytes,i+6,2))});
    for(std::size_t i=0;i<v.index_bytes.size();i+=4)v.indices.push_back(std::uint32_t(Get(v.index_bytes,i,4)));
    for(std::size_t i=0;i<chunks[5].size();i+=12)v.histogram.push_back({std::uint32_t(Get(chunks[5],i,4)),
        std::uint32_t(Get(chunks[5],i+4,4)),std::uint32_t(Get(chunks[5],i+8,4))});
    v.known_cells=Get(metadata,32,8);v.unknown_cells=Get(metadata,40,8);Validate(v);
    Require(v.maximum_subdivision==Get(metadata,20,4),"maximum subdivision");return v;
}
}
