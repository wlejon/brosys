cmake_minimum_required(VERSION 3.24)

project(brosys VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Respect memory-conscious compile & link job bounds
if(NOT DEFINED BRO_COMPILE_JOBS)
    set(BRO_COMPILE_JOBS 4 CACHE STRING "Maximum parallel compile jobs")
endif()
if(NOT DEFINED BRO_LINK_JOBS)
    set(BRO_LINK_JOBS 2 CACHE STRING "Maximum parallel link jobs")
endif()
set_property(GLOBAL PROPERTY JOB_POOLS compile_job_pool=${BRO_COMPILE_JOBS} link_job_pool=${BRO_LINK_JOBS})
set(CMAKE_JOB_POOL_COMPILE compile_job_pool)
set(CMAKE_JOB_POOL_LINK link_job_pool)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:/MP>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX>:/W4>
    )
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
else()
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

set(BROSYS_SOURCES
    src/version.cpp
    src/power_common.cpp
    src/audio_common.cpp
    src/network_common.cpp
    src/notifications_common.cpp
    src/tray_common.cpp
    src/sys.cpp
)

if(WIN32)
    list(APPEND BROSYS_SOURCES
        src/power_win.cpp
        src/audio_win.cpp
        src/network_win.cpp
        src/notifications_win.cpp
        src/tray_win.cpp
    )
else()
    list(APPEND BROSYS_SOURCES
        src/power_linux.cpp
        src/audio_linux.cpp
        src/network_linux.cpp
        src/notifications_linux.cpp
        src/tray_linux.cpp
        src/dbus_helper.cpp
    )
endif()

add_library(brosys STATIC ${BROSYS_SOURCES})
add_library(brosys::brosys ALIAS brosys)
target_compile_definitions(brosys PUBLIC BROSYS_STATIC_DEFINE)

target_include_directories(brosys
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
    target_link_libraries(brosys PUBLIC
        powrprof
        user32
        ole32
        oleaut32
        wlanapi
        iphlpapi
        advapi32
        shell32
        ws2_32
    )
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSYSTEMD QUIET libsystemd)
        if(LIBSYSTEMD_FOUND)
            target_link_libraries(brosys PRIVATE ${LIBSYSTEMD_LIBRARIES})
            target_include_directories(brosys PRIVATE ${LIBSYSTEMD_INCLUDE_DIRS})
        endif()
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(brosys PUBLIC Threads::Threads)
endif()

option(BROSYS_BUILD_TESTS "Build brosys test suite" ON)
if(BROSYS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
