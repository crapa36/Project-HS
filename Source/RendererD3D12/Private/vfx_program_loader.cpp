#include "hs/renderer/vfx_program_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include <limits>
#include <set>
#include <bit>

namespace hs
{
namespace
{
constexpr std::uint32_t kMagic = 0x34565348u;
constexpr std::size_t kHeaderBytes = 32;
constexpr std::size_t kDirectoryBytes = 13 * 16;
constexpr std::uint32_t kSectionCount = 13;
constexpr std::uint32_t kStrides[kSectionCount] = {68, 52, 96, 12, 40, 4, 16, 8, 12, 12, 0, 20, 4};

struct Reader
{
    std::span<const std::byte> data;
    std::size_t pos{};
    bool Read(std::uint32_t& v)
    {
        if (pos + 4 > data.size()) return false;
        v = static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[pos])) |
            (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[pos + 1])) << 8) |
            (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[pos + 2])) << 16) |
            (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[pos + 3])) << 24);
        pos += 4; return true;
    }
    bool ReadFloat(float& f) { std::uint32_t u{}; if (!Read(u)) return false; std::memcpy(&f, &u, sizeof f); return true; }
};
struct Section { std::uint32_t count{}, offset{}, bytes{}; };
bool Fail(std::string& error, const char* message) { error = message; return false; }
bool RangeOk(VfxRange r, std::size_t count) { return r.first <= count && r.count <= count - r.first; }
bool Finite(float v) { return std::isfinite(v); }
std::uint32_t Hash(std::span<const std::byte> bytes)
{
    std::uint32_t h = 2166136261u;
    for (const std::byte b : bytes) { h ^= std::to_integer<unsigned char>(b); h *= 16777619u; }
    return h;
}
template<class T> bool EnumOk(T value, std::uint32_t count) { return static_cast<std::uint32_t>(value) < count; }
}

bool LoadVfxProgram(std::span<const std::byte> bytes, VfxProgramData& output, std::string& error)
{
    error.clear();
    if (bytes.size() < kHeaderBytes) return Fail(error, "VFX program header is truncated");
    Reader header{bytes}; std::uint32_t magic{}, format{}, schema{}, engine{}, minimum{}, sections{}, payloadBytes{}, checksum{};
    if (!header.Read(magic) || !header.Read(format) || !header.Read(schema) || !header.Read(engine) ||
        !header.Read(minimum) || !header.Read(sections) || !header.Read(payloadBytes) || !header.Read(checksum))
        return Fail(error, "VFX program header is truncated");
    if (magic != kMagic || format != 5 || schema != 4 || engine != 1 || minimum > 1)
        return Fail(error, "VFX program version contract mismatch");
    if (sections != kSectionCount || payloadBytes != bytes.size() - kHeaderBytes)
        return Fail(error, "VFX program length or section count mismatch");
    const auto payload = bytes.subspan(kHeaderBytes);
    if (Hash(payload) != checksum) return Fail(error, "VFX program checksum mismatch");
    std::array<Section, kSectionCount> table{};
    Reader directory{payload}; std::uint32_t kind{};
    for (std::uint32_t i = 0; i < kSectionCount; ++i) {
        if (!directory.Read(kind) || !directory.Read(table[i].count) || !directory.Read(table[i].offset) || !directory.Read(table[i].bytes))
            return Fail(error, "VFX section directory is truncated");
        if (kind != i + 1 || table[i].offset < kDirectoryBytes || table[i].offset > payload.size() ||
            table[i].bytes > payload.size() - table[i].offset || table[i].offset + table[i].bytes > payload.size())
            return Fail(error, "VFX section directory is invalid");
        if ((i == 10 && table[i].count != table[i].bytes) || (kStrides[i] != 0 && (table[i].bytes % kStrides[i] != 0 || table[i].bytes / kStrides[i] != table[i].count)))
            return Fail(error, "VFX section stride mismatch");
        if (i && table[i - 1].offset + table[i - 1].bytes != table[i].offset)
            return Fail(error, "VFX sections are not contiguous");
        if (i == 0 && table[i].offset != kDirectoryBytes) return Fail(error, "VFX first section offset is invalid");
    }
    if (static_cast<std::size_t>(table.back().offset) + static_cast<std::size_t>(table.back().bytes) != payload.size()) return Fail(error, "VFX payload has trailing bytes");

    VfxProgramData parsed;
    auto section = [&](std::size_t i) { return payload.subspan(table[i].offset, table[i].bytes); };
    auto reserve = [&](auto& v, std::size_t n) { v.reserve(n); };
    reserve(parsed.effects, table[0].count); reserve(parsed.sources, table[1].count); reserve(parsed.outputs, table[2].count);
    reserve(parsed.parameters, table[3].count); reserve(parsed.texture_bindings, table[4].count); reserve(parsed.slice_indices, table[5].count);
    reserve(parsed.curves, table[6].count); reserve(parsed.curve_keys, table[7].count); reserve(parsed.effect_lookup, table[8].count);
    reserve(parsed.texture_resources, table[9].count); parsed.strings.assign(section(10).begin(), section(10).end());
    reserve(parsed.upgrade_bindings, table[11].count); reserve(parsed.upgrade_sequences, table[12].count);
    auto readSection = [&](std::size_t index, auto&& fn) { Reader r{section(index)}; for (std::uint32_t n=0;n<table[index].count;++n) if (!fn(r)) return false; return r.pos == table[index].bytes; };
    auto range = [](Reader& r, VfxRange& v) { return r.Read(v.first) && r.Read(v.count); };
    if (!readSection(0, [&](Reader& r) { VfxEffectRecord v{}; bool ok=r.Read(v.handle)&&r.Read(v.input_mode)&&r.Read(v.importance)&&r.Read(v.role)&&r.Read(v.style)&&r.Read(v.readability)&&r.Read(v.payload_kind)&&r.Read(v.anchor)&&r.Read(v.orientation)&&r.Read(v.scale)&&r.Read(v.binding_timing)&&r.Read(v.geometry)&&r.Read(v.must_match_logic)&&r.Read(v.timing_kind)&&r.ReadFloat(v.seconds)&&range(r,v.sources); if(ok) parsed.effects.push_back(v); return ok; })) return Fail(error,"VFX effect table is invalid");
    if (!readSection(1, [&](Reader& r) { VfxSourceRecord v{}; std::uint32_t type{}; bool ok=r.Read(v.effect)&&r.Read(v.stable_id)&&r.Read(type)&&r.Read(v.knot_count); v.type=static_cast<VfxSourceType>(type); for(float& k:v.knots)ok=ok&&r.ReadFloat(k); ok=ok&&range(r,v.parameters)&&range(r,v.outputs)&&r.Read(v.authoritative); if(ok) parsed.sources.push_back(v); return ok; })) return Fail(error,"VFX source table is invalid");
    if (!readSection(2, [&](Reader& r) { VfxOutputRecord v{}; std::uint32_t p{},motion{}; bool ok=r.Read(v.source)&&r.Read(p); v.profile=static_cast<VfxOutputProfile>(p); ok=ok&&r.Read(v.shape)&&r.Read(v.shape_domain)&&r.Read(v.shape_scale_rule)&&r.Read(v.shape_component_kind); for(float& x:v.rgba)ok=ok&&r.ReadFloat(x); ok=ok&&r.ReadFloat(v.hdr)&&r.Read(v.gradient_row)&&r.Read(v.min_quality)&&r.Read(v.coverage_type)&&r.Read(v.coverage_ref)&&r.Read(v.surface_target)&&range(r,v.parameters)&&range(r,v.textures)&&r.Read(motion)&&r.ReadFloat(v.motion_rate_hz)&&r.ReadFloat(v.motion_amplitude)&&r.ReadFloat(v.motion_inset_fraction); v.motion=static_cast<VfxMotionKind>(motion); if(ok) parsed.outputs.push_back(v); return ok; })) return Fail(error,"VFX output table is invalid");
    if (!readSection(3, [&](Reader& r) { VfxParameterRecord v{}; std::uint32_t t{}; bool ok=r.Read(v.key)&&r.Read(t)&&r.Read(v.bits); v.type=static_cast<VfxParameterType>(t); parsed.parameters.push_back(v); return ok; })) return Fail(error,"VFX parameter table is invalid");
    if (!readSection(4, [&](Reader& r) { VfxTextureBindingRecord v{}; bool ok=r.Read(v.role)&&r.Read(v.catalog_slot)&&r.Read(v.selection)&&r.Read(v.min_quality)&&r.ReadFloat(v.strength)&&range(r,v.slices)&&r.Read(v.first_frame)&&r.Read(v.frame_count)&&r.ReadFloat(v.fps); parsed.texture_bindings.push_back(v); return ok; })) return Fail(error,"VFX texture table is invalid");
    if (!readSection(5, [&](Reader& r) { std::uint32_t v{}; bool ok=r.Read(v); parsed.slice_indices.push_back(v); return ok; })) return Fail(error,"VFX slice table is invalid");
    if (!readSection(6, [&](Reader& r) { VfxCurveRecord v{}; bool ok=r.Read(v.interpolation)&&r.Read(v.domain)&&range(r,v.keys); parsed.curves.push_back(v); return ok; })) return Fail(error,"VFX curve table is invalid");
    if (!readSection(7, [&](Reader& r) { VfxCurveKey v{}; bool ok=r.ReadFloat(v.time)&&r.ReadFloat(v.value); parsed.curve_keys.push_back(v); return ok; })) return Fail(error,"VFX curve key table is invalid");
    if (!readSection(8, [&](Reader& r) { VfxEffectLookupRecord v{}; std::uint32_t lo{},hi{}; bool ok=r.Read(lo)&&r.Read(hi)&&r.Read(v.handle); v.effect_id=static_cast<std::uint64_t>(lo)|(static_cast<std::uint64_t>(hi)<<32); if(ok) parsed.effect_lookup.push_back(v); return ok; })) return Fail(error,"VFX effect lookup table is invalid");
    if (!readSection(9, [&](Reader& r) { VfxTextureResourceRecord v{}; bool ok=r.Read(v.slot)&&range(r,v.asset_path_bytes); parsed.texture_resources.push_back(v); return ok; })) return Fail(error,"VFX resource table is invalid");
    if (!readSection(11, [&](Reader& r) { VfxUpgradeBindingRecord v{}; std::uint32_t lo{},hi{}; bool ok=r.Read(lo)&&r.Read(hi)&&r.Read(v.ordinal)&&range(r,v.sequence); v.skill_id=static_cast<std::uint64_t>(lo)|(static_cast<std::uint64_t>(hi)<<32); if(ok) parsed.upgrade_bindings.push_back(v); return ok; })) return Fail(error,"VFX upgrade table is invalid");
    if (!readSection(12, [&](Reader& r) { std::uint32_t v{}; bool ok=r.Read(v); parsed.upgrade_sequences.push_back(v); return ok; })) return Fail(error,"VFX upgrade sequence table is invalid");

    for (std::size_t i=0;i<parsed.effects.size();++i) { const auto& e=parsed.effects[i]; if (!e.handle || e.handle>parsed.effects.size() || e.handle!=i+1 || e.input_mode>1 || e.importance<1 || e.importance>5 || e.must_match_logic>1 || e.timing_kind>1 || !Finite(e.seconds) || !RangeOk(e.sources,parsed.sources.size())) return Fail(error,"VFX effect relationship is invalid"); }
    for (const auto& s:parsed.sources) { if (!s.effect || s.effect>parsed.effects.size() || !EnumOk(s.type,7) || (s.knot_count!=2&&s.knot_count!=4) || !RangeOk(s.parameters,parsed.parameters.size()) || !RangeOk(s.outputs,parsed.outputs.size()) || s.authoritative>1) return Fail(error,"VFX source relationship is invalid"); for(std::size_t i=0;i<s.knot_count;++i) if(!Finite(s.knots[i]) || s.knots[i]<0 || s.knots[i]>1) return Fail(error,"VFX source knot is invalid"); }
    for (const auto& o:parsed.outputs) { if (o.source>=parsed.sources.size() || !EnumOk(o.profile,19) || !EnumOk(o.motion,75) || o.min_quality>2 || !RangeOk(o.parameters,parsed.parameters.size()) || !RangeOk(o.textures,parsed.texture_bindings.size()) || !Finite(o.hdr) || !Finite(o.motion_rate_hz) || o.motion_rate_hz<0 || o.motion_rate_hz>1000 || !Finite(o.motion_amplitude) || o.motion_amplitude<0 || o.motion_amplitude>1 || !Finite(o.motion_inset_fraction) || o.motion_inset_fraction<0 || o.motion_inset_fraction>1) return Fail(error,"VFX output relationship is invalid"); for(float x:o.rgba) if(!Finite(x)) return Fail(error,"VFX output color is invalid"); }
    for (const auto& p:parsed.parameters) if(!EnumOk(p.type,5)) return Fail(error,"VFX parameter type is invalid");
    for (const auto& t:parsed.texture_bindings) if(t.min_quality>2 || !Finite(t.strength) || !Finite(t.fps) || !RangeOk(t.slices,parsed.slice_indices.size())) return Fail(error,"VFX texture binding is invalid");
    for (const auto& c:parsed.curves) if(!RangeOk(c.keys,parsed.curve_keys.size())) return Fail(error,"VFX curve range is invalid");
    for (const auto& k:parsed.curve_keys) if(!Finite(k.time)||!Finite(k.value)) return Fail(error,"VFX curve key is invalid");
    for (std::size_t i=0;i<parsed.effect_lookup.size();++i) if(!parsed.effect_lookup[i].effect_id || !parsed.effect_lookup[i].handle || parsed.effect_lookup[i].handle>parsed.effects.size()) return Fail(error,"VFX effect lookup is invalid");
    for (const auto& r:parsed.texture_resources) if(!RangeOk(r.asset_path_bytes,parsed.strings.size())) return Fail(error,"VFX texture resource string range is invalid");
    for (std::size_t i=0;i<parsed.texture_resources.size();++i) {
        const auto &r=parsed.texture_resources[i];
        if(r.slot!=i || r.asset_path_bytes.count==0) return Fail(error,"VFX texture slot is invalid");
        const std::string path(reinterpret_cast<const char*>(parsed.strings.data()+r.asset_path_bytes.first),r.asset_path_bytes.count);
        constexpr std::string_view prefix="Content/Textures/VFX/";
        if(!path.starts_with(prefix) || !path.ends_with(".dds")) return Fail(error,"VFX texture resource path is invalid");
        const auto name=std::string_view(path).substr(prefix.size());
        if(name.size()<=4 || name.find("..")!=std::string_view::npos) return Fail(error,"VFX texture basename is invalid");
        for(const unsigned char ch:name) if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'||ch=='.')) return Fail(error,"VFX texture basename is invalid");
    }
    for (const auto& b:parsed.upgrade_bindings) if(!RangeOk(b.sequence,parsed.upgrade_sequences.size())) return Fail(error,"VFX upgrade sequence range is invalid");
    for (const auto h:parsed.upgrade_sequences) if(!h || h>parsed.effects.size()) return Fail(error,"VFX upgrade target is invalid");
    for (const auto& s:parsed.sources) { for (std::size_t i=1;i<s.knot_count;++i) if (s.knots[i] < s.knots[i-1]) return Fail(error,"VFX source knots are not monotonic"); }
    for (const auto& e:parsed.effects) for (std::uint32_t i=e.sources.first;i<e.sources.first+e.sources.count;++i) if (parsed.sources[i].effect != e.handle) return Fail(error,"VFX source effect back-reference is invalid");
    for (const auto& s:parsed.sources) for (std::uint32_t i=s.outputs.first;i<s.outputs.first+s.outputs.count;++i) if (parsed.outputs[i].source != (&s-&parsed.sources[0])) return Fail(error,"VFX output source back-reference is invalid");
    for (const auto& t:parsed.texture_bindings) if (t.catalog_slot >= parsed.texture_resources.size()) return Fail(error,"VFX texture catalog slot is invalid");
    if(parsed.effect_lookup.size()!=parsed.effects.size()) return Fail(error,"VFX effect lookup is incomplete");
    std::set<std::uint32_t> effectIds;
    for(std::size_t i=0;i<parsed.effect_lookup.size();++i) {
        const auto &e=parsed.effect_lookup[i];
        if(e.handle!=i+1 || !e.effect_id || !effectIds.insert(e.effect_id).second) return Fail(error,"VFX effect identity is duplicate or invalid");
    }
    for(const auto &p:parsed.parameters) {
        if(!p.key || (p.type==VfxParameterType::Float&&!Finite(std::bit_cast<float>(p.bits))) ||
           (p.type==VfxParameterType::Bool&&p.bits>1) || (p.type==VfxParameterType::CurveRow&&p.bits>=parsed.curves.size())) return Fail(error,"VFX typed parameter is invalid");
    }
    for(const auto &c:parsed.curves) if(c.interpolation>2) return Fail(error,"VFX curve interpolation is invalid");
    std::size_t sourceCursor=0, outputCursor=0;
    for(const auto &e:parsed.effects) {if(e.sources.first!=sourceCursor) return Fail(error,"VFX source ownership is not contiguous");sourceCursor+=e.sources.count;}
    for(const auto &source:parsed.sources) {if(source.outputs.first!=outputCursor) return Fail(error,"VFX output ownership is not contiguous");outputCursor+=source.outputs.count;}
    if(sourceCursor!=parsed.sources.size() || outputCursor!=parsed.outputs.size()) return Fail(error,"VFX program contains orphan records");
    std::set<std::pair<std::uint32_t,std::uint32_t>> upgrades;
    for(const auto &b:parsed.upgrade_bindings) if(!b.skill_id || !b.ordinal || !upgrades.emplace(b.skill_id,b.ordinal).second) return Fail(error,"VFX upgrade binding is duplicate or invalid");
    for(const auto &e:parsed.effects) {
        if(e.timing_kind==0 && e.seconds<=0) return Fail(error,"VFX fixed lifetime must be positive");
        bool hasAuthority=false;
        for(std::size_t i=e.sources.first;i<static_cast<std::size_t>(e.sources.first)+e.sources.count;++i) {
            const auto &source=parsed.sources[i];
            if(!source.authoritative) continue;
            hasAuthority=true;
            bool visibleLow=false;
            for(std::size_t j=source.outputs.first;j<static_cast<std::size_t>(source.outputs.first)+source.outputs.count;++j) visibleLow |= parsed.outputs[j].min_quality==0;
            if(!visibleLow) return Fail(error,"VFX authority is invisible at low quality");
        }
        if(e.must_match_logic && !hasAuthority) return Fail(error,"VFX gameplay geometry has no authority");
    }
    for(const auto &curve:parsed.curves) {
        if(curve.keys.count<2) return Fail(error,"VFX curve needs two keys");
        float previous=-1;
        for(std::size_t i=curve.keys.first;i<static_cast<std::size_t>(curve.keys.first)+curve.keys.count;++i) {
            const auto time=parsed.curve_keys[i].time;
            if(time<0 || time>1 || time<previous) return Fail(error,"VFX curve times are invalid");
            previous=time;
        }
    }
    output = std::move(parsed);
    return true;
}
}
