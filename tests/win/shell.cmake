# Windows shell services: tray hosting and notifications.
#
# Shell-mode tests run the tray host on a private desktop with a real
# Shell_NotifyIcon client process (brosys_tray_client) on the same desktop;
# the user's desktop and its Explorer tray are never touched.

add_executable(brosys_tray_client win/tray_client.cpp)
target_link_libraries(brosys_tray_client PRIVATE user32 gdi32 shell32)
target_compile_definitions(brosys_tray_client PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
brosys_warnings(brosys_tray_client)

brosys_test(test_win_tray_shell SOURCES win/test_win_tray_shell.cpp)
add_dependencies(test_win_tray_shell brosys_tray_client)

brosys_test(test_win_notify_balloons SOURCES win/test_win_notify_balloons.cpp)
add_dependencies(test_win_notify_balloons brosys_tray_client)

# Against the user's real desktop, read-only: Auto resolves to None beside
# Explorer and nothing is created.
brosys_test(test_win_shell_alongside SOURCES win/test_win_shell_alongside.cpp)

# Pure pieces: the wire layout, icon conversion, balloon mapping.
brosys_test(test_win_tray_wire SOURCES win/test_win_tray_wire.cpp)
