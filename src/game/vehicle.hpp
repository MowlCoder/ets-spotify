#pragma once
#include <string>

#include "game/vehicle_logic.hpp"

// The truck the player sits in, from the SDK "truck" configuration event.
namespace vehicle {
bool Set(const Info& info);              // game thread; true when the truck changed
Info Current();                          // any thread; empty id before the game reported a truck
std::string Describe(const Info& info);  // "Iveco S-Way", for the screen and the log
}  // namespace vehicle
