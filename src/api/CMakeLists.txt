cmake_minimum_required(VERSION 3.24)

set(BROSYS_API_SOURCES
    host_class.cpp
    api.cpp
    native_power.cpp
    native_audio.cpp
    native_network.cpp
    native_bluetooth.cpp
    native_notifications.cpp
    native_tray.cpp
)

add_library(brosys_api STATIC
    ${BROSYS_API_SOURCES}
)

target_include_directories(brosys_api PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
)

target_link_libraries(brosys_api PUBLIC
    brosys
    bronze_runtime_shared
)

target_compile_features(brosys_api PUBLIC cxx_std_20)

brosys_warnings(brosys_api)
