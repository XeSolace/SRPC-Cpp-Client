cmake_minimum_required(VERSION 3.24)

project(SRPCClient VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "SRPCClient currently supports Windows only")
endif()

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(SRPC_BUILD_EXAMPLES "Build SRPCClient examples" ON)

add_library(srpc_client STATIC)
add_library(srpc::client ALIAS srpc_client)

target_sources(srpc_client
    PRIVATE
        src/client.cpp
        src/client_memory.cpp
        src/client_transfer.cpp
        src/client_debug.cpp
        src/client_xbdm.cpp
        src/connection.cpp
        src/debug_protocol.cpp
        src/discovery.cpp
        src/error.cpp
        src/image.cpp
        src/protocol.cpp
        src/screenshot.cpp
        src/connection.hpp
        src/debug_protocol.hpp
        src/protocol.hpp
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS include
        FILES
            include/srpc/client.hpp
            include/srpc/debug.hpp
            include/srpc/discovery.hpp
            include/srpc/error.hpp
            include/srpc/image.hpp
            include/srpc/screenshot.hpp
            include/srpc/transfer.hpp
            include/srpc/types.hpp
)

target_compile_features(srpc_client PUBLIC cxx_std_20)
target_compile_definitions(srpc_client PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(srpc_client PUBLIC ws2_32 iphlpapi)

set_target_properties(srpc_client PROPERTIES
    CXX_EXTENSIONS OFF
    DEBUG_POSTFIX d
    EXPORT_NAME client
    OUTPUT_NAME srpc_client
)

if(MSVC)
    target_compile_options(srpc_client PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
endif()

if(SRPC_BUILD_EXAMPLES)
    add_executable(srpc_basic_example examples/basic.cpp)
    target_link_libraries(srpc_basic_example PRIVATE srpc::client)
    set_target_properties(srpc_basic_example PROPERTIES CXX_EXTENSIONS OFF)
endif()

if(BUILD_TESTING)
    add_executable(srpc_protocol_tests tests/protocol_tests.cpp)
    target_link_libraries(srpc_protocol_tests PRIVATE srpc::client)
    target_include_directories(srpc_protocol_tests PRIVATE src)
    set_target_properties(srpc_protocol_tests PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(srpc_protocol_tests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    endif()
    add_test(NAME srpc_protocol_tests COMMAND srpc_protocol_tests)

    add_executable(srpc_debug_protocol_tests tests/debug_protocol_tests.cpp)
    target_link_libraries(srpc_debug_protocol_tests PRIVATE srpc::client)
    target_include_directories(srpc_debug_protocol_tests PRIVATE src)
    set_target_properties(srpc_debug_protocol_tests PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(srpc_debug_protocol_tests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    endif()
    add_test(NAME srpc_debug_protocol_tests COMMAND srpc_debug_protocol_tests)

    add_executable(srpc_image_tests tests/image_tests.cpp)
    target_link_libraries(srpc_image_tests PRIVATE srpc::client)
    set_target_properties(srpc_image_tests PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(srpc_image_tests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    endif()
    add_test(NAME srpc_image_tests COMMAND srpc_image_tests)

    add_executable(srpc_screenshot_tests tests/screenshot_tests.cpp)
    target_link_libraries(srpc_screenshot_tests PRIVATE srpc::client)
    set_target_properties(srpc_screenshot_tests PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(srpc_screenshot_tests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    endif()
    add_test(NAME srpc_screenshot_tests COMMAND srpc_screenshot_tests)

    add_executable(srpc_integration_tests tests/client_integration_tests.cpp)
    target_link_libraries(srpc_integration_tests PRIVATE srpc::client)
    set_target_properties(srpc_integration_tests PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(srpc_integration_tests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    endif()
    add_test(NAME srpc_integration_tests COMMAND srpc_integration_tests)
endif()

install(TARGETS srpc_client
    EXPORT SRPCClientTargets
    FILE_SET public_headers DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(EXPORT SRPCClientTargets
    FILE SRPCClientTargets.cmake
    NAMESPACE srpc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SRPCClient
)

configure_package_config_file(
    cmake/SRPCClientConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/SRPCClientConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SRPCClient
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/SRPCClientConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/SRPCClientConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/SRPCClientConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SRPCClient
)
