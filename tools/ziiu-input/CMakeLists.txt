cmake_minimum_required(VERSION 3.20)
project(ziiu_input CXX)

# ViGEmClient (MIT) from the pinned reference checkout; not committed.
#   git clone https://github.com/nefarius/ViGEmClient .local/refs/ViGEmClient
#   git -C .local/refs/ViGEmClient checkout b66d02d57e32cc8595369c53418b843e958649b4
set(VIGEM "${CMAKE_CURRENT_SOURCE_DIR}/../../.local/refs/ViGEmClient")
if(NOT EXISTS "${VIGEM}/src/ViGEmClient.cpp")
    message(FATAL_ERROR "ViGEmClient missing: see tools/ziiu-input/CMakeLists.txt")
endif()

add_executable(ziiu-input ziiu_input.cpp "${VIGEM}/src/ViGEmClient.cpp")
target_include_directories(ziiu-input PRIVATE "${VIGEM}/include")
target_compile_features(ziiu-input PRIVATE cxx_std_17)
target_compile_definitions(ziiu-input PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(ziiu-input PRIVATE /W3)
target_link_libraries(ziiu-input PRIVATE setupapi ws2_32)
set_property(TARGET ziiu-input PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
