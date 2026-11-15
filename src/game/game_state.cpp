#include "game/game_state.hpp"

#include <atomic>
#include <cmath>

#include "core/log.hpp"

namespace game {
namespace {
std::atomic<bool> g_paused{true};  // the game starts in its menu and reports "started" when driving
std::atomic<bool> g_hasDriven{false};
std::atomic<Ignition> g_ignition{Ignition::Unknown};
std::atomic<float> g_speed{NAN};
}  // namespace

bool Paused() { return g_paused.load(std::memory_order_relaxed); }
bool HasDriven() { return g_hasDriven.load(std::memory_order_relaxed); }
Ignition IgnitionState() { return g_ignition.load(std::memory_order_relaxed); }
float SpeedMs() { return g_speed.load(std::memory_order_relaxed); }
float SpeedKmh() { return SpeedMs() * 3.6f; }

void SetPaused(bool paused) {
    g_paused.store(paused);
    logx::Log("[game] {}", paused ? "paused" : "driving");
    if (!paused && !g_hasDriven.exchange(true)) { logx::Log("[game] first drive this session"); }
}

void SetIgnition(bool on) {
    const Ignition value = on ? Ignition::On : Ignition::Off;
    if (g_ignition.exchange(value) != value) { logx::Log("[game] ignition {}", on ? "on" : "off"); }
}

void SetSpeed(float metersPerSecond) { g_speed.store(metersPerSecond, std::memory_order_relaxed); }

}  // namespace game
