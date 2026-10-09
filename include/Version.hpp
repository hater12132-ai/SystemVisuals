#pragma once
#include <string_view>
namespace systemvisuals {
inline constexpr std::string_view Name = "SystemVisuals";
inline constexpr std::string_view Author = "hater12132-ai";
inline constexpr std::string_view Description =
    "Visual-only Levi pack: TargetHUD + Item name ESP (dropped items, FOV, no wallhack).";
inline constexpr std::string_view Version = "0.1.0";
inline constexpr std::string_view Library = "libSystemVisuals.so";
}
// keep old ns alias used by existing sources during transition
namespace bactro {
using namespace systemvisuals;
}
