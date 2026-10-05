# macOS power / audio / network against the OS's own tools (pmset, ioreg,
# scutil, networksetup, osascript, system_profiler, SwitchAudioSource), and
# the shell services' honest capabilities. All read-only toward the Mac.
foreach(t power audio network shell)
    brosys_test(test_mac_${t} SOURCES mac/test_mac_${t}.cpp)
endforeach()
target_link_libraries(test_mac_audio PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
# The audio test waits for hot-plug events, the network test for a scan.
set_tests_properties(test_mac_audio test_mac_network PROPERTIES TIMEOUT 180)
