#pragma once
// Which truck the player sits in, from the SDK "truck" configuration event.
// Pure code: unit-tested in tests/unit.

#include <cstring>
#include <string>

#include "game/scs_sdk.hpp"

namespace vehicle {

struct Info {
    std::string id;     // e.g. "iveco.sway"
    std::string brand;  // e.g. "Iveco"
    std::string name;   // e.g. "S-Way"
};

// Reads the "truck" configuration. False for other configurations and for an empty one (no truck).
inline bool ReadTruckConfig(const scs_telemetry_configuration_t& config, Info* out) {
    if (!config.id || std::strcmp(config.id, SCS_TELEMETRY_CONFIG_truck) != 0 ||
        !config.attributes) {
        return false;
    }

    Info info;
    for (const scs_named_value_t* a = config.attributes; a->name; ++a) {
        if (a->value.type != SCS_VALUE_TYPE_string || !a->value.value_string) { continue; }
        if (!std::strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_id)) {
            info.id = a->value.value_string;
        } else if (!std::strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_brand)) {
            info.brand = a->value.value_string;
        } else if (!std::strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_name)) {
            info.name = a->value.value_string;
        }
    }

    if (info.id.empty()) { return false; }
    *out = std::move(info);
    return true;
}

// ini section with this truck's screen settings: "nav." + id, unusual characters replaced by '_'.
inline std::string SectionFor(const std::string& id) {
    std::string out = "nav.";
    for (char c : id) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out += plain ? c : '_';
    }
    return out;
}

}  // namespace vehicle
