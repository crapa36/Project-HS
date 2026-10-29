#include <hs/renderer/vfx_program_loader.hpp>
#include <hs/runtime/vfx_image_io.hpp>
#include "vfx_typed_mesh_commands.hpp"
#include "vfx_profile_admission.hpp"
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace hs::runtime_detail
{
std::vector<VfxOwnedMeshInput> BuildVfxTypedMeshCommands(
    const VfxProgramData &program, std::span<const VfxPersistentInput> inputs)
{
    VfxImageData data;
    data.program = program;
    PrepareVfxMeshRecipes(program.View(), data.mesh_effect_recipes, data.mesh_recipes);
    return BuildVfxTypedMeshCommands(data.View(), inputs);
}
}

namespace
{
constexpr std::uint32_t Hash(std::string_view s)
{ std::uint32_t h=2166136261u; for(const unsigned char c:s) h=(h^c)*16777619u; return h; }
void Check(bool value, const char *message) { if(!value) throw std::runtime_error(message); }
}
int RunVfxMeshTests()
{
    try
    {
        hs::VfxProgramData p;
        hs::VfxEffectRecord effect{};
        effect.handle=1; effect.input_mode=1; effect.payload_kind=Hash("ProjectilePayload");
        effect.sources={0,1}; effect.seconds=0.45f;
        p.effects.push_back(effect);
        hs::VfxSourceRecord source{};
        source.effect=1; source.type=hs::VfxSourceType::ProjectileFollow; source.outputs={0,1};
        p.sources.push_back(source);
        hs::VfxOutputRecord output{};
        output.source=0; output.profile=hs::VfxOutputProfile::MeshEmissiveOit;
        output.shape=Hash("arrowhead_mesh"); output.motion=hs::VfxMotionKind::VelocityAlignedSharpHead;
        output.rgba={1,0.8f,0.5f,0.9f}; output.hdr=3; output.gradient_row=7; output.parameters={0,2};
        p.outputs.push_back(output);
        p.parameters.push_back({Hash("fresnel"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(0.35f)});
        p.parameters.push_back({Hash("head_to_tail_ratio"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(0.42f)});
        hs::VfxPersistentInput owner;
        owner.stable_id=91; owner.effect_handle=1; owner.normalized_age=1;
        owner.current_transform[12]=5; owner.previous_transform[12]=4;
        hs::VfxProjectilePayload payload; payload.velocity={60,0,0}; payload.hitbox_radius=0.2f;
        owner.payload=payload;
        auto commands=hs::runtime_detail::BuildVfxTypedMeshCommands(p,std::span(&owner,1));
        Check(commands.size()==1,"live projectile head vanished at authored end");
        const auto &c=commands.front();
        Check(c.owner_id==91 && c.position.x==5 && c.previous_position.x==4 && c.velocity.x==60 && c.radius==0.2f,
              "owner geometry or metres/second velocity changed");
        Check(c.mesh_index==1 && c.gradient_row==7 && c.hdr==3 && c.fresnel==0.35f && c.head_to_tail_ratio==0.42f && c.shell_thickness==0,"authored material/mesh selection lost");
        Check(hs::runtime_detail::BuildVfxTypedMeshCommands(p,{}).empty(),"despawn retained a head");
        p.outputs[0].shape=Hash("unknown_mesh");
        Check(hs::runtime_detail::BuildVfxTypedMeshCommands(p,std::span(&owner,1)).empty(),"unknown mesh substituted");
        p.outputs[0].shape=Hash("enemy_thorn_mesh");
        p.outputs[0].motion=hs::VfxMotionKind::VelocityAligned;
        p.parameters[1].bits=std::bit_cast<std::uint32_t>(0.5f);
        commands=hs::runtime_detail::BuildVfxTypedMeshCommands(p,std::span(&owner,1));
        Check(commands.size()==1 && commands[0].head_to_tail_ratio==0.5f,"enemy thorn authored ratio lost");
        p.outputs[0].shape=Hash("boss_crest_lance_mesh");
        p.parameters[1]={Hash("shell_thickness"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(0.22f)};
        p.outputs[0].motion=hs::VfxMotionKind::VelocityAlignedHeavyLance;
        p.sources[0].type=hs::VfxSourceType::BallisticCollision;
        commands=hs::runtime_detail::BuildVfxTypedMeshCommands(p,std::span(&owner,1));
        Check(commands.size()==1 && commands[0].mesh_index==6 && commands[0].position.x==5 && commands[0].shell_thickness==0.22f && commands[0].head_to_tail_ratio==0,
              "boss head must follow gameplay trajectory");
        p.outputs[0].source=4;
        Check(hs::runtime_detail::BuildVfxTypedMeshCommands(p,std::span(&owner,1)).empty(),"cross-source output accepted");
        p.outputs[0].source=0;
        auto valid_sibling=p.outputs[0];
        valid_sibling.rgba[0]=0.4f;
        valid_sibling.parameters={0,2};
        p.parameters.push_back({Hash("unexpected"),hs::VfxParameterType::Float,0});
        p.outputs[0].parameters={2,1};
        p.outputs.push_back(valid_sibling);
        p.sources[0].outputs.count=2;
        commands=hs::runtime_detail::BuildVfxTypedMeshCommands(p,std::span(&owner,1));
        Check(commands.size()==1 && commands[0].color.x==0.4f,
              "malformed mesh row suppressed or reordered a valid sibling");
        hs::VfxImageData data;
        data.program=p;
        hs::PrepareVfxMeshRecipes(data.program.View(),data.mesh_effect_recipes,data.mesh_recipes);
        auto image=data.View();
        std::vector<hs::VfxOwnedMeshInput> appended;
        appended.reserve(4);
        hs::VfxOwnedMeshInput prefix;prefix.owner_id=77;appended.push_back(prefix);
        const auto capacity=appended.capacity();
        hs::runtime_detail::AppendVfxTypedMeshCommands(image,{},appended);
        auto rejected=owner;rejected.effect_handle=0;
        hs::runtime_detail::AppendVfxTypedMeshCommands(image,std::span(&rejected,1),appended);
        Check(appended.size()==1&&appended[0].owner_id==77&&appended.capacity()==capacity,
              "empty/rejected mesh append changed prefix or capacity");
        hs::runtime_detail::AppendVfxTypedMeshCommands(image,std::span(&owner,1),appended);
        Check(appended.size() - 1 <= hs::runtime_detail::PrepareVfxEventProfileAdmission(
                  image, owner.effect_handle).persistent_mesh_rows,
              "prepared mesh reservation was smaller than actual output");
        Check(appended.size()==2&&appended[1].owner_id==owner.stable_id&&
              appended[1].position.x==owner.current_transform[12]&&appended.capacity()==capacity,
              "mesh append lost actual owner geometry or reused capacity");
        std::cout<<"typed owned mesh tests passed\n";
        return 0;
    }
    catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
