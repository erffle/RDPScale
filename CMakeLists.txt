cmake_minimum_required(VERSION 3.20)

project(RDPScale VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep the produced EXE/DLL self-contained with the static MSVC runtime.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)
FetchContent_Declare(
    detours_src
    GIT_REPOSITORY https://github.com/microsoft/Detours.git
    # Pinned after the upstream VS/MSVC C2362 fixes; current upstream as of
    # 2026-09-16. Pinning keeps public builds reproducible.
    GIT_TAG adb07604aa56508448b95bf037c2a6d0d3b6831a
    GIT_SHALLOW FALSE
)
FetchContent_GetProperties(detours_src)
if(NOT detours_src_POPULATED)
    FetchContent_Populate(detours_src)
endif()

# Detours ships with nmake files rather than a CMake project. Build the same
# source set used by its official static library.
add_library(detours STATIC
    ${detours_src_SOURCE_DIR}/src/detours.cpp
    ${detours_src_SOURCE_DIR}/src/modules.cpp
    ${detours_src_SOURCE_DIR}/src/disasm.cpp
    ${detours_src_SOURCE_DIR}/src/image.cpp
    ${detours_src_SOURCE_DIR}/src/creatwth.cpp
    ${detours_src_SOURCE_DIR}/src/disolx86.cpp
    ${detours_src_SOURCE_DIR}/src/disolx64.cpp
    ${detours_src_SOURCE_DIR}/src/disolia64.cpp
    ${detours_src_SOURCE_DIR}/src/disolarm.cpp
    ${detours_src_SOURCE_DIR}/src/disolarm64.cpp
)
target_include_directories(detours PUBLIC ${detours_src_SOURCE_DIR}/src)
target_compile_definitions(detours PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(RDPScale
    src/launcher.cpp
)

add_library(RDPScaleHook SHARED
    src/hook.cpp
    src/hook.def
)

target_link_libraries(RDPScale PRIVATE detours)
target_link_libraries(RDPScaleHook PRIVATE detours Shcore)

foreach(target RDPScale RDPScaleHook)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    endif()
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out"
        ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/lib"
    )
endforeach()
