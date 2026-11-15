#include "game/vehicle.hpp"

#include <mutex>

#include "core/immortal.hpp"

namespace vehicle {
namespace {
std::mutex& g_mutex = core::Immortal<std::mutex>();
Info& g_info = core::Immortal<Info>();
}  // namespace

bool Set(const Info& info) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (info.id == g_info.id) { return false; }
    g_info = info;
    return true;
}

Info Current() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_info;
}

std::string Describe(const Info& info) {
    if (info.id.empty()) { return "?"; }
    std::string out = info.brand;
    if (!info.name.empty()) { out += (out.empty() ? "" : " ") + info.name; }
    return out.empty() ? info.id : out;
}
}  // namespace vehicle
