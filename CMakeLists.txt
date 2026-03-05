cmake_minimum_required(VERSION 3.16)
project(RemoteDesktopHost)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows target only
if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows only")
endif()

# WebRTC stream (libdatachannel): optional. For WebRTC in exe use vcpkg:
#   vcpkg install libdatachannel:x64-windows-static
#   cmake -DCMAKE_TOOLCHAIN_FILE=[vcpkg]/scripts/buildsystems/vcpkg.cmake ..
set(USE_WEBRTC_STREAM OFF)
find_package(LibDataChannel CONFIG QUIET)
if(LibDataChannel_FOUND)
    set(USE_WEBRTC_STREAM ON)
    message(STATUS "WebRTC stream: LibDataChannel found")
else()
    message(STATUS "WebRTC stream: LibDataChannel not found (use vcpkg for WebRTC, see README)")
endif()

add_executable(RemoteDesktopHost
    main.cpp
)
if(USE_WEBRTC_STREAM)
    target_sources(RemoteDesktopHost PRIVATE webrtc_stream.cpp)
    target_compile_definitions(RemoteDesktopHost PRIVATE USE_WEBRTC_STREAM=1)
    target_link_libraries(RemoteDesktopHost PRIVATE LibDataChannel::datachannel)
endif()

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
    pdh
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
