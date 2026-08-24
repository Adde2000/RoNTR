cmake_minimum_required(VERSION 3.20)
project(ron_tactical_radio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Path to a clone of https://github.com/teamspeak/ts3client-pluginsdk
set(TS3_PLUGINSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ts3client-pluginsdk"
    CACHE PATH "TeamSpeak 3 Client Plugin SDK checkout")

add_library(ron_tactical_radio SHARED src/plugin.cpp)

target_include_directories(ron_tactical_radio PRIVATE
    "${TS3_PLUGINSDK_DIR}/include"
    src
)

if(WIN32)
    # Output: ron_tactical_radio.dll -> copy to %APPDATA%/TS3Client/plugins/
    set_target_properties(ron_tactical_radio PROPERTIES PREFIX "")
endif()

# --- Off-target unit tests for DSP/spatial logic (Linux/macOS friendly) ---
option(RTR_BUILD_TESTS "Build host-side tests" OFF)
if(RTR_BUILD_TESTS)
    add_executable(rtr_tests tests/test_core.cpp)
    target_include_directories(rtr_tests PRIVATE src)
    enable_testing()
    add_test(NAME core COMMAND rtr_tests)
endif()
