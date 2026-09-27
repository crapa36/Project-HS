#include "vfx_spec_validation.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hs::content {
namespace {
using Json = nlohmann::json;
[[noreturn]] void Fail(const std::string& path, const std::string& reason) { throw std::runtime_error("VFX " + path + ": " + reason); }
void Require(bool value, const std::string& path, const std::string& reason) { if (!value) Fail(path, reason); }
void Keys(const Json& object, std::initializer_list<std::string_view> allowed, const std::string& path) {
    Require(object.is_object(), path, "expected object");
    for (const auto& [key, value] : object.items()) {
        (void)value;
        bool found = false;
        for (auto candidate : allowed) found |= key == candidate;
        Require(found, path + "." + key, "unknown field");
    }
}
std::string Text(const Json& value, const std::string& path) { Require(value.is_string() && !value.get_ref<const std::string&>().empty(), path, "expected nonempty string"); return value.get<std::string>(); }
const Json& Field(const Json& object, const char* key, const std::string& path) { Require(object.is_object() && object.contains(key), path + "." + key, "required field missing"); return object.at(key); }
const Json& Ref(const Json& library, const Json& value, const std::string& path) { auto id = Text(value,path); Require(library.is_object() && library.contains(id),path,"unresolved reference " + id); return library.at(id); }
double Number(const Json& value,const std::string& path) { Require(value.is_number(),path,"expected finite number"); double n=value.get<double>(); Require(std::isfinite(n),path,"expected finite number"); return n; }
void Finite(const Json& value,const std::string& path) { if(value.is_number()) (void)Number(value,path); else if(value.is_structured()) for(const auto& [key, child]:value.items()) Finite(child,path+"."+key); }
const std::map<std::string,std::string> sources{
    {"direct", "direct"},
    {"projectile_follow", "owner_follow"},
    {"ballistic_collision", "stateful_particle"},
    {"curl_motes", "stateless_particle"},
    {"impact_sprite", "stateless_particle"},
    {"history_ribbon", "ribbon_history"},
    {"volume_local", "local_volume"},
};
const std::map<std::string,std::pair<std::string,std::string>> profiles{
    {"sprite_sdf_add", {"VFX_ProceduralSDF", "additive"}},
    {"mesh_emissive_oit", {"VFX_MeshLitEmissive", "transparent"}},
    {"sprite_add", {"VFX_SpriteAdditiveHDR", "additive"}},
    {"ribbon_add", {"VFX_RibbonHDR", "additive"}},
    {"ground_sdf_soft", {"VFX_GroundSDF", "transparent"}},
    {"ground_sdf_add", {"VFX_GroundSDF", "additive"}},
    {"distortion", {"VFX_Distortion", "distortion"}},
    {"ground_sdf_oit", {"VFX_GroundSDF", "transparent"}},
    {"sprite_flame_oit", {"VFX_ProceduralFlame", "transparent"}},
    {"light", {"VFX_LightInjection", "lighting_injection"}},
    {"ribbon_oit", {"VFX_RibbonHDR", "transparent"}},
    {"local_volume", {"VFX_LocalVolumeRaymarch", "volume"}},
    {"decal", {"VFX_DecalSDF", "decal"}},
    {"sprite_noise_oit", {"VFX_ProceduralNoiseHDR", "transparent"}},
    {"screen_overlay", {"VFX_ScreenOverlay", "screen"}},
    {"entity_surface", {"VFX_EntityDissolve", "entity_surface_hook"}},
    {"fresnel_shell_oit", {"VFX_FresnelShell", "transparent"}},
    {"fresnel_shell_emissive", {"VFX_FresnelShell", "transparent"}},
    {"sprite_smoke_6way_oit", {"VFX_Smoke6Way", "transparent"}},
};
void Params(const Json& value,const Json& schema,const Json& catalog,const Json& curves,const std::string& path) {
    Require(value.is_object(),path,"expected parameter object");
    const auto& allowed=Field(schema,"allowed",path);
    static const std::map<std::string,std::set<std::string>> enums{
        {"anti_alias",{"fwidth"}}, {"direction",{"up","up_and_out"}}, {"falloff",{"ring"}},
        {"noise",{"fbm","3d_fbm"}}, {"sdf",{"asymmetric_star_slash","cross_plus_ring","cross_slash","star_ring"}},
        {"motion_blend",{"high_only"}}, {"spawn_domain",{"inside_binding_radius"}}};
    for(const auto& [key,v]:value.items()) {
        auto at=path+"."+key; bool found=false; for(const auto& a:allowed) found |= a==key;
        Require(found,at,"parameter not allowed in this scope");
        const auto& meta=Ref(catalog,key,at); auto type=Text(Field(meta,"type",at),at);
        if(type=="float") (void)Number(v,at);
        else if(type=="int") Require(v.is_number_integer(),at,"expected integer");
        else if(type=="bool") Require(v.is_boolean(),at,"expected boolean");
        else if(type=="curve_ref") (void)Ref(curves,v,at);
        else if(type=="float_or_enum" && v.is_number()) (void)Number(v,at);
        else if(type=="enum" || type=="enum_or_id" || type=="float_or_enum") {
            auto text=Text(v,at); auto entry=enums.find(key);
            Require(entry!=enums.end() && entry->second.contains(text),at,"unknown enum behavior " + text);
        } else Fail(at,"unknown parameter type " + type);
    }
}
void TextureReferences(const Json& output, const Json& profile, const Json& libraries, const std::string& path) {
    const auto& textures=libraries.at("texture_catalog");
    std::set<std::string> bound;
    for(const auto& [kind, values]:profile.at("texture_bindings").items())
        if(values.is_array()) for(const auto& value:values) { (void)Ref(textures,value,path); bound.insert(value.get<std::string>()); }
    const auto resource=[&](const Json& value)->const Json& { const auto& texture=Ref(textures,value,path); Require(bound.contains(Text(value,path)),path,"texture not bound by render profile"); return texture; };
    if(output.contains("coverage_source")) {
        const auto& coverage=output.at("coverage_source"); Keys(coverage,{"type","function","shape_ref","resource","resources","clip"},path+".coverage_source");
        const auto type=Text(Field(coverage,"type",path),path);
        const std::set<std::string> types{"analytic","analytic_mask","analytic_or_projected","analytic_screen","flipbook_6way","mesh_surface","none","projected_texture_mask","volume_density"};
        Require(types.contains(type),path,"unknown coverage behavior");
        for(auto key:{"function","shape_ref"}) if(coverage.contains(key)) {
            const auto id=Text(coverage.at(key),path); const bool local=output.contains("shape_inline") && output.at("shape_inline").at("id")==id;
            Require(local || libraries.at("shape_library").contains(id) || libraries.at("analytic_mask_functions").contains(id),path,"unknown analytic coverage reference");
        }
        if(coverage.contains("resource")) (void)resource(coverage.at("resource"));
        if(type=="flipbook_6way") {
            const auto& resources=Field(coverage,"resources",path); Require(resources.is_array() && resources.size()==2,path,"six-way smoke requires two resources");
            for(const auto& id:resources) { const auto& texture=resource(id); (void)Ref(texture.at("clip_map"),Field(coverage,"clip",path),path); }
        }
    }
    if(output.contains("detail_sources")) {
        const auto& details=output.at("detail_sources"); Require(details.is_array(),path,"detail sources must be array");
        for(const auto& detail:details) {
            Keys(detail,{"type","resource","selection","slice_group","strength"},path+".detail_sources");
            const auto type=Text(Field(detail,"type",path),path);
            Require(type=="authored_mask_array" || type=="texture_array" || type=="ribbon_detail_array",path,"unknown detail behavior");
            const auto& texture=resource(Field(detail,"resource",path));
            const auto& group=Field(detail,"slice_group",path); Require(group.is_array() && !group.empty(),path,"empty detail group");
            std::set<std::string> used;
            for(const auto& slice:group) { const auto name=Text(slice,path); bool found=false; for(const auto& item:texture.at("slice_map")) found |= item.at("name")==name; Require(found && used.insert(name).second,path,"unknown or duplicate detail slice"); }
            const auto selection=Text(Field(detail,"selection",path),path);
            Require(selection=="stable_seed_mod_group_size" || (selection=="stable_seed_mod_4" && group.size()==4),path,"unknown detail selection");
            const auto strength=Number(Field(detail,"strength",path),path); Require(strength>=0 && strength<=1,path,"invalid detail strength");
        }
    }
    if(output.contains("frame_motion_source")) {
        const auto& motion=output.at("frame_motion_source"); Keys(motion,{"type","resource","encoding","quality"},path+".frame_motion_source");
        Require(Field(motion,"type",path)=="flipbook_motion_vectors" && Field(motion,"quality",path)=="high",path,"invalid frame motion contract");
        (void)resource(Field(motion,"resource",path));
    }
}
void Validate(const Json& s) {
    Require(Field(s,"schema_version","$")==4,"$.schema_version","expected v4");
    Finite(s,"$");
    const auto& libraries=Field(s,"libraries","$"); const auto& engine=Field(s,"engine_contract","$");
    const auto& schemas=Field(s,"schemas","$"); const auto& parameterSchemas=Field(schemas,"parameter_schemas","$.schemas");
    const auto& catalog=Field(schemas,"parameter_catalog","$.schemas"); const auto& curves=Field(libraries,"curve_library","$.libraries");
    const auto& sourceContracts=Field(engine,"source_type_contracts","$.engine_contract");
    const auto& backends=Field(engine,"source_backend_registry","$.engine_contract");
    Require(sourceContracts.size()==7,"$.engine_contract.source_type_contracts","expected seven registered sources");
    Require(backends.size()==6,"$.engine_contract.source_backend_registry","expected six registered backends");
    for(const auto& [id,contract]:sourceContracts.items()) {
        auto at="$.engine_contract.source_type_contracts."+id; auto known=sources.find(id);
        Require(known!=sources.end(),at,"unknown source behavior");
        Require(Field(contract,"backend",at)==known->second,at,"backend does not match registered source");
        const auto& backend=Ref(backends,known->second,at); bool allowed=false;
        for(const auto& type:Field(backend,"allowed_source_types",at)) allowed |= type==id;
        Require(allowed,at,"backend does not permit source");
        (void)Ref(parameterSchemas,Field(contract,"parameter_schema",at),at);
    }
    const auto& renderProfiles=Field(libraries,"render_profiles","$.libraries");
    Require(renderProfiles.size()==19,"$.libraries.render_profiles","expected nineteen registered profiles");
    for(const auto& [id,profile]:renderProfiles.items()) {
        auto at="$.libraries.render_profiles."+id; auto known=profiles.find(id);
        Require(known!=profiles.end(),at,"unknown render profile");
        Require(Field(profile,"shader_contract",at)==known->second.first && Field(profile,"phase",at)==known->second.second,at,"shader or phase differs from registered profile");
        (void)Ref(libraries.at("shader_contracts"),profile.at("shader_contract"),at);
        (void)Ref(engine.at("render_phases"),profile.at("phase"),at);
        (void)Ref(parameterSchemas,Field(profile,"parameter_schema",at),at);
    }
    for(const auto& [id,curve]:curves.items()) {
        auto at="$.libraries.curve_library."+id; const auto& keys=Field(curve,"keys",at);
        Require(keys.is_array() && keys.size()>=2,at,"curve requires at least two keys"); double previous=-1;
        for(const auto& key:keys) { Require(key.is_array() && key.size()==2,at,"invalid curve key"); double t=Number(key[0],at); Require(t>=0 && t<=1 && t>previous,at,"curve times must increase within [0,1]"); previous=t; (void)Number(key[1],at); }
    }
    for(const auto& [id,gradient]:libraries.at("gradient_library").items()) {
        auto at="$.libraries.gradient_library."+id; const auto& stops=Field(gradient,"stops",at); double previous=-1;
        Require(stops.is_array() && stops.size()>=2,at,"gradient requires at least two stops");
        for(const auto& stop:stops) { Require(stop.is_array() && stop.size()==3,at,"invalid gradient stop"); double t=Number(stop[0],at), alpha=Number(stop[2],at); Require(t>=0 && t<=1 && t>previous && alpha>=0 && alpha<=1,at,"invalid gradient time/alpha"); previous=t; (void)Text(stop[1],at); }
    }
    const auto& effects=Field(s,"effects","$"); Require(effects.is_array() && effects.size()==196,"$.effects","expected 196 effects");
    std::set<std::string> ids;
    std::map<std::string, std::string> effectInputModes;
    for(const auto& effect:effects) {
        auto id=Text(Field(effect,"id","$.effects"),"$.effects.id"); auto at="$.effects["+id+"]";
        Require(ids.insert(id).second,at,"duplicate effect ID");
        Keys(effect,{"id","role","importance","input_mode","visual","timing","gameplay","readability","visual_elements"},at);
        Require(Field(effect,"importance",at).is_number_integer() && effect.at("importance")>=1 && effect.at("importance")<=5,at,"importance must be an integer in [1,5]");
        auto mode=Text(Field(effect,"input_mode",at),at); Require(mode=="event_stream" || mode=="persistent_snapshot",at,"invalid input mode");
        effectInputModes.emplace(id, mode);
        const auto& visual=Field(effect,"visual",at); Keys(visual,{"read","concept","style","signature_ref","signature"},at+".visual");
        const auto& style=Ref(libraries.at("style_families"),Field(visual,"style",at),at+".visual.style");
        Require(visual.contains("signature_ref")!=visual.contains("signature"),at,"exactly one signature required");
        if(visual.contains("signature_ref")) (void)Ref(libraries.at("shared_visual_signatures"),visual.at("signature_ref"),at+".visual.signature_ref");
        else { const auto& signature=visual.at("signature"); Keys(signature,{"shape","motion","material"},at+".visual.signature"); for(auto k:{"shape","motion","material"}) (void)Text(Field(signature,k,at),at); }
        const auto& timing=Field(effect,"timing",at); Keys(timing,{"source","seconds","fallback_seconds"},at+".timing");
        auto timingSource=Text(Field(timing,"source",at),at); Require(timingSource=="fixed" || timingSource=="gameplay",at,"unknown timing source");
        if(timingSource=="fixed") Require(Number(Field(timing,"seconds",at),at)>0,at,"fixed duration must be positive");
        const auto& gameplay=Field(effect,"gameplay",at); Keys(gameplay,{"binding","binding_ref","authoritative"},at+".gameplay");
        Require(gameplay.contains("binding")!=gameplay.contains("binding_ref"),at,"exactly one binding required");
        const auto& binding=gameplay.contains("binding")?gameplay.at("binding"):Ref(libraries.at("binding_profiles"),gameplay.at("binding_ref"),at+".gameplay.binding_ref");
        (void)Ref(schemas.at("binding_payload_schemas"),Field(binding,"payload_schema",at),at+".gameplay.payload_schema");
        (void)Ref(libraries.at("readability_profiles"),Field(effect,"readability",at),at+".readability");
        const auto& elements=Field(effect,"visual_elements",at); Require(elements.is_array() && !elements.empty(),at,"visual elements required");
        std::set<std::string> elementIds, lowElements;
        for(const auto& element:elements) {
            auto elementId=Text(Field(element,"id",at),at); auto ep=at+".visual_elements["+elementId+"]";
            Require(elementIds.insert(elementId).second,ep,"duplicate element ID"); Keys(element,{"id","source","outputs","t"},ep);
            if(element.contains("t")) { const auto& t=element.at("t"); Require(t.is_array() && (t.size()==2 || t.size()==4),ep+".t","expected interval or four envelope knots"); double previous=-1; for(const auto& knot:t) { double n=Number(knot,ep); Require(n>=0 && n<=1 && n>=previous,ep+".t","knots must increase within [0,1]"); previous=n; } }
            const auto& source=Field(element,"source",ep); Keys(source,{"type","params"},ep+".source"); auto type=Text(Field(source,"type",ep),ep); const auto& contract=Ref(sourceContracts,type,ep+".source.type");
            if(type=="volume_local") Require(id=="particle.boss.phase_change" || id=="particle.boss.spawn" || id=="particle.boss.death" || id=="persistent.boss.phase2_aura",ep,"local volume effect not allowlisted");
            if(source.contains("params")) Params(source.at("params"),Ref(parameterSchemas,contract.at("parameter_schema"),ep),catalog,curves,ep+".source.params");
            const auto& outputs=Field(element,"outputs",ep); Require(outputs.is_array(),ep,"outputs must be array");
            for(std::size_t oi=0;oi<outputs.size();++oi) {
                const auto& output=outputs[oi]; auto op=ep+".outputs["+std::to_string(oi)+"]";
                Keys(output,{"profile","color","hdr","alpha","motion","shape","shape_inline","params","gradient","min_quality","coverage_source","detail_sources","frame_motion_source","surface_target"},op);
                const auto& profile=Ref(renderProfiles,Field(output,"profile",op),op+".profile");
                TextureReferences(output,profile,libraries,op);
                auto quality=output.value("min_quality",std::string("low")); Require(quality=="low" || quality=="medium" || quality=="high",op,"unknown quality"); if(quality=="low") lowElements.insert(elementId);
                (void)Ref(style.at("colors"),Field(output,"color",op),op+".color");
                Require(Number(Field(output,"hdr",op),op)>=0,op,"negative HDR"); double alpha=Number(Field(output,"alpha",op),op); Require(alpha>=0 && alpha<=1,op,"alpha outside [0,1]");
                const bool shapeless = output.at("profile")=="light" || output.at("profile")=="entity_surface";
                Require(!(output.contains("shape") && output.contains("shape_inline")) && (shapeless || output.contains("shape") || output.contains("shape_inline")),op,"one shape required for geometric profiles");
                if(output.contains("shape")) (void)Ref(libraries.at("shape_library"),output.at("shape"),op+".shape");
                else if(output.contains("shape_inline")) { const auto& shape=output.at("shape_inline"); Keys(shape,{"id","domain","construction","scale_rule","component_kind","coverage_function"},op+".shape_inline"); for(auto key:{"id","domain","construction","scale_rule","component_kind"}) (void)Text(Field(shape,key,op),op); }
                (void)Ref(libraries.at("gradient_library"),output.value("gradient",std::string("energy_clean")),op+".gradient");
                if(output.contains("params")) Params(output.at("params"),Ref(parameterSchemas,profile.at("parameter_schema"),op),catalog,curves,op+".params");
            }
        }
        const auto authoritative=gameplay.value("authoritative",Json::array()); Require(authoritative.is_array(),at,"authoritative must be array");
        Require(!binding.value("must_match_logic",false) || !authoritative.empty(),at,"gameplay-bound effect requires authoritative elements");
        for(const auto& entry:authoritative) { auto name=Text(entry,at); Require(elementIds.contains(name),at+".gameplay.authoritative","unknown element " + name); Require(lowElements.contains(name),at+".gameplay.authoritative","authoritative element unavailable at low quality " + name); }
    }
    const auto& compatibility=Field(s,"compatibility","$");
    const auto& aliases=Field(compatibility,"legacy_aliases","$.compatibility");
    {
        Require(aliases.is_array(),"$.compatibility.legacy_aliases","expected array");
        std::set<std::string> legacyIds;
        for(std::size_t index=0; index<aliases.size(); ++index) {
            const auto& alias=aliases[index];
            const auto at="$.compatibility.legacy_aliases["+std::to_string(index)+"]";
            Keys(alias,{"legacy_id","replacement_ids","decision","reason"},at);
            const auto legacyId=Text(Field(alias,"legacy_id",at),at);
            Require(!ids.contains(legacyId),at+".legacy_id","legacy ID collides with current effect ID");
            Require(legacyIds.insert(legacyId).second,at+".legacy_id","duplicate legacy ID");
            const auto decision=Text(Field(alias,"decision",at),at);
            Require(decision=="remove_after_reference_scan",at+".decision","unsupported alias decision");
            (void)Text(Field(alias,"reason",at),at);
            const auto& replacements=Field(alias,"replacement_ids",at);
            Require(replacements.is_array() && !replacements.empty(),at+".replacement_ids","at least one replacement is required");
            std::set<std::string> replacementIds;
            for(const auto& replacement:replacements) {
                const auto replacementId=Text(replacement,at+".replacement_ids");
                Require(replacementIds.insert(replacementId).second,at+".replacement_ids","duplicate replacement ID");
                const auto found=effectInputModes.find(replacementId);
                Require(found!=effectInputModes.end(),at+".replacement_ids","unresolved effect " + replacementId);
                Require(found->second=="persistent_snapshot",at+".replacement_ids","remove_after_reference_scan requires persistent_snapshot replacements");
            }
        }
    }
    const auto& bindings=Field(s,"skill_upgrade_visual_bindings","$"); Require(bindings.is_array() && bindings.size()==72,"$.skill_upgrade_visual_bindings","expected 72 upgrade bindings"); std::set<std::string> bindingIds;
    for(const auto& binding:bindings) { auto at=std::string("$.skill_upgrade_visual_bindings"); Keys(binding,{"skill_id","upgrade_ordinal","display_name","vfx_sequence","visual_behavior"},at); auto key=Text(Field(binding,"skill_id",at),at)+":"+Field(binding,"upgrade_ordinal",at).dump(); Require(bindingIds.insert(key).second,at,"duplicate skill/ordinal"); const auto& sequence=Field(binding,"vfx_sequence",at); Require(sequence.is_array(),at,"VFX sequence must be an array"); for(const auto& item:sequence) Require(ids.contains(Text(item,at)),at,"unresolved effect " + item.dump()); }
}
}
void ValidateVfxSpecification(const nlohmann::json& specification) {
    try { Validate(specification); } catch(const nlohmann::json::exception& error) { throw std::runtime_error(std::string("VFX $: invalid structure: ")+error.what()); }
}
}
