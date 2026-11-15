# Finds the Euro Truck Simulator 2 folder in the Steam libraries, for `cmake --install`.

# Sets <out> to the game folder, or to an empty string when the game is not found.
function(ets2_find_game out)
    set(${out} "" PARENT_SCOPE)
    cmake_host_system_information(RESULT steam QUERY WINDOWS_REGISTRY "HKCU/Software/Valve/Steam"
                                  VALUE "SteamPath" ERROR_VARIABLE error)
    if(NOT steam)
        return()
    endif()

    # Libraries on other drives are listed in libraryfolders.vdf:  "path"  "D:\\SteamLibrary"
    set(libraries "${steam}")
    set(vdf "${steam}/steamapps/libraryfolders.vdf")
    if(EXISTS "${vdf}")
        file(STRINGS "${vdf}" lines REGEX "\"path\"")
        foreach(line IN LISTS lines)
            if(line MATCHES "\"path\"[ \t]+\"([^\"]+)\"")
                string(REPLACE "\\\\" "/" library "${CMAKE_MATCH_1}")
                list(APPEND libraries "${library}")
            endif()
        endforeach()
    endif()

    foreach(library IN LISTS libraries)
        set(dir "${library}/steamapps/common/Euro Truck Simulator 2")
        if(EXISTS "${dir}/bin/win_x64/eurotrucks2.exe")
            set(${out} "${dir}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()
