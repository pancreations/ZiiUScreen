cmake_minimum_required(VERSION 3.20)
project(ziiuctl CXX)

add_executable(ziiuctl ziiuctl.cpp)
target_include_directories(ziiuctl PRIVATE ../../driver/include)
target_compile_definitions(ziiuctl PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_features(ziiuctl PRIVATE cxx_std_17)
target_compile_options(ziiuctl PRIVATE /W4 /WX)
target_link_libraries(ziiuctl PRIVATE cfgmgr32)
