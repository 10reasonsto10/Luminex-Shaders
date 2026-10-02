cmake_minimum_required(VERSION 3.24)

project(LuminexShaders VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")


set(LUMINEX_RESHADE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/reshade" CACHE PATH "ReShade source checkout")

include(FetchContent)

set(MINHOOK_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MINHOOK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.3
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(minhook imgui)

# ReShade FX is intentionally embedded as a compiler library only. Luminex owns
# the D3D11 hook, resources, state isolation, and technique execution.
add_library(luminex_reshadefx STATIC
    ${LUMINEX_RESHADE_SOURCE_DIR}/source/effect_codegen_dxbc.cpp
    ${LUMINEX_RESHADE_SOURCE_DIR}/source/effect_expression.cpp
    ${LUMINEX_RESHADE_SOURCE_DIR}/source/effect_lexer.cpp
    ${LUMINEX_RESHADE_SOURCE_DIR}/source/effect_parser_exp.cpp
    ${LUMINEX_RESHADE_SOURCE_DIR}/source/effect_parser_stmt.cpp
    ${LUMINEX_RESHADE_SOURCE_DIR}/source/effect_preprocessor.cpp
    ${LUMINEX_RESHADE_SOURCE_DIR}/source/effect_symbol_table.cpp
)
target_include_directories(luminex_reshadefx PUBLIC ${LUMINEX_RESHADE_SOURCE_DIR}/source)
target_compile_definitions(luminex_reshadefx PRIVATE _UNICODE UNICODE NOMINMAX)
target_link_libraries(luminex_reshadefx PRIVATE d3dcompiler)

add_library(luminex_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(luminex_imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(luminex_imgui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_library(LuminexRuntime SHARED
    src/runtime.cpp
    src/runtime.hpp
    src/runtime_entry.cpp
)

target_compile_definitions(LuminexRuntime PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_include_directories(LuminexRuntime PRIVATE ${LUMINEX_RESHADE_SOURCE_DIR}/include)

target_link_libraries(LuminexRuntime PRIVATE
    minhook
    luminex_imgui
    luminex_reshadefx
    d3d11
    dxgi
    d3dcompiler
    user32
    gdi32
)

set_target_properties(LuminexRuntime PROPERTIES
    OUTPUT_NAME LuminexRuntime
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC)
    target_compile_options(LuminexRuntime PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(luminex_imgui PRIVATE /W3 /EHsc)
endif()

if(MSVC)
    target_compile_options(luminex_reshadefx PRIVATE /W3 /EHsc /utf-8 /Zc:char8_t-)
endif()

add_executable(LuminexLoader src/loader.cpp src/loader.rc src/loader.manifest)
target_compile_definitions(LuminexLoader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_target_properties(LuminexLoader PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_GLOBAL_EnableUAC "true"
    VS_GLOBAL_UACExecutionLevel "RequireAdministrator"
)
if(MSVC)
    target_compile_options(LuminexLoader PRIVATE /W4 /permissive- /EHsc)
    target_link_options(LuminexLoader PRIVATE /MANIFEST:NO)
endif()
