set(TESTS
    test_smoke
    test_power
    test_audio
    test_network
    test_notifications
    test_tray
)

foreach(test_name IN LISTS TESTS)
    add_executable(${test_name} "${test_name}.cpp")
    target_link_libraries(${test_name} PRIVATE brosys)
    target_include_directories(${test_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
