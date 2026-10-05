# Linux power / network / audio backends.
#
#   test_system_models    pure translation layers (UPower / NM / PipeWire values -> public types)
#   test_power_mock       real upowerd on a private bus over a umockdev sysfs + a fake logind
#   test_power_system     the real system bus, read-only (compared with upower / busctl / systemd-inhibit)
#   test_network_fake     a scripted NetworkManager on a private bus (updates, coalescing, scans)
#   test_network_system   the real NetworkManager, read-only (compared with nmcli)
#   test_network_wifi     opt-in: a real scan on a mac80211_hwsim radio (see the test's header)
#   test_audio_private    a private PipeWire + WirePlumber with null sinks / sources (mutations)
#   test_audio_session    the user's PipeWire session, read-only (compared with pw-dump / wpctl)

add_library(brosys_system_fakes STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/linux/fakes/fake_logind.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/linux/fakes/fake_nm.cpp)
target_link_libraries(brosys_system_fakes PUBLIC brosys brosys_linux_testsupport)
target_include_directories(brosys_system_fakes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
brosys_warnings(brosys_system_fakes)

set(_sys_libs brosys_system_fakes brosys_linux_testsupport PkgConfig::BROSYS_SDBUS)

pkg_check_modules(BROSYS_UMOCKDEV QUIET IMPORTED_TARGET umockdev-1.0)
brosys_test(test_power_mock SOURCES linux/test_power_mock.cpp LIBS ${_sys_libs} TIMEOUT 180)
if(BROSYS_UMOCKDEV_FOUND)
    target_link_libraries(test_power_mock PRIVATE PkgConfig::BROSYS_UMOCKDEV)
    target_compile_definitions(test_power_mock PRIVATE BROSYS_HAVE_UMOCKDEV=1)
endif()
brosys_test(test_power_system SOURCES linux/test_power_system.cpp LIBS ${_sys_libs})
brosys_test(test_network_fake SOURCES linux/test_network_fake.cpp LIBS ${_sys_libs})
brosys_test(test_network_system SOURCES linux/test_network_system.cpp LIBS ${_sys_libs})
brosys_test(test_network_wifi SOURCES linux/test_network_wifi.cpp LIBS ${_sys_libs} TIMEOUT 300)

if(BROSYS_WITH_PIPEWIRE)
    brosys_test(test_audio_session SOURCES linux/test_audio_session.cpp LIBS ${_sys_libs})
    brosys_test(test_audio_private SOURCES linux/test_audio_private.cpp linux/fakes/private_pipewire.cpp
                LIBS ${_sys_libs} TIMEOUT 180)
endif()

brosys_test(test_system_models SOURCES linux/test_system_models.cpp LIBS ${_sys_libs})
if(BROSYS_WITH_PIPEWIRE)
    target_link_libraries(test_system_models PRIVATE PkgConfig::BROSYS_PIPEWIRE)
    target_compile_definitions(test_system_models PRIVATE BROSYS_TEST_PIPEWIRE=1)
    target_compile_options(test_system_models PRIVATE -Wno-missing-field-initializers)
endif()
