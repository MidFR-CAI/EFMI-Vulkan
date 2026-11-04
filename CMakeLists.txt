cmake_minimum_required(VERSION 3.20)
project(efmivulkan LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "efmivulkan is x64 only. Configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
# The official Vulkan headers (vulkan.h, vk_layer.h). SOURCE_SUBDIR points nowhere:
# only the include folder is used.
FetchContent_Declare(vulkan_headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG v1.3.283
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _no_cmake)
FetchContent_MakeAvailable(vulkan_headers)
set(VULKAN_INCLUDE "${vulkan_headers_SOURCE_DIR}/include")

# Code shared by the layer and the tests: hashing, the ini language, the EFMI mod
# parser, logging, and WWMI12's skeleton and shape key code for later milestones.
add_library(efmivulkan_core STATIC
  src/hash.cpp
  src/skeleton.cpp
  src/shapekeys.cpp
  src/ini.cpp
  src/efmimod.cpp
  src/texhash.cpp
  src/log.cpp)
target_include_directories(efmivulkan_core PUBLIC src)
target_compile_definitions(efmivulkan_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(efmivulkan_core PRIVATE /W4 /utf-8)

# The Vulkan layer. The loader finds it through efmivulkan.json, written next to it.
add_library(efmivulkan SHARED
  src/efmivulkan.def
  src/layer.cpp
  src/efmi.cpp
  src/efmidraw.cpp
  src/textures.cpp
  src/efmitex.cpp
  src/efmibones.cpp
  src/ibregion.cpp
  src/modindex.cpp)
target_include_directories(efmivulkan PRIVATE "${VULKAN_INCLUDE}")
target_link_libraries(efmivulkan PRIVATE efmivulkan_core)
target_compile_definitions(efmivulkan PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
# /EHa: destructors also run when a hardware fault is caught by Guard().
target_compile_options(efmivulkan PRIVATE /W4 /EHa /Zi /utf-8)
target_link_options(efmivulkan PRIVATE /DEBUG /OPT:REF /OPT:ICF)
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:efmivulkan>/efmivulkan.json" CONTENT [=[{
  "file_format_version": "1.1.2",
  "layer": {
    "name": "VK_LAYER_EFMIVULKAN_mod_loader",
    "type": "GLOBAL",
    "library_path": ".\\efmivulkan.dll",
    "api_version": "1.3.283",
    "implementation_version": "1",
    "description": "EFMI mods on Vulkan. Acts in Endfield.exe only; passes everything through elsewhere.",
    "disable_environment": { "EFMIVULKAN_DISABLE": "1" }
  }
}
]=])

# Registers the layer only while the game runs.
add_executable(efmivulkan_launch tools/launch.cpp)
target_compile_definitions(efmivulkan_launch PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(efmivulkan_launch PRIVATE /W4)
# HKLM: the loader ignores HKCU layers in elevated programs, and Endfield runs elevated.
target_link_options(efmivulkan_launch PRIVATE "/MANIFESTUAC:level='requireAdministrator'")

add_executable(test_hash tests/test_hash.cpp)
target_link_libraries(test_hash PRIVATE efmivulkan_core)

# Parses every mod in a Mods folder and prints what milestone 2 would draw.
add_executable(test_parse tests/test_parse.cpp)
target_compile_definitions(test_parse PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(test_parse PRIVATE /utf-8)
target_link_libraries(test_parse PRIVATE efmivulkan_core)

# Renders with Vulkan through the layer and checks it end to end, without the game.
add_executable(smoketest tests/smoketest.cpp)
target_include_directories(smoketest PRIVATE "${VULKAN_INCLUDE}")
target_compile_definitions(smoketest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(smoketest PRIVATE efmivulkan_core)
add_dependencies(smoketest efmivulkan)
