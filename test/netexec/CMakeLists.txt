# test/netexec/CMakeLists.txt
# Catch2 tests for the netexec networking layer.

set(netexec_test_sources
    ../test_main.cpp
    test_helpers.hpp
    test_netexec_context.cpp
    test_netexec_timer.cpp
    test_netexec_endpoint.cpp
    test_netexec_resolve.cpp
    test_netexec_acceptor.cpp
    test_netexec_connect.cpp
    test_netexec_send_receive.cpp
    test_netexec_cancellation.cpp
    test_netexec_errors.cpp
    test_netexec_concurrency.cpp
    test_netexec_iocp_specific.cpp
    test_netexec_poll_specific.cpp
    test_netexec_udp.cpp
    test_netexec_tls.cpp
    test_let_async_scope.cpp
)

add_executable(test.netexec ${netexec_test_sources})
add_compile_diagnostics(test.netexec)

set_target_properties(test.netexec PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# netexec uses the `stdexec` namespace directly, so do not pull in the
# test-wide STDEXEC_NAMESPACE=std::execution definition from common_test_settings.
target_include_directories(test.netexec PRIVATE "${CMAKE_SOURCE_DIR}/test")

target_link_libraries(test.netexec
    PUBLIC
    STDEXEC::stdexec
    stdexec_executable_flags
    Catch2::Catch2WithMain
)

if(WIN32)
    target_link_libraries(test.netexec PUBLIC ws2_32)
    target_compile_options(test.netexec PRIVATE
        /Zc:__cplusplus /Zc:preprocessor /EHsc
        /permissive- /await:strict /utf-8
        /FS
        $<$<CONFIG:Release>:/Zi>)
    target_link_options(test.netexec PRIVATE
        $<$<CONFIG:Release>:/DEBUG:FULL>)
endif()

catch_discover_tests(test.netexec PROPERTIES TIMEOUT 30)
