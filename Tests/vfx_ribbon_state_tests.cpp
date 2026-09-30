#include "vfx_ribbon_state.hpp"

#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{
using hs::VfxMotionKind;
using hs::VfxOutputProfile;
using hs::VfxRibbonOutputInput;
using hs::VfxRibbonSourceInput;
using hs::renderer_detail::VfxRibbonState;

void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

VfxRibbonSourceInput Source(std::uint64_t owner, std::uint32_t importance = 1)
{
    VfxRibbonSourceInput source{};
    source.owner_id = owner;
    source.effect_handle = 1;
    source.source_id = 2;
    source.importance = importance;
    source.position = {2.0f, 0.0f, 0.0f};
    source.previous_position = {0.0f, 0.0f, 0.0f};
    source.history_seconds = 0.2f;
    source.outputs.push_back({VfxOutputProfile::RibbonAdd, VfxMotionKind::HistoryRibbonTaperedFlow,
                              {1.0f, 0.5f, 0.2f, 1.0f}, 0.2f, 0.05f, 0.01f, 0.0f, 2.0f, 0.5f, 3, 0, false});
    return source;
}
}

int main()
{
    try
    {
        std::string error;
        auto source = Source(10);
        VfxRibbonState state;
        std::vector<VfxRibbonSourceInput> inputs{source};
        Check(state.Update(inputs, 10, 1, 1, 128, 0.5f, {4.0f, 5.0f, 6.0f}, error), error.c_str());
        Check(state.Capacity() == 2 && state.Updates().size() == 1 && state.Outputs().size() == 1, "initial ribbon state shape changed");
        Check(state.Updates()[0].identity[3] == 3 && std::abs(state.Updates()[0].rendered_head.x - 1.0f) < 0.001f, "ribbon reset or interpolation lost");
        Check(state.Outputs()[0].metadata[0] == 0 && state.Outputs()[0].metadata[1] == 3 && state.Outputs()[0].detail.x == 0.0f, "ribbon GPU packing changed");
        Check(std::bit_cast<std::uint32_t>(state.Outputs()[0].previous_eye.w) == 10u, "ribbon output lost exact current tick");

        Check(state.Update(inputs, 10, 1, 1, 128, 0.5f, {4.0f, 5.0f, 6.0f}, error), error.c_str());
        Check(state.Updates()[0].identity[3] == 1, "repeated render frame reset the ribbon slot");
        auto duplicate = inputs;
        duplicate.push_back(source);
        Check(!state.Update(duplicate, 11, 1, 1, 128, 0.0f, {}, error), "duplicate ribbon source accepted");

        auto two_outputs = Source(10);
        two_outputs.outputs.push_back({VfxOutputProfile::RibbonOit, VfxMotionKind::TwoPhaseNarrowWakes,
                                       {0.2f, 0.4f, 1.0f, 1.0f}, 0.1f, 0.02f, 0.0f, 0.2f, 1.0f, 0.25f, 7, 0xffffffffu, true});
        std::swap(two_outputs.outputs[0], two_outputs.outputs[1]);
        Check(state.Update(std::vector<VfxRibbonSourceInput>{two_outputs}, 12, 1, 1, 128, 1.0f, {}, error), error.c_str());
        Check(state.Updates().size() == 1 && state.Outputs().size() == 2 &&
              (state.Outputs()[0].metadata[3] & 1u) == 0u && (state.Outputs()[1].metadata[3] & 1u) == 1u &&
              state.Outputs()[1].metadata[2] == 2 && state.Outputs()[1].metadata[3] == 3 && state.Outputs()[1].material.w == 4.0f,
              "shared ribbon history outputs were duplicated or profile-sorted incorrectly");

        Check(state.Update(inputs, 13, 2, 1, 128, 0.0f, {}, error), error.c_str());
        Check(state.Updates()[0].identity[3] == 3, "session reset did not reset the reused ribbon slot");
        Check(state.Update(inputs, 14, 2, 2, 128, 0.0f, {}, error), error.c_str());
        Check(state.Updates()[0].identity[3] == 3, "catalog reset did not reset the reused ribbon slot");
        Check(state.Update(inputs, 13, 2, 2, 64, 0.0f, {}, error), error.c_str());
        Check(state.Capacity() == 1 && state.Updates()[0].identity[3] == 3, "backward tick or budget reset failed");

        auto lower = Source(30, 0);
        Check(state.Update(std::vector<VfxRibbonSourceInput>{inputs[0], lower}, 15, 3, 1, 64, 0.0f, {}, error), error.c_str());
        Check(state.DroppedSources() == 1, "ribbon dropped source count changed");

        VfxRibbonState tail;
        Check(tail.Update(inputs, 1, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(tail.Update({}, 2, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(tail.Updates().size() == 1 && tail.Updates()[0].identity[3] == 0 && tail.Outputs().size() == 1 &&
              tail.Outputs()[0].previous_eye.x == 0.0f && tail.Outputs()[0].previous_eye.y == 0.0f &&
              tail.Outputs()[0].previous_eye.z == 0.0f && std::bit_cast<std::uint32_t>(tail.Outputs()[0].previous_eye.w) == 2u,
              "retired ribbon tail was not emitted with output");
        auto replacement = Source(99);
        Check(tail.Update(std::vector<VfxRibbonSourceInput>{replacement}, 20, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(tail.DroppedSources() == 0 && tail.Updates()[0].identity[3] == 3, "retired ribbon slot was not reclaimed");

        VfxRibbonState priority;
        auto low = Source(40, 1);
        Check(priority.Update(std::vector<VfxRibbonSourceInput>{low}, 1, 1, 1, 64, 0.0f, {}, error), error.c_str());
        auto high = Source(41, 9);
        Check(priority.Update(std::vector<VfxRibbonSourceInput>{low, high}, 2, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(priority.DroppedSources() == 1 && priority.Updates().size() == 1 && priority.Updates()[0].identity[3] == 3 && priority.Outputs().size() == 1, "higher priority ribbon did not take over the full slot");
        auto duplicate_priority = std::vector<VfxRibbonSourceInput>{low, low};
        duplicate_priority[1].importance = 99;
        Check(!priority.Update(duplicate_priority, 3, 1, 1, 64, 0.0f, {}, error), "duplicate ribbon key with different importance accepted");

        auto invalid = Source(7);
        invalid.history_seconds = 1.1f;
        Check(!tail.Update(std::vector<VfxRibbonSourceInput>{invalid}, 21, 1, 1, 64, 0.0f, {}, error), "overlong ribbon history accepted");
        invalid = Source(7);
        invalid.outputs[0].gradient_row = 0xffffffffu;
        Check(!tail.Update(std::vector<VfxRibbonSourceInput>{invalid}, 21, 1, 1, 64, 0.0f, {}, error), "invalid gradient row accepted");
        invalid = Source(7);
        invalid.outputs[0].detail_slice = 8;
        Check(!tail.Update(std::vector<VfxRibbonSourceInput>{invalid}, 21, 1, 1, 64, 0.0f, {}, error), "invalid detail slice accepted");
        invalid = Source(7);
        invalid.outputs[0].motion = VfxMotionKind::HistoryRibbonTearsBackward;
        Check(!tail.Update(std::vector<VfxRibbonSourceInput>{invalid}, 21, 1, 1, 64, 0.0f, {}, error),
              "ground rails accepted a missing lane width");

        VfxRibbonState dash_state;
        auto dash = Source(76);
        dash.source_id = 123;
        dash.position = {8.0f, .025f, 3.0f};
        dash.previous_position = {2.0f, .025f, 3.0f};
        dash.control.x = 1.8f;
        dash.history_seconds = .24f;
        dash.outputs[0].motion = VfxMotionKind::HistoryRibbonTearsBackward;
        Check(dash_state.Update(std::vector{dash}, 1, 1, 1, 64, 1.0f, {}, error), error.c_str());
        Check(dash_state.Updates().size() == 1 && dash_state.Outputs().size() == 1 &&
              dash_state.Updates()[0].previous_interpolation.x == 2.0f &&
              dash_state.Updates()[0].position_duration.x == 8.0f &&
              dash_state.Outputs()[0].metadata[2] == 7 &&
              dash_state.Outputs()[0].analytic.w == 1.8f &&
              dash_state.Outputs()[0].material.y == .24f,
              "dash rail did not pack its actual segment, paired motion, or bounded history");
        dash.position.x = 10.0f;
        Check(dash_state.Update(std::vector{dash}, 2, 1, 1, 64, .5f, {}, error), error.c_str());
        Check(dash_state.Updates()[0].identity[3] == 1 &&
              dash_state.Updates()[0].position_duration.x == 10.0f &&
              dash_state.Updates()[0].rendered_head.x == 9.0f,
              "dash rail did not interpolate its growing tip from the prior tick");
        Check(dash_state.Update({}, 3, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(dash_state.Outputs().size() == 1 && dash_state.Updates()[0].identity[3] == 0,
              "dash rail did not retain its short history after owner expiry");
        Check(dash_state.Update({}, 17, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(dash_state.Outputs().empty(), "dash rail outlived its 0.24 second history");
        auto next_dash = dash;
        next_dash.owner_id = 77;
        Check(dash_state.Update(std::vector{next_dash}, 18, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(dash_state.Updates().size() == 1 && (dash_state.Updates()[0].identity[3] & 2u),
              "expired dash rail was not reset for a new owner");

        VfxRibbonState axial_state;
        auto axial = Source(8);
        axial.outputs[0].motion = VfxMotionKind::ShortLineIncomingVelocity;
        Check(axial_state.Update(std::vector<VfxRibbonSourceInput>{axial}, 1, 1, 1, 64, 0.0f, {}, error), error.c_str());
        Check(axial_state.Outputs().size() == 1 && axial_state.Outputs()[0].metadata[2] == 6 &&
              axial_state.Outputs()[0].metadata[3] == 0,
              "short incoming velocity ribbon did not receive the distinct one-strand motion code");

        VfxRibbonState analytic_state;
        auto link = Source(101);
        link.analytic = true;
        link.position = {8, 2, 3}; link.previous_position = {-2, 1, 0}; link.control = {1, 5, -1};
        link.segments = 16; link.normalized_age = .4f; link.history_seconds = 0;
        link.outputs[0].motion = VfxMotionKind::CurvedLinkTravelingHead;
        link.outputs[0].travel_pulse = true;
        auto sheath = link.outputs[0]; sheath.motion = VfxMotionKind::SoftWiderSheath;
        sheath.profile = VfxOutputProfile::RibbonOit; sheath.travel_pulse = false;
        link.outputs.push_back(sheath);
        Check(analytic_state.Update(std::vector{link}, 100, 1, 1, 64, .25f, {}, error), error.c_str());
        const auto &update = analytic_state.Updates()[0];
        Check((update.identity[3]&7u)==7u && ((update.identity[3]>>8)&255u)==16u &&
              update.rendered_head.x==8 && update.previous_interpolation.x==-2 &&
              update.control_age.y==5 && update.control_age.w==.4f,
              "analytic segment/control/age or endpoint packing incorrect");
        Check(analytic_state.Outputs().size()==2 && analytic_state.Outputs()[0].metadata[2]==4 &&
              analytic_state.Outputs()[1].metadata[2]==5 && analytic_state.Outputs()[0].analytic.x==.4f &&
              analytic_state.Outputs()[0].analytic.y==1 && analytic_state.Outputs()[1].analytic.y==0 &&
              analytic_state.Outputs()[0].analytic.z==1,"analytic shared outputs or pulse missing");
        link.control.y=7;
        Check(analytic_state.Update(std::vector{link},100,1,1,64,.8f,{},error),error.c_str());
        Check((analytic_state.Updates()[0].identity[3]&2u)==0 && analytic_state.Updates()[0].control_age.y==7,
              "same-tick analytic refresh reset history or lost changed control");
        Check(analytic_state.Update({},100,1,1,64,0,{},error),error.c_str());
        Check(analytic_state.Outputs().empty() && analytic_state.Updates().size()==1 && analytic_state.Updates()[0].identity[3]==4,
              "cancelled analytic source retained a temporal tail");
        Check(analytic_state.Update({},101,1,1,64,0,{},error),error.c_str());
        Check(analytic_state.Updates().empty(),"analytic cancellation repeated indefinitely");
        Check(analytic_state.Update(std::vector{link},102,1,1,64,0,{},error),error.c_str());
        Check((analytic_state.Updates()[0].identity[3]&2u)!=0,"reclaimed analytic slot was not reset");
        auto invalid_link=link; invalid_link.segments=64;
        Check(!analytic_state.Update(std::vector{invalid_link},103,1,1,64,0,{},error),"analytic point count exceeded fixed slot");
        invalid_link=link; invalid_link.control.x=std::numeric_limits<float>::infinity();
        Check(!analytic_state.Update(std::vector{invalid_link},103,1,1,64,0,{},error),"nonfinite analytic control accepted");
        invalid_link=link; invalid_link.normalized_age=1.1f;
        Check(!analytic_state.Update(std::vector{invalid_link},103,1,1,64,0,{},error),"out-of-range analytic owner age accepted");
        invalid_link=link; invalid_link.analytic=false;
        Check(!analytic_state.Update(std::vector{invalid_link},103,1,1,64,0,{},error),"link motion accepted as history emitter");

        std::cout << "vfx ribbon state contracts passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
