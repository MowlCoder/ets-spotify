#pragma once

// What the game reports through the SCS telemetry SDK: one place, written by the SDK callbacks on
// the game thread (plugin.cpp) and read by any thread. All calls are cheap atomic accesses.
namespace game {

enum class Ignition { Unknown, Off, On };

bool Paused();     // menus and loading screens; true until the player first drives
bool HasDriven();  // the player has been in the truck at least once this session
Ignition IgnitionState();
float SpeedMs();  // truck speed in m/s (negative in reverse); NaN until the game reports it
float SpeedKmh();

// Telemetry input (game thread).
void SetPaused(bool paused);
void SetIgnition(bool on);
void SetSpeed(float metersPerSecond);

}  // namespace game
