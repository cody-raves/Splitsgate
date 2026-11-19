cmake_minimum_required(VERSION 3.20)
project(SplitgateNativeOverlay LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(vendor/minhook EXCLUDE_FROM_ALL)

add_library(imgui STATIC
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_dx11.cpp
    vendor/imgui/backends/imgui_impl_dx12.cpp
    vendor/imgui/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC vendor/imgui vendor/imgui/backends)
target_compile_definitions(imgui PRIVATE UNICODE _UNICODE)

add_library(SplitgateSplitsOverlay SHARED overlay.cpp)
target_include_directories(SplitgateSplitsOverlay PRIVATE vendor/imgui vendor/imgui/backends vendor/minhook/include)
target_link_libraries(SplitgateSplitsOverlay PRIVATE imgui minhook d3d11 d3d12 dxgi dwmapi windowscodecs)
target_compile_definitions(SplitgateSplitsOverlay PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_custom_command(TARGET SplitgateSplitsOverlay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SplitgateSplitsOverlay>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/splitgate-new-s-mark-clean.png"
        "$<TARGET_FILE_DIR:SplitgateSplitsOverlay>/assets/splitgate-new-s-mark-clean.png"
)

add_executable(EmbedBinary embed_binary.cpp)

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(OVERLAY_PAYLOAD_CPP "${GENERATED_DIR}/overlay_payload.cpp")
set(LOGO_PAYLOAD_CPP "${GENERATED_DIR}/logo_payload.cpp")
add_custom_command(
    OUTPUT "${OVERLAY_PAYLOAD_CPP}"
    COMMAND $<TARGET_FILE:EmbedBinary> $<TARGET_FILE:SplitgateSplitsOverlay> "${OVERLAY_PAYLOAD_CPP}" g_bundled_overlay_dll
    DEPENDS EmbedBinary SplitgateSplitsOverlay
    VERBATIM
)
add_custom_command(
    OUTPUT "${LOGO_PAYLOAD_CPP}"
    COMMAND $<TARGET_FILE:EmbedBinary> "${CMAKE_CURRENT_SOURCE_DIR}/assets/splitgate-new-s-mark-clean.png" "${LOGO_PAYLOAD_CPP}" g_bundled_logo_png
    DEPENDS EmbedBinary "${CMAKE_CURRENT_SOURCE_DIR}/assets/splitgate-new-s-mark-clean.png"
    VERBATIM
)

add_executable(SplitgateOverlayLoader loader.cpp "${OVERLAY_PAYLOAD_CPP}" "${LOGO_PAYLOAD_CPP}")
target_compile_definitions(SplitgateOverlayLoader PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(SplitgateOverlayLoader PRIVATE winhttp)

add_executable(ArenaRaceProbe arena_probe.cpp)
target_compile_definitions(ArenaRaceProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(SplitgateRaceProbe sg1_probe.cpp)
target_compile_definitions(SplitgateRaceProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(ArenaDllLoader arena_dll_loader.cpp)
target_compile_definitions(ArenaDllLoader PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

