# Linux test support (shared by every Linux fragment) and the D-Bus layer test.
add_library(brosys_linux_testsupport STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/linux/support/proc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/linux/support/private_bus.cpp)
target_include_directories(brosys_linux_testsupport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(brosys_linux_testsupport PUBLIC brodbus::brodbus)
brosys_warnings(brosys_linux_testsupport)

brosys_test(test_dbus_layer SOURCES linux/test_dbus_layer.cpp LIBS brosys_linux_testsupport PkgConfig::BROSYS_SDBUS)
