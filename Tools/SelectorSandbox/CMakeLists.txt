cmake_minimum_required(VERSION 3.24)
project(SkiSelectorSandboxProbe LANGUAGES CXX)

add_executable(SkiSelectorSandboxProbe probe.cpp)
target_compile_features(SkiSelectorSandboxProbe PRIVATE cxx_std_20)
target_compile_definitions(SkiSelectorSandboxProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(SkiSelectorSandboxProbe PRIVATE userenv advapi32 ws2_32)

set(CEF_ROOT "C:/Program Files/Epic Games/UE_5.8/Engine/Source/ThirdParty/CEF3/cef_binary_128.4.13+ge76af7e+chromium-128.0.6613.138+v2_windows64" CACHE PATH "Unreal 5.8 CEF SDK")
add_executable(SkiSelectorCefProbe cef_probe.cpp)
target_compile_features(SkiSelectorCefProbe PRIVATE cxx_std_20)
target_compile_definitions(SkiSelectorCefProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(SkiSelectorCefProbe PRIVATE "${CEF_ROOT}")
target_link_libraries(SkiSelectorCefProbe PRIVATE
    "${CEF_ROOT}/Release/libcef.lib"
    "${CEF_ROOT}/VS2015/libcef_dll_wrapper/Release/libcef_dll_wrapper.lib"
    comctl32 shlwapi user32)
