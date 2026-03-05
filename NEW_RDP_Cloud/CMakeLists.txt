cmake_minimum_required(VERSION 3.16)
project(RemoteDesktopHost)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows target only
if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows only")
endif()

add_executable(RemoteDesktopHost
    main.cpp
)

target_include_directories(RemoteDesktopHost PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_compile_definitions(RemoteDesktopHost PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00  # Windows 10
    UNICODE
    _UNICODE
)

target_compile_options(RemoteDesktopHost PRIVATE
    /W3
    /O2
    /EHsc
    /MP
)

target_link_libraries(RemoteDesktopHost PRIVATE
    ws2_32
    gdiplus
    d3d11
    dxgi
    psapi
    shlwapi
    shell32
    advapi32
    user32
    gdi32
    ole32
)

# Copy config template to output
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/host_config.json.template
    ${CMAKE_BINARY_DIR}/host_config.json
    COPYONLY
)

set_target_properties(RemoteDesktopHost PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
