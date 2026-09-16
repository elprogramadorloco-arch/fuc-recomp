cmake_minimum_required(VERSION 3.22)
project(FateUnlimitedCodesRecompCore LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The current native PSP host targets Windows x64.")
endif()

foreach(required FUC_CORPUS_DIR FUC_PSPRECOMP_CORE FUC_FFMPEG_ROOT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} must be provided by tools/build_psp_core.ps1")
    endif()
endforeach()

file(GLOB FUC_GENERATED_SOURCES CONFIGURE_DEPENDS "${FUC_CORPUS_DIR}/*.cpp")
if(NOT FUC_GENERATED_SOURCES)
    message(FATAL_ERROR "No generated PSP AOT units were found in ${FUC_CORPUS_DIR}")
endif()

add_library(psprecomp_core STATIC IMPORTED GLOBAL)
set_target_properties(psprecomp_core PROPERTIES IMPORTED_LOCATION "${FUC_PSPRECOMP_CORE}")

add_executable(FateUnlimitedCodesRecompCore
    ${FUC_GENERATED_SOURCES}
    recomp/fate_psp_profile.cpp
    recomp/psp_entry_host.cpp
    src/port/player_input.cpp
    src/port/psp_pad.cpp
    src/port/state_bridge.cpp
    src/port/fate_psp_battle_map.cpp
    src/port/psp_atrac.cpp
    src/port/psp_mpeg.cpp
    src/port/psp_pgf.cpp
    src/port/psp_vag.cpp
    src/renderer/psp_ge_command.cpp
    src/renderer/psp_ge_surface.cpp
    src/renderer/psp_ge_d3d11_surface.cpp
    src/platform/windows_psp_audio.cpp
    src/platform/windows_psp_window.cpp
    media/vcs_media_decoder.cpp
)
target_compile_features(FateUnlimitedCodesRecompCore PRIVATE cxx_std_20)
target_compile_definitions(FateUnlimitedCodesRecompCore PRIVATE
    UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_include_directories(FateUnlimitedCodesRecompCore PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../PSPRecomp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp"
    "${CMAKE_CURRENT_SOURCE_DIR}/media"
    "${FUC_FFMPEG_ROOT}/include"
)

foreach(ffmpeg avcodec avformat avutil swresample swscale)
    set(ffmpeg_library "${FUC_FFMPEG_ROOT}/lib/${ffmpeg}.lib")
    if(NOT EXISTS "${ffmpeg_library}")
        message(FATAL_ERROR "Missing FFmpeg import library: ${ffmpeg_library}")
    endif()
    target_link_libraries(FateUnlimitedCodesRecompCore PRIVATE "${ffmpeg_library}")
endforeach()

target_link_libraries(FateUnlimitedCodesRecompCore PRIVATE
    psprecomp_core user32 gdi32 d3d11 d3dcompiler xinput9_1_0 winmm)

if(MSVC)
    target_compile_options(FateUnlimitedCodesRecompCore PRIVATE /W4 /permissive- /EHsc)
    target_link_options(FateUnlimitedCodesRecompCore PRIVATE /STACK:268435456)
elseif(MINGW)
    target_compile_options(FateUnlimitedCodesRecompCore PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unknown-pragmas)
    target_link_options(FateUnlimitedCodesRecompCore PRIVATE -static -Wl,--stack,268435456)
endif()

set_target_properties(FateUnlimitedCodesRecompCore PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
