# stdexec/examples/netexec/CMakeLists.txt -*-makefile-*-
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Examples for the netexec networking layer (beman/net ported to stdexec).
# This directory can be built stand-alone (PROJECT_IS_TOP_LEVEL) or as part of
# the stdexec examples tree.

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25...4.3)
    project(netexec_examples LANGUAGES CXX)
    find_package(stdexec CONFIG REQUIRED)
endif()

# The netexec headers live next to the stdexec headers under stdexec/include.
add_library(netexec_headers INTERFACE)
target_include_directories(netexec_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
target_link_libraries(netexec_headers INTERFACE STDEXEC::stdexec)

set(NETEXEC_EXAMPLES
    server
    client
    http-server
    http-munich
    http-warwick
    milano
    cppcon-2024
    taps
    cat_image_server
    simple_echo_server
)

foreach(example ${NETEXEC_EXAMPLES})
    set(target netexec.${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE netexec_headers)
    if(TARGET stdexec_executable_flags)
        target_link_libraries(${target} PRIVATE stdexec_executable_flags)
    endif()
    target_compile_features(${target} PRIVATE cxx_std_23)

    if(WIN32)
        target_link_libraries(${target} PRIVATE ws2_32)
        target_compile_options(${target} PRIVATE
            /Zc:__cplusplus
            /Zc:preprocessor
            /EHsc
            /permissive-
            /await:strict
            /utf-8
            /wd4714  # stdexec internal __forceinline functions may not be inlined
        )
    endif()

    # Copy the shared data/ directory next to the executable so HTTP examples
    # can find their assets (index.html, images, favicon, etc.).
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "$<TARGET_FILE_DIR:${target}>/data"
        COMMENT "Copying netexec data for ${target}"
    )
endforeach()
