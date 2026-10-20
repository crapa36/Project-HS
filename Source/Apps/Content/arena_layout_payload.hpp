#pragma once

#include <hs/game_domain/arena_boundary.hpp>
#include <hs/game_domain/arena_obstacle.hpp>

#include <nlohmann/json.hpp>

#include <bit>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hs::content
{

inline constexpr std::uint32_t kArenaLayoutPayloadAbi = 1;

struct ArenaLayoutPayload
{
    float half_extent{};
    ArenaBoundary boundary{};
    std::array<ArenaObstacle2D, kArenaObstacleMaxCount> obstacles{};
    std::uint8_t obstacle_count{};
};

namespace arena_layout_detail
{

using Json = nlohmann::json;

[[noreturn]] inline void Fail(std::string_view reason)
{
    throw std::runtime_error("arena layout payload: " + std::string(reason));
}

inline void Require(bool condition, std::string_view reason)
{
    if (!condition) Fail(reason);
}

inline std::uint32_t FloatBits(float value, std::string_view reason)
{
    Require(std::isfinite(value), reason);
    return std::bit_cast<std::uint32_t>(value);
}

inline float DecodeFloat(std::uint32_t bits, std::string_view reason)
{
    const auto value = std::bit_cast<float>(bits);
    Require(std::isfinite(value), reason);
    return value;
}

inline std::uint32_t Unsigned(const Json &value, std::string_view reason)
{
    Require(value.is_number_unsigned() && value.get<std::uint64_t>() <=
                std::numeric_limits<std::uint32_t>::max(),
            reason);
    return static_cast<std::uint32_t>(value.get<std::uint64_t>());
}

inline Json Parse(std::string_view bytes)
{
    std::vector<std::unordered_set<std::string>> object_keys;
    const auto value = Json::parse(bytes, [&](int, Json::parse_event_t event, Json &parsed) {
        if (event == Json::parse_event_t::object_start)
            object_keys.emplace_back();
        else if (event == Json::parse_event_t::key)
            Require(!object_keys.empty() &&
                        object_keys.back().insert(parsed.get<std::string>()).second,
                    "duplicate object key");
        else if (event == Json::parse_event_t::object_end)
            object_keys.pop_back();
        return true;
    }, false);
    Require(!value.is_discarded() && object_keys.empty(), "invalid JSON");
    return value;
}

inline void ExactKeys(const Json &value)
{
    Require(value.is_object() && value.size() == 4, "invalid payload keys");
    for (const auto key : {"abi", "half_extent_bits", "boundary_bits", "obstacles_bits"})
        Require(value.contains(key), "missing payload key");
}

inline std::uint32_t ObstacleKind(ArenaObstacleKind kind)
{
    switch (kind)
    {
    case ArenaObstacleKind::Tree: return 0;
    case ArenaObstacleKind::Rock: return 1;
    }
    Fail("unknown obstacle kind");
}

inline ArenaObstacleKind DecodeObstacleKind(std::uint32_t kind)
{
    if (kind == 0) return ArenaObstacleKind::Tree;
    if (kind == 1) return ArenaObstacleKind::Rock;
    Fail("unknown obstacle kind");
}

} // namespace arena_layout_detail

[[nodiscard]] inline std::string EncodeArenaLayoutPayload(const ArenaLayoutPayload &payload)
{
    using namespace arena_layout_detail;
    Require(payload.half_extent > 0.0f && std::isfinite(payload.half_extent),
            "half extent must be finite and positive");
    Require(payload.boundary.count >= 3 &&
                payload.boundary.count <= kArenaBoundaryMaxPoints,
            "boundary count exceeds public capacity or is too small");
    Require(payload.obstacle_count <= kArenaObstacleMaxCount,
            "obstacle count exceeds public capacity");

    Json boundary = Json::array();
    for (std::size_t i = 0; i < payload.boundary.count; ++i)
    {
        const auto &point = payload.boundary.points[i];
        boundary.push_back(Json::array({FloatBits(point.x, "boundary x must be finite"),
                                        FloatBits(point.y, "boundary y must be finite")}));
    }
    Json obstacles = Json::array();
    for (std::size_t i = 0; i < payload.obstacle_count; ++i)
    {
        const auto &obstacle = payload.obstacles[i];
        Require(std::isfinite(obstacle.center.x) && std::isfinite(obstacle.center.y) &&
                    std::isfinite(obstacle.radius) && obstacle.radius > 0.0f,
                "obstacle center must be finite and radius must be finite and positive");
        obstacles.push_back(Json::array(
            {ObstacleKind(obstacle.kind), FloatBits(obstacle.center.x, "obstacle x must be finite"),
             FloatBits(obstacle.center.y, "obstacle y must be finite"),
             FloatBits(obstacle.radius, "obstacle radius must be finite")}));
    }

    return Json{{"abi", kArenaLayoutPayloadAbi},
                {"half_extent_bits", FloatBits(payload.half_extent, "half extent must be finite")},
                {"boundary_bits", std::move(boundary)},
                {"obstacles_bits", std::move(obstacles)}}
        .dump();
}

[[nodiscard]] inline ArenaLayoutPayload DecodeArenaLayoutPayload(std::string_view bytes)
{
    using namespace arena_layout_detail;
    const auto root = Parse(bytes);
    ExactKeys(root);
    Require(Unsigned(root.at("abi"), "ABI must be a bounded unsigned integer") ==
                kArenaLayoutPayloadAbi,
            "unsupported arena layout ABI");

    ArenaLayoutPayload payload;
    payload.half_extent = DecodeFloat(Unsigned(root.at("half_extent_bits"),
                                                "half extent bits must be a bounded unsigned integer"),
                                      "half extent must be finite");
    Require(payload.half_extent > 0.0f, "half extent must be positive");

    const auto &boundary = root.at("boundary_bits");
    Require(boundary.is_array() && boundary.size() >= 3 &&
                boundary.size() <= kArenaBoundaryMaxPoints,
            "boundary count exceeds public capacity or is too small");
    payload.boundary.count = static_cast<std::uint8_t>(boundary.size());
    for (std::size_t i = 0; i < boundary.size(); ++i)
    {
        const auto &point = boundary.at(i);
        Require(point.is_array() && point.size() == 2, "boundary point must have two bit values");
        payload.boundary.points[i] = {
            DecodeFloat(Unsigned(point.at(0), "boundary x bits must be a bounded unsigned integer"),
                        "boundary x must be finite"),
            DecodeFloat(Unsigned(point.at(1), "boundary y bits must be a bounded unsigned integer"),
                        "boundary y must be finite")};
    }

    const auto &obstacles = root.at("obstacles_bits");
    Require(obstacles.is_array() && obstacles.size() <= kArenaObstacleMaxCount,
            "obstacle count exceeds public capacity");
    payload.obstacle_count = static_cast<std::uint8_t>(obstacles.size());
    for (std::size_t i = 0; i < obstacles.size(); ++i)
    {
        const auto &entry = obstacles.at(i);
        Require(entry.is_array() && entry.size() == 4,
                "obstacle must have kind, center x/y, and radius bit values");
        auto &obstacle = payload.obstacles[i];
        obstacle.kind = DecodeObstacleKind(
            Unsigned(entry.at(0), "obstacle kind must be a bounded unsigned integer"));
        obstacle.center.x = DecodeFloat(
            Unsigned(entry.at(1), "obstacle x bits must be a bounded unsigned integer"),
            "obstacle x must be finite");
        obstacle.center.y = DecodeFloat(
            Unsigned(entry.at(2), "obstacle y bits must be a bounded unsigned integer"),
            "obstacle y must be finite");
        obstacle.radius = DecodeFloat(
            Unsigned(entry.at(3), "obstacle radius bits must be a bounded unsigned integer"),
            "obstacle radius must be finite");
        Require(obstacle.radius > 0.0f, "obstacle radius must be positive");
    }
    return payload;
}

} // namespace hs::content
