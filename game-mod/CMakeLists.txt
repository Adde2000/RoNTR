# Built as part of the RE-UE4SS "MyMods" superbuild (see scripts/setup.ps1,
# which generates build/MyMods/CMakeLists.txt referencing this directory).
cmake_minimum_required(VERSION 3.22)

set(TARGET RoNTacticalRadio)
project(${TARGET})

add_library(${TARGET} SHARED "src/dllmain.cpp")
target_include_directories(${TARGET} PRIVATE src)
target_link_libraries(${TARGET} PUBLIC UE4SS PRIVATE ws2_32)
