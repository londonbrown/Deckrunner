cmake_minimum_required(VERSION 3.24)
project(deckrunner_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# RED4ext.SDK (MIT) at the commit that defines RUNTIME_VERSION_2_50, header-only.
set(RED4EXT_HEADER_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../cp-RED4ext.SDK" red4ext_sdk EXCLUDE_FROM_ALL)

# PhysX 3.4 public headers (BSD-3), copied to third_party with CDPR's one ABI difference applied
# (PxShape::getReferenceCount). Nothing is linked: PxGetPhysics is resolved at runtime from the game's
# own PhysX3_x64.dll and everything else used is virtual or inline.
set(PHYSX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../ref-PhysX-3.4")

add_library(deckrunner_probe SHARED src/Main.cpp src/PhysxProbe.cpp src/StageB.cpp src/StageC.cpp src/Board.cpp src/Pad.cpp src/Audio.cpp src/Data.cpp src/Transform.cpp src/Visibility.cpp)
target_include_directories(deckrunner_probe PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/physx-3.4.2-cdpr-include"
  "${PHYSX_ROOT}/PxShared/include")
target_compile_definitions(deckrunner_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX NDEBUG)
target_compile_options(deckrunner_probe PRIVATE /EHsc /W3 /utf-8)
target_link_libraries(deckrunner_probe PRIVATE RED4ext::SDK hid setupapi xaudio2 ole32)

# Offline ABI proof (not shipped): vtable offsets used by this compiler vs. offsets read from the game binary.
add_executable(abi_offsets tests/abi_offsets.cpp)
target_include_directories(abi_offsets PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/physx-3.4.2-cdpr-include"
  "${PHYSX_ROOT}/PxShared/include")
target_compile_definitions(abi_offsets PRIVATE NDEBUG)

# Offline Stage B harness (not shipped): drives deckrunner_runtime.dll with probe dumps.
add_executable(stageb_harness tests/stageb_harness.cpp src/Audio.cpp src/Data.cpp)
target_link_libraries(stageb_harness PRIVATE xaudio2 ole32)
target_compile_definitions(stageb_harness PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# Offline winding check (not shipped): heightfield cells must face up.
add_executable(hf_winding tests/hf_winding.cpp)

# Offline check of the Stage C1 chase-camera maths (not shipped).
add_executable(rigid_math tests/rigid_math.cpp)

# Offline checks of the PlayStation pad reader (not shipped): report parsing, and a read-only probe of a connected pad.
add_executable(pad_parse tests/pad_parse.cpp src/Pad.cpp)
target_compile_definitions(pad_parse PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pad_parse PRIVATE hid setupapi)
add_executable(pad_probe tests/pad_probe.cpp src/Pad.cpp)
target_compile_definitions(pad_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pad_probe PRIVATE hid setupapi)
