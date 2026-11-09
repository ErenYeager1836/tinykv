# M1 TCP echo test
add_executable(test_echo test_echo.cpp)
target_compile_options(test_echo PRIVATE ${TINYKV_COMMON_FLAGS})
if(TINYKV_LINK_FLAGS)
    target_link_options(test_echo PRIVATE ${TINYKV_LINK_FLAGS})
endif()
add_test(NAME test_echo COMMAND test_echo $<TARGET_FILE:tinykv>)

# M2 epoll event-loop concurrency test
add_executable(test_epoll test_epoll.cpp)
target_compile_options(test_epoll PRIVATE ${TINYKV_COMMON_FLAGS})
if(TINYKV_LINK_FLAGS)
    target_link_options(test_epoll PRIVATE ${TINYKV_LINK_FLAGS})
endif()
add_test(NAME test_epoll COMMAND test_epoll $<TARGET_FILE:tinykv>)

# M3 RESP2 Parser & Fuzz Unit Test
add_executable(test_resp test_resp.cpp ../src/resp.cpp)
target_compile_options(test_resp PRIVATE ${TINYKV_COMMON_FLAGS})
target_include_directories(test_resp PRIVATE ../src)
if(TINYKV_LINK_FLAGS)
    target_link_options(test_resp PRIVATE ${TINYKV_LINK_FLAGS})
endif()
add_test(NAME test_resp COMMAND test_resp)

# M3 End-to-end Server RESP2 Command Integration Test
add_executable(test_server_resp test_server_resp.cpp)
target_compile_options(test_server_resp PRIVATE ${TINYKV_COMMON_FLAGS})
if(TINYKV_LINK_FLAGS)
    target_link_options(test_server_resp PRIVATE ${TINYKV_LINK_FLAGS})
endif()
add_test(NAME test_server_resp COMMAND test_server_resp $<TARGET_FILE:tinykv>)
