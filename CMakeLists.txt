cmake_minimum_required(VERSION 3.22)
project(FateUnlimitedCodesRecompLauncher LANGUAGES CXX)

add_executable(FateUnlimitedCodesRecomp WIN32
    src/launcher_main.cpp
)
target_compile_features(FateUnlimitedCodesRecomp PRIVATE cxx_std_20)
target_compile_definitions(FateUnlimitedCodesRecomp PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0600)
target_link_libraries(FateUnlimitedCodesRecomp PRIVATE comctl32 shell32 xinput9_1_0)

if(MSVC)
    target_compile_options(FateUnlimitedCodesRecomp PRIVATE /W4 /permissive- /utf-8)
elseif(MINGW)
    target_compile_options(FateUnlimitedCodesRecomp PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(FateUnlimitedCodesRecomp PRIVATE -municode -static)
endif()

set_target_properties(FateUnlimitedCodesRecomp PROPERTIES
    OUTPUT_NAME "FateUnlimitedCodesRecomp"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

add_executable(FateUnlimitedCodesDataTool
    src/prepare_main.cpp
    src/iso9660.cpp
)
target_compile_features(FateUnlimitedCodesDataTool PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(FateUnlimitedCodesDataTool PRIVATE /W4 /permissive- /utf-8)
elseif(MINGW)
    target_compile_options(FateUnlimitedCodesDataTool PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(FateUnlimitedCodesDataTool PRIVATE -municode -static)
endif()
set_target_properties(FateUnlimitedCodesDataTool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)
