#pragma once
#include <atomic>

namespace bactro::phase {
inline std::atomic_bool inFirstPersonHand{false};
inline std::atomic_bool outlinePass{false};
/** Set while an actor mesh is being submitted — Entity Outline reads this. */
inline std::atomic_bool entityMeshArmed{false};
} // namespace bactro::phase
