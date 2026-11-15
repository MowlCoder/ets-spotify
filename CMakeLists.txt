cmake_minimum_required(VERSION 3.25)
project(ets_spotify VERSION 0.1.0 LANGUAGES CXX)

# Everything is in CMakePresets.json:
#   cmake --workflow --preset default          configure, build (Release) and run the tests
#   cmake --install build --config Release     copy the plugin into the game (found via Steam)

# Static CRT: the plugin must not depend on the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
add_compile_options(
    /permissive- /utf-8 /W4 /WX
    # The extra warnings recommended by cpp-best-practices (02-Use_the_Tools_Available).
    /w14242 /w14254 /w14263 /w14265 /w14287 /we4289 /w14296 /w14311 /w14545 /w14546 /w14547 /w14549
    /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928
    # Windows SDK and standard library headers are not ours to fix.
    /external:anglebrackets /external:W0
)

# The language standard, per target: everything links ets_options (ets_core passes it on).
add_library(ets_options INTERFACE)
target_compile_features(ets_options INTERFACE cxx_std_20)

option(ETS_SPOTIFY_TESTS "Build and register the unit tests and the offline harness" ON)
option(ETS_SPOTIFY_ANALYZE "Run the MSVC static analyzer (/analyze) during the build" OFF)
option(ETS_SPOTIFY_ASAN "Build with AddressSanitizer (for the tests)" OFF)

if(ETS_SPOTIFY_ANALYZE)
    add_compile_options(/analyze /analyze:external-)
endif()
if(ETS_SPOTIFY_ASAN)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO /DEBUG)
endif()

# ---- Code that runs without the game: shared by the plugin and the unit tests ----

add_library(ets_core STATIC
    src/core/config.cpp
    src/core/http.cpp
    src/core/json.cpp
    src/core/log.cpp
    src/core/pkce.cpp
    src/core/text.cpp
    src/core/worker.cpp
)
target_include_directories(ets_core PUBLIC src)
target_compile_definitions(ets_core PUBLIC ETS_SPOTIFY_VERSION="${PROJECT_VERSION}")
target_link_libraries(ets_core PUBLIC ets_options winhttp bcrypt crypt32 ws2_32 shell32 ole32 user32)

# ---- The plugin: <game>\bin\win_x64\plugins\ets_spotify.dll ----

add_library(ets_spotify SHARED
    src/plugin.cpp
    src/game/game_state.cpp
    src/game/vehicle.cpp
    src/game/test_exports.cpp
    src/d3d/hooks.cpp
    src/screen/nav_screen.cpp
    src/screen/screen_profile.cpp
    src/screen/calibration.cpp
    src/ui/ui_renderer.cpp
    src/ui/painter.cpp
    src/ui/texts.cpp
    src/spotify/spotify.cpp
    src/spotify/auth.cpp
    src/spotify/token_store.cpp
    src/spotify/web_api.cpp
    src/cabin/cabin.cpp
    src/cabin/spotify_mixer.cpp
    src/input/input.cpp
    src/diagnostics/screen_probe.cpp
    src/diagnostics/pattern.cpp
    src/diagnostics/capture.cpp
)
target_link_libraries(ets_spotify PRIVATE ets_core d3d11 dxgi d2d1 dwrite windowscodecs)

# ---- Tests: ctest --preset release ----

if(ETS_SPOTIFY_TESTS)
    enable_testing()

    # Pure logic: JSON, PKCE, URL helpers, the OAuth listener, screen detection, settings parsing...
    add_executable(unit_tests
        tests/unit/main.cpp
        tests/unit/test_core.cpp
        tests/unit/test_cabin.cpp
        tests/unit/test_screen.cpp
        tests/unit/test_spotify.cpp
        tests/unit/test_game.cpp
        tests/unit/test_ui.cpp
    )
    target_link_libraries(unit_tests PRIVATE ets_core)
    add_test(NAME unit COMMAND unit_tests)

    # The plugin inside a fake "game" with its own D3D11 device (see tests/harness.cpp). It writes its
    # log, settings and picture dumps to build/test-out.
    add_executable(probe_harness tests/harness.cpp)
    target_include_directories(probe_harness PRIVATE src)
    target_link_libraries(probe_harness PRIVATE ets_options d3d11 dxgi user32)
    add_dependencies(probe_harness ets_spotify)
    set(HARNESS_OUT "${CMAKE_BINARY_DIR}/test-out")
    file(MAKE_DIRECTORY "${HARNESS_OUT}")
    add_test(NAME harness COMMAND probe_harness $<TARGET_FILE:ets_spotify> "${HARNESS_OUT}")
    set_tests_properties(harness PROPERTIES TIMEOUT 120)

    if(ETS_SPOTIFY_ASAN)  # the AddressSanitizer runtime DLL sits next to the compiler
        cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH ASAN_RUNTIME_DIR)
        set_tests_properties(unit harness PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${ASAN_RUNTIME_DIR}")
    endif()
endif()

# ---- Install: cmake --install build --config Release ----
#
# Copies the plugin into the game. The game folder is looked up in the Steam libraries; for another
# folder configure with -DETS2_DIR=<game folder> or install with --prefix <game folder>.
# To uninstall, delete <game>\bin\win_x64\plugins\ets_spotify.dll.

include(cmake/ets2.cmake)
set(ETS2_DIR "" CACHE PATH "Euro Truck Simulator 2 folder for cmake --install (empty: look in the Steam libraries)")
if(NOT ETS2_DIR)
    ets2_find_game(found)
    if(found)
        set(ETS2_DIR "${found}" CACHE PATH "Euro Truck Simulator 2 folder for cmake --install" FORCE)
    endif()
endif()
if(ETS2_DIR AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${ETS2_DIR}" CACHE PATH "Install prefix: the game folder" FORCE)
endif()
message(STATUS "Game folder for cmake --install: ${ETS2_DIR}")

install(CODE [[
    if(NOT EXISTS "${CMAKE_INSTALL_PREFIX}/bin/win_x64/eurotrucks2.exe")
        message(FATAL_ERROR "Not a Euro Truck Simulator 2 folder: ${CMAKE_INSTALL_PREFIX}\n"
                            "Configure with -DETS2_DIR=<game folder> or install with --prefix <game folder>.")
    endif()
    execute_process(COMMAND tasklist /FI "IMAGENAME eq eurotrucks2.exe" /NH OUTPUT_VARIABLE tasks ERROR_QUIET)
    if(tasks MATCHES "eurotrucks2.exe")
        message(FATAL_ERROR "Close Euro Truck Simulator 2 first: the game keeps the plugin file locked.")
    endif()
]])
install(FILES $<TARGET_FILE:ets_spotify> DESTINATION bin/win_x64/plugins)
