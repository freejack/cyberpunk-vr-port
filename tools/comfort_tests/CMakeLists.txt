cmake_minimum_required(VERSION 3.20)
project(ComfortTests LANGUAGES CXX)
enable_testing()

# The cutscene comfort vignette: policy and geometry (pure), and the pass itself on the real adapter.
add_executable(comfort_vignette_policy policy.cpp)
target_include_directories(comfort_vignette_policy PRIVATE ../../include)
target_compile_features(comfort_vignette_policy PRIVATE cxx_std_20)
target_compile_definitions(comfort_vignette_policy PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME comfort_vignette_policy COMMAND comfort_vignette_policy)

add_executable(comfort_vignette_gpu gpu.cpp ../../src/Render/VignettePass.cpp)
target_include_directories(comfort_vignette_gpu PRIVATE ../../include)
target_compile_features(comfort_vignette_gpu PRIVATE cxx_std_20)
target_compile_definitions(comfort_vignette_gpu PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(comfort_vignette_gpu PRIVATE d3d12 dxgi d3dcompiler)
add_test(NAME comfort_vignette_gpu COMMAND comfort_vignette_gpu)
add_test(NAME comfort_vignette_gpu_warp COMMAND comfort_vignette_gpu warp)
