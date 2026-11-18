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
    vendor/imgui/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC vendor/imgui vendor/imgui/backends)
target_compile_definitions(imgui PRIVATE UNICODE _UNICODE)

add_library(SplitgateSplitsOverlay SHARED overlay.cpp)
target_include_directories(SplitgateSplitsOverlay PRIVATE vendor/imgui vendor/imgui/backends vendor/minhook/include)
target_link_libraries(SplitgateSplitsOverlay PRIVATE imgui minhook d3d11 dxgi dwmapi windowscodecs)
target_compile_definitions(SplitgateSplitsOverlay PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_custom_command(TARGET SplitgateSplitsOverlay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SplitgateSplitsOverlay>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/splitgate-new-s-mark-clean.png"
        "$<TARGET_FILE_DIR:SplitgateSplitsOverlay>/assets/splitgate-new-s-mark-clean.png"
)

add_executable(SplitgateOverlayLoader loader.cpp)
target_compile_definitions(SplitgateOverlayLoader PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

