// game/vehicle_logic.hpp (the SDK truck configuration) and input/keymap.hpp (key names in the ini).

#include <windows.h>

#include <array>

#include "check.hpp"
#include "game/vehicle_logic.hpp"
#include "input/keymap.hpp"

using test::Check;

namespace {
scs_named_value_t StringAttribute(const char* name, const char* value) {
    scs_named_value_t a{};
    a.name = name;
    a.index = SCS_U32_NIL;
    a.value.type = SCS_VALUE_TYPE_string;
    a.value.value_string = value;
    return a;
}
}  // namespace

TEST_CASE("truck configuration", TruckConfiguration) {
    std::array<scs_named_value_t, 6> attributes{
        StringAttribute("brand_id", "iveco"), StringAttribute("brand", "Iveco"), {},
        StringAttribute("id", "iveco.sway"),  StringAttribute("name", "S-Way"),  {}};
    attributes[2].name = "fuel.capacity";  // a non-string attribute in between
    attributes[2].index = SCS_U32_NIL;
    attributes[2].value.type = SCS_VALUE_TYPE_float;
    attributes[2].value.value_float = 800.0f;
    const scs_telemetry_configuration_t truck{"truck", attributes.data()};
    vehicle::Info info;
    Check(vehicle::ReadTruckConfig(truck, &info) && info.id == "iveco.sway" &&
              info.brand == "Iveco" && info.name == "S-Way",
          "truck id, brand and name read across a 64-byte attribute array");

    const scs_telemetry_configuration_t trailer{"trailer", attributes.data()};
    const scs_named_value_t none{};
    const scs_telemetry_configuration_t empty{"truck", &none};
    Check(!vehicle::ReadTruckConfig(trailer, &info) && !vehicle::ReadTruckConfig(empty, &info),
          "other configurations and an empty truck (no truck) ignored");

    Check(vehicle::SectionFor("iveco.sway") == "nav.iveco.sway" &&
              vehicle::SectionFor("a b]c") == "nav.a_b_c",
          "ini section per truck");
}

TEST_CASE("key names", KeyNames) {
    Check(keymap::Parse(L"F8") == VK_F8 && keymap::Parse(L"f12") == VK_F12 &&
              keymap::Parse(L"F24") == VK_F24,
          "function keys, any case");
    Check(keymap::Parse(L"period") == VK_OEM_PERIOD && keymap::Parse(L"PAGEDOWN") == VK_NEXT &&
              keymap::Parse(L"NUMPAD5") == VK_NUMPAD5,
          "named keys");
    Check(keymap::Parse(L"A") == 'A' && keymap::Parse(L"7") == '7', "letters and digits");
    Check(keymap::Parse(L"") == 0 && keymap::Parse(L"F25") == 0 && keymap::Parse(L"FOO") == 0 &&
              keymap::Parse(L"NUMPAD10") == 0,
          "unknown names give 0");

    bool roundTrip = keymap::Name(VK_F8) == L"F8" && keymap::Name(VK_NUMPAD0) == L"NUMPAD0";
    for (const keymap::NamedKey& k : keymap::kNamedKeys) {
        roundTrip &= keymap::Parse(keymap::Name(k.vk)) == k.vk;
    }
    Check(roundTrip, "Name() and Parse() agree");
}
