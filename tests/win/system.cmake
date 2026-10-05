# Windows power / audio / network against the OS's own answers.
add_library(brosys_win_oracle STATIC ${CMAKE_CURRENT_SOURCE_DIR}/win/support/oracle.cpp)
target_include_directories(brosys_win_oracle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(brosys_win_oracle PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_link_libraries(brosys_win_oracle PUBLIC ole32 oleaut32 wbemuuid)
brosys_warnings(brosys_win_oracle)

foreach(t power audio network)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/win/test_win_${t}.cpp)
        brosys_test(test_win_${t} SOURCES win/test_win_${t}.cpp LIBS brosys_win_oracle)
    endif()
endforeach()
if(TARGET test_win_power)
    target_link_libraries(test_win_power PRIVATE powrprof user32)
endif()

# A real adapter created and removed by the test (Microsoft KM-TEST Loopback,
# the inbox netloop.inf). Device installation needs elevation: skips otherwise.
brosys_test(test_win_network_adapter SOURCES win/test_win_network_adapter.cpp LIBS brosys_win_oracle TIMEOUT 180)
target_link_libraries(test_win_network_adapter PRIVATE setupapi newdev advapi32)
