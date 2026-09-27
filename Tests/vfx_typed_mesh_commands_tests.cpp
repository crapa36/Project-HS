#include "vfx_typed_mesh_commands.hpp"
#include <bit>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
constexpr std::uint32_t Hash(std::string_view s)
{ std::uint32_t h=2166136261u; for(const unsigned char c:s) h=(h^c)*16777619u; return h; }
void Check(bool value, const char *message) { if(!value) throw std::runtime_error(message); }
}
int main()
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
        output.rgba={1,0.8f,0.5f,0.9f}; output.hdr=3; output.gradient_row=7; output.parameters={0,1};
        p.outputs.push_back(output);
        p.parameters.push_back({Hash("fresnel"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(0.35f)});
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
        Check(c.mesh_index==1 && c.gradient_row==7 && c.hdr==3 && c.fresnel==0.35f,"authored material/mesh selection lost");
        Check(hs::runtime_detail::BuildVfxTypedMeshCommands(p,{}).empty(),"despawn retained a head");
        p.outputs[0].shape=Hash("unknown_mesh");
        Check(hs::runtime_detail::BuildVfxTypedMeshCommands(p,std::span(&owner,1)).empty(),"unknown mesh substituted");
        p.outputs[0].shape=Hash("boss_crest_lance_mesh");
        p.outputs[0].motion=hs::VfxMotionKind::VelocityAlignedHeavyLance;
        p.sources[0].type=hs::VfxSourceType::BallisticCollision;
        commands=hs::runtime_detail::BuildVfxTypedMeshCommands(p,std::span(&owner,1));
        Check(commands.size()==1 && commands[0].mesh_index==6 && commands[0].position.x==5,
              "boss head must follow gameplay trajectory");
        p.outputs[0].source=4;
        Check(hs::runtime_detail::BuildVfxTypedMeshCommands(p,std::span(&owner,1)).empty(),"cross-source output accepted");
        std::cout<<"typed owned mesh tests passed\n";
        return 0;
    }
    catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
