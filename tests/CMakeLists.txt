# brosys_test(<name> SOURCES ... [LIBS ...]): one real ctest. Exit code 77
# is a skip (a service genuinely absent), anything else nonzero a failure.
function(brosys_test name)
    cmake_parse_arguments(T "" "TIMEOUT" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE brosys ${T_LIBS})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
    brosys_warnings(${name})
    if(WIN32)
        target_compile_definitions(${name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
    endif()
    if(NOT T_TIMEOUT)
        set(T_TIMEOUT 120)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT ${T_TIMEOUT} SKIP_RETURN_CODE 77)
endfunction()

brosys_test(test_event_queue SOURCES common/test_event_queue.cpp)
if(WIN32 OR APPLE)
    brosys_test(test_linux_only_services SOURCES common/test_linux_only_services.cpp)
endif()

# Per-area fragments (each owns its own tests and helper executables).
foreach(fragment
        win/system.cmake win/shell.cmake
        linux/dbus.cmake linux/desktop.cmake linux/system.cmake
        mac/system.cmake)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${fragment})
        if((fragment MATCHES "^win/" AND WIN32) OR (fragment MATCHES "^linux/" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
           OR (fragment MATCHES "^mac/" AND APPLE))
            include(${CMAKE_CURRENT_SOURCE_DIR}/${fragment})
        endif()
    endif()
endforeach()
