# Linux notifications + tray (StatusNotifier / dbusmenu) tests and their
# helper processes. Everything runs on private dbus-daemons.

# Pure conversions (hints, images, SNI properties, dbusmenu layouts).
brosys_test(test_desktop_parse SOURCES linux/test_desktop_parse.cpp)

brosys_test(test_notify_server SOURCES linux/test_notify_server.cpp LIBS brosys_linux_testsupport TIMEOUT 180)

# A real StatusNotifierItem + dbusmenu client process.
add_executable(brosys_sni_item ${CMAKE_CURRENT_SOURCE_DIR}/linux/desktop/sni_item.cpp)
target_link_libraries(brosys_sni_item PRIVATE brosys)
target_include_directories(brosys_sni_item PRIVATE ${PROJECT_SOURCE_DIR}/src)
brosys_warnings(brosys_sni_item)

# A second brosys TrayHost process (owns the watcher name for the WatcherClient tests).
add_executable(brosys_tray_host ${CMAKE_CURRENT_SOURCE_DIR}/linux/desktop/tray_host.cpp)
target_link_libraries(brosys_tray_host PRIVATE brosys)
brosys_warnings(brosys_tray_host)

brosys_test(test_tray_sni SOURCES linux/test_tray_sni.cpp LIBS brosys_linux_testsupport TIMEOUT 180)
target_compile_definitions(test_tray_sni PRIVATE
    BROSYS_SNI_ITEM="$<TARGET_FILE:brosys_sni_item>"
    BROSYS_TRAY_HOST="$<TARGET_FILE:brosys_tray_host>")
add_dependencies(test_tray_sni brosys_sni_item brosys_tray_host)

# Third-party item: libayatana-appindicator3 (optional; the test skips without it).
pkg_check_modules(BROSYS_AYATANA QUIET IMPORTED_TARGET ayatana-appindicator3-0.1)
brosys_test(test_tray_ayatana SOURCES linux/test_tray_ayatana.cpp LIBS brosys_linux_testsupport)
if(BROSYS_AYATANA_FOUND)
    add_executable(brosys_ayatana_item ${CMAKE_CURRENT_SOURCE_DIR}/linux/desktop/ayatana_item.cpp)
    target_link_libraries(brosys_ayatana_item PRIVATE PkgConfig::BROSYS_AYATANA)
    brosys_warnings(brosys_ayatana_item)
    # libayatana-appindicator 0.5.9x marks the API every tray app still uses deprecated.
    target_compile_options(brosys_ayatana_item PRIVATE -Wno-deprecated-declarations)
    target_compile_definitions(test_tray_ayatana PRIVATE BROSYS_AYATANA_ITEM="$<TARGET_FILE:brosys_ayatana_item>")
    add_dependencies(test_tray_ayatana brosys_ayatana_item)
else()
    target_compile_definitions(test_tray_ayatana PRIVATE BROSYS_AYATANA_ITEM="")
endif()
