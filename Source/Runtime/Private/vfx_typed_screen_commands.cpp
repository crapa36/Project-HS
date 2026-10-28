#include "vfx_typed_screen_commands.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <string_view>

namespace hs::runtime_detail
{
namespace
{
constexpr std::uint64_t Hash64(std::string_view s)
{ std::uint64_t h=14695981039346656037ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}return h; }
constexpr auto kHit=Hash64("particle.player.hit_direction"),kHealth=Hash64("post.player.low_health_vignette");
Result Invalid(const char *message)
{ return Result::Failure(ErrorCode::InvalidArgument,"hs_vfx_screen",message); }
}
void VfxLowHealthPulseState::Advance(std::uint64_t stable,std::uint64_t owner,Tick tick,float ratio) noexcept
{
    if(stable_id!=stable||owner_id!=owner||tick<last_tick)
    {
        Clear();stable_id=stable;owner_id=owner;last_tick=entered_tick=previous_tick=tick;
    }
    if(tick>last_tick)
    {
        previous_tick=last_tick;previous_phase=phase;previous_frequency=frequency;
    }
    phase=std::fmod(phase+double(tick-last_tick)*double(frequency)/60.0,1.0);
    last_tick=tick;frequency=2.0f-ratio/0.35f;
}
double VfxLowHealthPulseState::PhaseAt(double display_tick) const noexcept
{
    if(display_tick>=last_tick)return phase;
    const double elapsed=std::max(0.0,display_tick-static_cast<double>(previous_tick));
    return std::fmod(previous_phase+elapsed*previous_frequency/60.0,1.0);
}
float VfxLowHealthPulseState::EntryFade(double display_tick) const noexcept
{
    return static_cast<float>(std::clamp((display_tick-static_cast<double>(entered_tick))/9.0,0.0,1.0));
}
bool IsVfxTypedScreenEffect(const VfxImage &image,VfxEffectHandle handle) noexcept
{
    return handle&&handle<=image.screen_effect_recipes.size()&&
        (image.screen_effect_recipes[handle-1].effect_id==kHit||image.screen_effect_recipes[handle-1].effect_id==kHealth);
}
Result BuildVfxTypedScreenCommands(const VfxImage &image,VfxEventView events,
    std::span<const VfxPersistentInput> persistent,Tick observation_tick,double display_tick,VfxQuality admission,
    VfxLowHealthPulseState &pulse,std::vector<VfxScreenInput> &output)
{
    output.clear();std::vector<VfxScreenInput> commands;commands.swap(output);auto next_pulse=pulse;
    const auto append=[&](const VfxScreenEffectRecipe &effect,const VfxContextPayload &context,
        float phase,float age,bool persistent_owner,std::uint64_t sequence,std::uint64_t owner,float entry_fade)->Result
    {
        if(!effect.route_valid||effect.outputs.first>image.screen_recipes.size()||
           effect.outputs.count>image.screen_recipes.size()-effect.outputs.first)
            return Invalid("Screen overlay output violates its prepared source or gradient contract.");
        for(std::size_t i=effect.outputs.first;i<static_cast<std::size_t>(effect.outputs.first)+effect.outputs.count;++i)
        {
            const auto &recipe=image.screen_recipes[i];
            if(recipe.min_quality>static_cast<std::uint32_t>(admission))continue;
            if(!persistent_owner&&(age<recipe.knot_start||age>=recipe.knot_end))continue;
            const float source_age=persistent_owner?age:(age-recipe.knot_start)/(recipe.knot_end-recipe.knot_start);
            VfxScreenInput v;v.event_sequence=sequence;v.owner_id=owner;v.output_index=recipe.output_index;
            v.gradient_row=recipe.gradient_row;v.shape=recipe.shape;v.color={recipe.rgb.x,recipe.rgb.y,recipe.rgb.z,1};
            v.alpha=recipe.alpha;v.hdr=recipe.hdr;v.center_clear=recipe.center_clear;v.edge_power=recipe.edge_power;
            v.source_direction={-context.direction.x,-context.direction.y,-context.direction.z};
            v.gradient_age=std::clamp(source_age,0.0f,1.0f);
            if(persistent_owner)
            {
                v.gradient_age=phase;
                v.envelope=entry_fade*(0.75f+0.25f*std::cos(2.0f*std::numbers::pi_v<float>*phase));
            }
            commands.push_back(v);
        }
        return Result::Success();
    };
    for(const auto &v:events)
    {
        if(!v.effect_handle||v.effect_handle>image.screen_effect_recipes.size())continue;
        const auto &effect=image.screen_effect_recipes[v.effect_handle-1];if(effect.effect_id!=kHit)continue;
        const auto *context=std::get_if<VfxContextPayload>(&v.payload);
        if(!v.sequence||!context||!effect.route_valid||!std::isfinite(context->direction.x)||
           !std::isfinite(context->direction.y)||!std::isfinite(context->direction.z))
            return Invalid("Screen hit requires an actual timed damage context.");
        if(display_tick<v.event_tick)continue;
        const float elapsed=float(display_tick-v.event_tick)/60.0f;if(elapsed>=effect.seconds)continue;
        if(auto r=append(effect,*context,elapsed,elapsed/effect.seconds,false,v.sequence,0,1.0f);!r)return r;
    }
    bool health_present=false;
    for(const auto &v:persistent)
    {
        if(!v.effect_handle||v.effect_handle>image.screen_effect_recipes.size())continue;
        const auto &effect=image.screen_effect_recipes[v.effect_handle-1];if(effect.effect_id!=kHealth)continue;
        const auto *context=std::get_if<VfxContextPayload>(&v.payload);
        if(!v.stable_id||!context||!context->owner_id||health_present||!effect.route_valid||
           !std::isfinite(context->ratio01)||context->ratio01<0||context->ratio01>0.35f||
           !std::isfinite(v.elapsed_seconds)||v.elapsed_seconds<0)
            return Invalid("Screen health requires its live low-health owner and tick clock.");
        health_present=true;next_pulse.Advance(v.stable_id,context->owner_id,observation_tick,context->ratio01);
        if(auto r=append(effect,*context,float(next_pulse.PhaseAt(display_tick)),v.normalized_age,true,0,
            v.stable_id,next_pulse.EntryFade(display_tick));!r)return r;
    }
    if(!health_present)next_pulse.Clear();
    pulse=next_pulse;output.swap(commands);return Result::Success();
}
}
