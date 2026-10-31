cmake_minimum_required(VERSION 3.24)
project(IndexTrackpadTests LANGUAGES CXX)
enable_testing()
# The OpenXR headers and registry come from the plugin build's FetchContent checkout.
set(openxr "${CMAKE_CURRENT_SOURCE_DIR}/../../build/_deps/openxr-src")
add_executable(index_trackpad_tests main.cpp)
target_include_directories(index_trackpad_tests PRIVATE ../../include "${openxr}/include")
target_compile_features(index_trackpad_tests PRIVATE cxx_std_20)
add_test(NAME index_trackpad_menu COMMAND index_trackpad_tests "${openxr}/specification/registry/xr.xml")
