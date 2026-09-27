#include "vfx_spec_validation.hpp"
#include "vfx_texture_validation.hpp"
#include "vfx_gradient_cooker.hpp"
#include "vfx_program_cooker.hpp"
#include <hs/renderer/vfx_program_loader.hpp>
#include <span>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace
{
using Json = nlohmann::json;
void Check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
std::vector<char> Read(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read fixture");
    return {std::istreambuf_iterator<char>(stream), {}};
}
void Write(const std::filesystem::path &path, const std::vector<char> &bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("cannot write fixture");
}
template<class F> void Reject(F operation, const char *message)
{
    bool rejected = false;
    try { operation(); } catch (const std::exception &) { rejected = true; }
    Check(rejected, message);
}
}
int main()
{
    try
    {
        const std::filesystem::path root = HS_VFX_SOURCE_ROOT;
        std::ifstream stream(root / "ContentSource/VFX/production_spec.v4.json");
        const auto spec = Json::parse(stream);
        hs::content::ValidateVfxSpecification(spec);
        hs::content::ValidateVfxTextures(spec, root);
        const auto mutate = [&](auto edit, const char *message) {
            auto invalid = spec; edit(invalid);
            Reject([&] { hs::content::ValidateVfxSpecification(invalid); }, message);
        };
        mutate([](Json &s) { s["effects"][1]["id"] = s["effects"][0]["id"]; }, "duplicate effect accepted");
        mutate([](Json &s) { s["effects"][0]["visual_elements"][0]["source"]["type"] = "unknown"; }, "unknown source accepted");
        mutate([](Json &s) { s["effects"][0]["visual_elements"][0]["outputs"][0]["profile"] = "unknown"; }, "unknown profile accepted");
        mutate([](Json &s) { s["effects"][0]["visual_elements"][0]["source"]["params"]["count"] = 4; }, "wrong scoped parameter accepted");
        mutate([](Json &s) { s["effects"][0]["visual_elements"][0]["outputs"][0]["params"]["fresnel"] = "bad"; }, "wrong parameter type accepted");
        mutate([](Json &s) { s["effects"][0]["gameplay"]["authoritative"] = Json::array({"missing"}); }, "missing authoritative element accepted");
        mutate([](Json &s) { s["effects"][0]["visual_elements"][0]["outputs"][0]["min_quality"] = "high"; }, "high-only authority accepted");
        mutate([](Json &s) { s["effects"][0]["visual_elements"][0]["t"] = Json::array({.8, .2}); }, "reversed timing accepted");
        auto alias_fixture = spec;
        auto &aliases = alias_fixture["compatibility"]["legacy_aliases"];
        Check(aliases.is_array() && aliases.size() == 1, "expected production legacy alias fixture");
        hs::content::ValidateVfxSpecification(alias_fixture);
        const auto reject_alias_edit = [&](auto edit, const char *message) {
            auto invalid = alias_fixture;
            edit(invalid["compatibility"]["legacy_aliases"][0]);
            Reject([&] { hs::content::ValidateVfxSpecification(invalid); }, message);
        };
        reject_alias_edit([](Json &alias) { alias["legacy_id"] = "particle.basic_attack"; }, "legacy alias effect collision accepted");
        reject_alias_edit([](Json &alias) { alias["legacy_id"] = ""; }, "empty legacy alias accepted");
        {
            auto invalid = alias_fixture;
            invalid["compatibility"]["legacy_aliases"].push_back(alias_fixture["compatibility"]["legacy_aliases"][0]);
            Reject([&] { hs::content::ValidateVfxSpecification(invalid); }, "duplicate legacy alias record accepted");
        }
        reject_alias_edit([](Json &alias) { alias["replacement_ids"] = Json::array(); }, "empty legacy replacements accepted");
        reject_alias_edit([](Json &alias) { alias["replacement_ids"] = Json::array({"missing.effect"}); }, "missing legacy replacement accepted");
        reject_alias_edit([](Json &alias) { alias["replacement_ids"] = Json::array({alias["replacement_ids"][0], alias["replacement_ids"][0]}); }, "duplicate legacy replacement accepted");
        reject_alias_edit([](Json &alias) { alias["decision"] = "auto_route"; }, "unsupported legacy alias decision accepted");
        reject_alias_edit([](Json &alias) { alias["replacement_ids"] = Json::array({"particle.common.hit"}); }, "event-stream legacy replacement accepted");
        {
            auto invalid = alias_fixture;
            invalid["legacy_aliases"] = invalid["compatibility"]["legacy_aliases"];
            invalid["compatibility"].erase("legacy_aliases");
            Reject([&] { hs::content::ValidateVfxSpecification(invalid); }, "top-level legacy alias bypassed compatibility contract");
        }
        const auto artifacts = std::filesystem::current_path() / "Artifacts/vfx-authoring-contract";
        const auto dds = root / "ContentSource/Textures/VFX/vfx_gradient_lut.dds";
        const auto rows = hs::content::CookVfxGradientDefaults(spec, dds, artifacts / "gradient.dds");
        auto invalid_motion = spec;
        invalid_motion["effects"][0]["visual_elements"][0]["outputs"][0]["motion"] = "unknown motion";
        Reject([&] { hs::content::CookVfxProgram(invalid_motion, rows, artifacts / "invalid-motion.hsbin"); }, "unknown motion accepted by cooker");
        const auto program = hs::content::CookVfxProgram(spec, rows, artifacts / "program.hsbin");
        hs::content::CookVfxProgram(spec, rows, artifacts / "program-repeat.hsbin");
        Check(Read(artifacts / "program.hsbin") == Read(artifacts / "program-repeat.hsbin"), "program compilation is not deterministic");
        const auto binary = Read(artifacts / "program.hsbin");
        hs::VfxProgramData loaded;
        std::string load_error;
        const bool loaded_ok = hs::LoadVfxProgram(std::as_bytes(std::span(binary)), loaded, load_error);
        Check(loaded_ok, load_error.c_str());
        const auto original_effect_count = loaded.effects.size();
        auto corrupt = binary;
        corrupt.back() ^= 1;
        Check(!hs::LoadVfxProgram(std::as_bytes(std::span(corrupt)), loaded, load_error), "corrupt program accepted");
        Check(loaded.effects.size() == original_effect_count, "failed load mutated previous program");
        Check(!hs::LoadVfxProgram(std::as_bytes(std::span(binary).first(binary.size()-1)), loaded, load_error), "truncated program accepted");
        const auto word = [&](std::size_t offset) {
            Check(offset + 4 <= binary.size(), "binary word outside file");
            std::uint32_t value{};
            for (unsigned i=0;i<4;++i) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(binary[offset+i])) << (8*i);
            return value;
        };
        Check(word(0) == 0x34565348u && word(4) == 5 && word(20) == 13, "binary header mismatch");
        const auto table = [&](unsigned index) { return 32u + word(32u + index*16u + 8u); };
        Check(word(32+8*16+4)==196 && word(32+9*16+4)==13 && word(32+11*16+4)==72, "lookup/routing table counts mismatch");
        const auto hash = [](const std::string &name) { std::uint64_t h=14695981039346656037ull; for(unsigned char c:name){h^=c;h*=1099511628211ull;} return h; };
        for (const auto &effect:program.at("effects")) {
            const auto handle=effect.at("handle").get<std::uint32_t>();
            Check((static_cast<std::uint64_t>(word(table(8)+(handle-1)*12))|(static_cast<std::uint64_t>(word(table(8)+(handle-1)*12+4))<<32))==hash(effect.at("id")), "cooked effect lookup lost identity");
            Check(word(table(8)+(handle-1)*12+8)==handle, "cooked effect lookup lost handle");
        }
        for (const auto &[name,slot]:program.at("texture_catalog_slots").items()) {
            const auto record=table(9)+slot.get<unsigned>()*12;
            const auto start=table(10)+word(record+4), count=word(record+8);
            Check(start<=binary.size() && count<=binary.size()-start, "asset path outside string table");
            Check(std::string(binary.begin()+start,binary.begin()+start+count)==spec.at("libraries").at("texture_catalog").at(name).at("runtime_asset").get<std::string>(), "cooked texture path changed");
        }
        for (unsigned i=0;i<72;++i) {
            const auto &binding=spec.at("skill_upgrade_visual_bindings").at(i);
            const auto record=table(11)+i*20;
            Check((static_cast<std::uint64_t>(word(record))|(static_cast<std::uint64_t>(word(record+4))<<32))==hash(binding.at("skill_id")) && word(record+8)==binding.at("upgrade_ordinal"), "upgrade identity changed");
            Check(word(record+16)==binding.at("vfx_sequence").size(), "upgrade sequence size changed");
            for(unsigned j=0;j<word(record+16);++j) {
                const auto handle=word(table(12)+(word(record+12)+j)*4);
                Check((static_cast<std::uint64_t>(word(table(8)+(handle-1)*12))|(static_cast<std::uint64_t>(word(table(8)+(handle-1)*12+4))<<32))==hash(binding.at("vfx_sequence").at(j)), "upgrade sequence target changed");
            }
        }
        const auto reject_wire_edit = [&](std::size_t offset, std::uint32_t value, const char *message) {
            auto changed=binary;
            const auto put=[&](std::size_t at,std::uint32_t v){for(unsigned i=0;i<4;++i)changed.at(at+i)=static_cast<char>((v>>(8*i))&255);};
            put(offset,value);
            std::uint32_t checksum=2166136261u;
            for(std::size_t i=32;i<changed.size();++i){checksum^=static_cast<unsigned char>(changed[i]);checksum*=16777619u;}
            put(28,checksum);
            Check(!hs::LoadVfxProgram(std::as_bytes(std::span(changed)), loaded, load_error),message);
            Check(loaded.effects.size()==original_effect_count,"semantic rejection changed loaded program");
        };
        reject_wire_edit(table(2),0xffffffffu,"out-of-range output owner accepted");
        reject_wire_edit(table(2)+80,0xffffffffu,"invalid motion opcode accepted");
        reject_wire_edit(table(8)+12,word(table(8)),"duplicate effect identity accepted");
        reject_wire_edit(table(9)+8,0xffffffffu,"out-of-range texture path accepted");
        reject_wire_edit(table(12),0xffffffffu,"invalid upgrade target accepted");
        for(unsigned i=0;i<196;++i) if(word(table(0)+i*68+13*4)==0) {
            reject_wire_edit(table(0)+i*68+14*4,0,"zero fixed lifetime accepted");
            reject_wire_edit(table(0)+i*68+14*4,0xbf800000u,"negative fixed lifetime accepted");
            break;
        }
        reject_wire_edit(table(6)+12,0,"empty curve accepted");
        auto multi_output = spec;
        auto &shared = multi_output["effects"][0]["visual_elements"][0]["outputs"];
        const auto duplicate_output = shared[0];
        shared.push_back(duplicate_output);
        const auto multi = hs::content::CookVfxProgram(multi_output, rows, artifacts / "multi-output.hsbin");
        Check(multi.at("section_counts").at(1)==534 && multi.at("section_counts").at(2)==535, "multiple outputs duplicated their shared source");
        const auto multi_bytes = Read(artifacts / "multi-output.hsbin");
        hs::VfxProgramData multi_loaded;
        const bool multi_ok = hs::LoadVfxProgram(std::as_bytes(std::span(multi_bytes)), multi_loaded, load_error);
        Check(multi_ok, load_error.c_str());
        Check(std::count_if(multi_loaded.sources.begin(), multi_loaded.sources.end(), [](const auto &source) {return source.outputs.count==2;})==1, "shared output range lost during load");
        Check(program.at("section_counts").at(0) == 196, "program effect count mismatch");
        Check(program.at("section_counts").at(1) == 534, "program source count mismatch");
        Check(program.at("section_counts").at(2) == 534, "program output count mismatch");
        const auto stable_motion = std::find_if(loaded.outputs.begin(), loaded.outputs.end(), [](const auto &o) { return o.motion == hs::VfxMotionKind::StableBoundaryInwardPulse; });
        Check(stable_motion != loaded.outputs.end() && stable_motion->motion_rate_hz == .8f && stable_motion->motion_amplitude == .22f && stable_motion->motion_inset_fraction == .12f, "stable boundary motion contract changed");
        const auto slow_motion = std::find_if(loaded.outputs.begin(), loaded.outputs.end(), [](const auto &o) { return o.motion == hs::VfxMotionKind::SlowRadialFlow; });
        Check(slow_motion != loaded.outputs.end() && slow_motion->motion_rate_hz == 0.f && slow_motion->motion_amplitude == 0.f && slow_motion->motion_inset_fraction == 0.f, "slow radial motion contract changed");
        Check(rows.size() == 68, "default expansion must yield 68 used style-gradient pairs");
        const auto source = Read(dds), cooked = Read(artifacts / "gradient.dds");
        Check(cooked.size() == 148 + rows.size() * 2048, "gradient payload size mismatch");
        Check(std::equal(source.begin() + 148, source.end(), cooked.begin() + 148), "supplied gradient samples changed");
        Check(std::equal(spec["libraries"]["texture_catalog"]["tex.vfx.gradient_lut"]["row_map"].begin(),
            spec["libraries"]["texture_catalog"]["tex.vfx.gradient_lut"]["row_map"].end(), rows.begin()), "existing row identities changed");
        const auto again = hs::content::CookVfxGradientDefaults(spec, dds, artifacts / "gradient-again.dds");
        Check(again == rows && Read(artifacts / "gradient-again.dds") == cooked, "gradient cook is nondeterministic");
        const auto fixture = artifacts / "dds-fixture";
        const auto texture_dir = fixture / "ContentSource/Textures/VFX";
        std::filesystem::create_directories(texture_dir);
        for (const auto &[id, texture] : spec["libraries"]["texture_catalog"].items())
        {
            const auto name = std::filesystem::path(texture["runtime_asset"].get<std::string>()).filename();
            std::filesystem::copy_file(root / "ContentSource/Textures/VFX" / name, texture_dir / name,
                std::filesystem::copy_options::overwrite_existing);
        }
        const auto target = texture_dir / "vfx_noise_basis_2d.dds";
        const auto valid = Read(target);
        auto damaged = valid; damaged.pop_back(); Write(target, damaged);
        Reject([&] { hs::content::ValidateVfxTextures(spec, fixture); }, "truncated DDS accepted");
        damaged = valid; const std::uint32_t srgb = 29; std::memcpy(damaged.data() + 128, &srgb, 4); Write(target, damaged);
        Reject([&] { hs::content::ValidateVfxTextures(spec, fixture); }, "sRGB data texture accepted");
        Write(target, valid);
        hs::content::ValidateVfxTextures(spec, fixture);
        std::cout << "vfx authoring contracts passed: references, scope, authority, DDS, deterministic 68-row LUT\n";
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
