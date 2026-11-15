#pragma once
// The part of the SCS Telemetry SDK ABI the plugin uses: init/shutdown, the log, events (paused,
// started, configuration) and channels (speed, ignition). Names and layouts follow the official C
// headers (scssdk*.h), so they stay in the global namespace with the SDK's own spelling; the
// static_asserts below repeat the SDK's size checks.

#include <array>
#include <cstddef>
#include <cstdint>

#define SCSAPIFUNC __stdcall  // the SDK's calling convention for every callback and entry point

using scs_result_t = int32_t;
using scs_u32_t = uint32_t;
using scs_string_t = const char*;
using scs_context_t = void*;
using scs_event_t = uint32_t;
using scs_log_type_t = int32_t;

constexpr scs_result_t SCS_RESULT_ok = 0;
constexpr scs_result_t SCS_RESULT_unsupported = -1;
constexpr scs_result_t SCS_RESULT_generic_error = -7;

constexpr scs_log_type_t SCS_LOG_TYPE_message = 0;
constexpr scs_log_type_t SCS_LOG_TYPE_warning = 1;
constexpr scs_log_type_t SCS_LOG_TYPE_error = 2;

// SCS_MAKE_VERSION, SCS_GET_MAJOR_VERSION, SCS_GET_MINOR_VERSION of the SDK.
constexpr scs_u32_t scs_make_version(scs_u32_t major, scs_u32_t minor) {
    return (major << 16) | minor;
}
constexpr scs_u32_t scs_major_version(scs_u32_t version) { return (version >> 16) & 0xffff; }
constexpr scs_u32_t scs_minor_version(scs_u32_t version) { return version & 0xffff; }

constexpr scs_u32_t SCS_TELEMETRY_VERSION_1_00 = scs_make_version(1, 0);
constexpr scs_u32_t SCS_TELEMETRY_VERSION_1_01 = scs_make_version(1, 1);

constexpr scs_event_t SCS_TELEMETRY_EVENT_frame_start = 1;
constexpr scs_event_t SCS_TELEMETRY_EVENT_frame_end = 2;
constexpr scs_event_t SCS_TELEMETRY_EVENT_paused = 3;
constexpr scs_event_t SCS_TELEMETRY_EVENT_started = 4;
constexpr scs_event_t SCS_TELEMETRY_EVENT_configuration = 5;

using scs_log_t = void(SCSAPIFUNC*)(scs_log_type_t type, scs_string_t message);

struct scs_sdk_init_params_v100_t {
    scs_string_t game_name;
    scs_string_t game_id;
    scs_u32_t game_version;
#ifdef _WIN64
    scs_u32_t padding;
#endif
    scs_log_t log;
};

using scs_telemetry_event_callback_t = void(SCSAPIFUNC*)(scs_event_t event, const void* event_info,
                                                         scs_context_t context);
using scs_telemetry_register_for_event_t = scs_result_t(SCSAPIFUNC*)(
    scs_event_t event, scs_telemetry_event_callback_t callback, scs_context_t context);
using scs_telemetry_unregister_from_event_t = scs_result_t(SCSAPIFUNC*)(scs_event_t event);

// ---- Values and channels (scssdk_value.h, scssdk_telemetry_channel.h) ----

using scs_value_type_t = scs_u32_t;
constexpr scs_value_type_t SCS_VALUE_TYPE_bool = 1;
constexpr scs_value_type_t SCS_VALUE_TYPE_s32 = 2;
constexpr scs_value_type_t SCS_VALUE_TYPE_u32 = 3;
constexpr scs_value_type_t SCS_VALUE_TYPE_u64 = 4;
constexpr scs_value_type_t SCS_VALUE_TYPE_float = 5;
constexpr scs_value_type_t SCS_VALUE_TYPE_double = 6;
constexpr scs_value_type_t SCS_VALUE_TYPE_string = 12;

constexpr scs_u32_t SCS_U32_NIL = static_cast<scs_u32_t>(-1);  // index for non-indexed channels
constexpr scs_u32_t SCS_TELEMETRY_CHANNEL_FLAG_none = 0;

// Only the scalar and string members are declared; the union starts at offset 8 and is as large as
// the SDK's largest member (scs_value_dplacement_t, 40 bytes), so arrays of values have the SDK
// stride.
struct scs_value_t {
    scs_value_type_t type;
    scs_u32_t padding;
    union {
        uint8_t value_bool;
        int32_t value_s32;
        uint32_t value_u32;
        uint64_t value_u64;
        float value_float;
        double value_double;
        scs_string_t value_string;  // scs_value_string_t: { scs_string_t value; }
        std::array<uint8_t, 40> storage;
    };
};

// Attribute of a configuration (scssdk_value.h). The array ends with an entry whose name is null.
struct scs_named_value_t {
    scs_string_t name;
    scs_u32_t index;
#ifdef _WIN64
    scs_u32_t padding;
#endif
    scs_value_t value;
};

// event_info of SCS_TELEMETRY_EVENT_configuration (scssdk_telemetry_event.h).
struct scs_telemetry_configuration_t {
    scs_string_t id;
    const scs_named_value_t* attributes;
};

#ifdef _WIN64  // sizes checked by the SDK itself (scs_check_size)
static_assert(sizeof(scs_value_t) == 48, "scs_value_t layout");
static_assert(sizeof(scs_named_value_t) == 64 && offsetof(scs_named_value_t, value) == 16,
              "scs_named_value_t layout");
static_assert(sizeof(scs_telemetry_configuration_t) == 16, "scs_telemetry_configuration_t layout");
#endif

// Truck configuration (scssdk_telemetry_common_configs.h).
constexpr const char* SCS_TELEMETRY_CONFIG_truck = "truck";
constexpr const char* SCS_TELEMETRY_CONFIG_ATTRIBUTE_brand_id = "brand_id";
constexpr const char* SCS_TELEMETRY_CONFIG_ATTRIBUTE_brand = "brand";
constexpr const char* SCS_TELEMETRY_CONFIG_ATTRIBUTE_id = "id";
constexpr const char* SCS_TELEMETRY_CONFIG_ATTRIBUTE_name = "name";

using scs_telemetry_channel_callback_t = void(SCSAPIFUNC*)(scs_string_t name, scs_u32_t index,
                                                           const scs_value_t* value,
                                                           scs_context_t context);
using scs_telemetry_register_for_channel_t = scs_result_t(SCSAPIFUNC*)(
    scs_string_t name, scs_u32_t index, scs_value_type_t type, scs_u32_t flags,
    scs_telemetry_channel_callback_t callback, scs_context_t context);
using scs_telemetry_unregister_from_channel_t = scs_result_t(SCSAPIFUNC*)(scs_string_t name,
                                                                          scs_u32_t index,
                                                                          scs_value_type_t type);

// Truck channels used by the plugin (scssdk_telemetry_truck_common_channels.h).
constexpr const char* SCS_TELEMETRY_TRUCK_CHANNEL_speed =
    "truck.speed";  // float, m/s (negative in reverse)
constexpr const char* SCS_TELEMETRY_TRUCK_CHANNEL_electric_enabled =
    "truck.electric.enabled";  // bool, ignition

struct scs_telemetry_init_params_v100_t {
    scs_sdk_init_params_v100_t common;
    scs_telemetry_register_for_event_t register_for_event;
    scs_telemetry_unregister_from_event_t unregister_from_event;
    scs_telemetry_register_for_channel_t register_for_channel;
    scs_telemetry_unregister_from_channel_t unregister_from_channel;
};
using scs_telemetry_init_params_v101_t = scs_telemetry_init_params_v100_t;
