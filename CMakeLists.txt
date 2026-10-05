cmake_minimum_required(VERSION 3.24)

project(brosys VERSION 0.2.0 LANGUAGES CXX)

# Targets
#   brosys::brosys   system services (power, audio, network, notifications, tray)
#
# Sources are collected per backend directory:
#   src/common         portable (enum names, shared helpers)
#   src/win            Windows backends
#   src/linux/dbus     the sd-bus layer (connections, values, object export)
#   src/linux/<svc>    Linux backends (notify, tray, power, network, audio)
#   src/mac            macOS backends (IOKit, CoreAudio, SystemConfiguration, CoreWLAN)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(_brosys_top_level OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_brosys_top_level ON)
endif()

option(BROSYS_BUILD_TESTS "Build the brosys tests" ${_brosys_top_level})

if(MSVC AND _brosys_top_level)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

function(brosys_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /MP)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

file(GLOB BROSYS_SOURCES CONFIGURE_DEPENDS src/common/*.cpp)

if(WIN32)
    file(GLOB _win_sources CONFIGURE_DEPENDS src/win/*.cpp)
    list(APPEND BROSYS_SOURCES ${_win_sources})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    find_package(Threads REQUIRED)
    pkg_check_modules(BROSYS_SDBUS REQUIRED IMPORTED_TARGET libsystemd>=246)
    pkg_check_modules(BROSYS_PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
    set(_pw_default OFF)
    if(BROSYS_PIPEWIRE_FOUND)
        set(_pw_default ON)
    endif()
    option(BROSYS_WITH_PIPEWIRE "Build the PipeWire audio backend (libpipewire-0.3)" ${_pw_default})
    if(BROSYS_WITH_PIPEWIRE AND NOT BROSYS_PIPEWIRE_FOUND)
        message(FATAL_ERROR "BROSYS_WITH_PIPEWIRE needs libpipewire-0.3 (pkg-config)")
    endif()
    file(GLOB _linux_sources CONFIGURE_DEPENDS
        src/linux/*.cpp
        src/linux/dbus/*.cpp
        src/linux/notify/*.cpp
        src/linux/tray/*.cpp
        src/linux/power/*.cpp
        src/linux/network/*.cpp)
    # The no-PipeWire stub is chosen below, not by the src/linux/*.cpp glob
    # (otherwise both AudioService::create definitions land in the archive).
    list(REMOVE_ITEM _linux_sources ${CMAKE_CURRENT_SOURCE_DIR}/src/linux/audio_unavailable.cpp)
    list(APPEND BROSYS_SOURCES ${_linux_sources})
    if(BROSYS_WITH_PIPEWIRE)
        file(GLOB _pw_sources CONFIGURE_DEPENDS src/linux/audio/*.cpp)
        list(APPEND BROSYS_SOURCES ${_pw_sources})
    else()
        list(APPEND BROSYS_SOURCES src/linux/audio_unavailable.cpp)
    endif()
elseif(APPLE)
    # Objective-C++ is confined to the few .mm files that need Cocoa-only APIs
    # (CoreWLAN, NSWorkspace, Network.framework path monitoring).
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    find_package(Threads REQUIRED)
    file(GLOB _mac_sources CONFIGURE_DEPENDS src/mac/*.cpp src/mac/*.mm)
    list(APPEND BROSYS_SOURCES ${_mac_sources})
else()
    message(FATAL_ERROR "brosys implements Windows, Linux and macOS backends only")
endif()

add_library(brosys STATIC ${BROSYS_SOURCES})
add_library(brosys::brosys ALIAS brosys)
target_include_directories(brosys
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
brosys_warnings(brosys)

if(WIN32)
    target_compile_definitions(brosys PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
    target_link_libraries(brosys PRIVATE
        user32 gdi32 shell32 ole32 oleaut32 advapi32 powrprof setupapi
        iphlpapi ws2_32 wlanapi uuid)
elseif(APPLE)
    target_compile_options(brosys PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(brosys PUBLIC Threads::Threads PRIVATE
        "-framework CoreFoundation" "-framework Foundation" "-framework IOKit" "-framework CoreAudio"
        "-framework AudioToolbox" "-framework CoreServices" "-framework SystemConfiguration"
        "-framework CoreWLAN" "-framework Network" "-framework AppKit")
else()
    target_link_libraries(brosys PRIVATE PkgConfig::BROSYS_SDBUS PUBLIC Threads::Threads)
    if(BROSYS_WITH_PIPEWIRE)
        target_link_libraries(brosys PRIVATE PkgConfig::BROSYS_PIPEWIRE)
        target_compile_definitions(brosys PRIVATE BROSYS_HAVE_PIPEWIRE=1)
        if(NOT MSVC)
            # spa / pipewire headers use designated-initializer and
            # zero-length-array idioms that -Wpedantic flags in C++.
            target_compile_options(brosys PRIVATE -Wno-missing-field-initializers)
        endif()
    endif()
endif()

if(BROSYS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
