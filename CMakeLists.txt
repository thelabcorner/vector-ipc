cmake_minimum_required(VERSION 3.20)

project(VectorIPC VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(VIPC_BUILD_TESTS "Build VectorIPC tests" ON)
option(VIPC_BUILD_BENCHMARKS "Build VectorIPC microbenchmarks" ON)
option(VIPC_BUILD_EXTERNALOBJECT "Build the ExtendScript ExternalObject adapter" ON)
option(VIPC_BUILD_CPP_TESTS "Build C++17 façade tests" ON)

if(VIPC_BUILD_TESTS AND VIPC_BUILD_CPP_TESTS)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

add_library(vectoripc STATIC
    src/core/vipc_protocol.c
    src/core/vipc_status.c
)
add_library(VectorIPC::vectoripc ALIAS vectoripc)

target_include_directories(vectoripc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
    target_sources(vectoripc PRIVATE
        src/platform/win/vipc_win_pipe.c
    )
    target_link_libraries(vectoripc PUBLIC advapi32)
    target_compile_definitions(vectoripc PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0601
    )
else()
    target_sources(vectoripc PRIVATE
        src/platform/posix/vipc_posix_unimplemented.c
    )
endif()

if(MSVC)
    target_compile_options(vectoripc PRIVATE /W4 /WX /sdl /guard:cf)
    target_link_options(vectoripc INTERFACE /guard:cf)
else()
    target_compile_options(vectoripc PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion
    )
endif()

if(WIN32 AND VIPC_BUILD_EXTERNALOBJECT)
    add_library(vectoripc_externalobject SHARED
        src/adapters/externalobject/vipc_externalobject.c
    )
    target_link_libraries(vectoripc_externalobject PRIVATE vectoripc)
    target_include_directories(vectoripc_externalobject PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/externalobject
    )
    target_compile_definitions(vectoripc_externalobject PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0601
    )
    set_target_properties(vectoripc_externalobject PROPERTIES
        OUTPUT_NAME "VectorIPCExternalObject"
        PREFIX ""
    )
    if(MSVC)
        target_compile_options(vectoripc_externalobject PRIVATE /W4 /WX /sdl /guard:cf)
        target_link_options(vectoripc_externalobject PRIVATE /guard:cf)
    endif()
endif()

if(VIPC_BUILD_TESTS)
    enable_testing()

    add_executable(vectoripc_selftest
        tests/selftest.c
    )
    target_link_libraries(vectoripc_selftest PRIVATE vectoripc)
    target_include_directories(vectoripc_selftest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    if(MSVC)
        target_compile_options(vectoripc_selftest PRIVATE /W4 /WX /sdl /guard:cf)
        target_link_options(vectoripc_selftest PRIVATE /guard:cf)
    endif()

    add_test(NAME vectoripc_selftest COMMAND vectoripc_selftest)

    add_executable(vectoripc_protocol_fuzz
        tests/protocol_fuzz.c
    )
    target_link_libraries(vectoripc_protocol_fuzz PRIVATE vectoripc)
    target_include_directories(vectoripc_protocol_fuzz PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    add_executable(vectoripc_full_duplex_test
        tests/full_duplex.c
    )
    target_link_libraries(vectoripc_full_duplex_test PRIVATE vectoripc)
    target_include_directories(vectoripc_full_duplex_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    add_executable(vectoripc_transport_stress
        tests/transport_stress.c
    )
    target_link_libraries(vectoripc_transport_stress PRIVATE vectoripc)
    target_include_directories(vectoripc_transport_stress PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    add_executable(vectoripc_transport_boundaries
        tests/transport_boundaries.c
    )
    target_link_libraries(vectoripc_transport_boundaries PRIVATE vectoripc)
    target_include_directories(vectoripc_transport_boundaries PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    add_executable(vectoripc_timeout_stress
        tests/timeout_stress.c
    )
    target_link_libraries(vectoripc_timeout_stress PRIVATE vectoripc)
    target_include_directories(vectoripc_timeout_stress PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    add_executable(vectoripc_full_duplex_stress
        tests/full_duplex_stress.c
    )
    target_link_libraries(vectoripc_full_duplex_stress PRIVATE vectoripc)
    target_include_directories(vectoripc_full_duplex_stress PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    add_executable(vectoripc_transport_soak
        tests/transport_soak.c
    )
    target_link_libraries(vectoripc_transport_soak PRIVATE vectoripc)
    target_include_directories(vectoripc_transport_soak PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    add_executable(vectoripc_cancellation_stress
        tests/cancellation_stress.c
    )
    target_link_libraries(vectoripc_cancellation_stress PRIVATE vectoripc)
    target_include_directories(vectoripc_cancellation_stress PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    if(MSVC)
        foreach(vipc_test_target
            vectoripc_protocol_fuzz
            vectoripc_full_duplex_test
            vectoripc_transport_stress
            vectoripc_transport_boundaries
            vectoripc_timeout_stress
            vectoripc_full_duplex_stress
            vectoripc_transport_soak
            vectoripc_cancellation_stress)
            target_compile_options(${vipc_test_target} PRIVATE /W4 /WX /sdl /guard:cf)
            target_link_options(${vipc_test_target} PRIVATE /guard:cf)
        endforeach()
    endif()

    add_test(NAME vectoripc_protocol_fuzz COMMAND vectoripc_protocol_fuzz)
    add_test(NAME vectoripc_full_duplex_test COMMAND vectoripc_full_duplex_test)
    add_test(NAME vectoripc_transport_stress COMMAND vectoripc_transport_stress)
    add_test(NAME vectoripc_transport_boundaries COMMAND vectoripc_transport_boundaries)
    add_test(NAME vectoripc_timeout_stress COMMAND vectoripc_timeout_stress)
    add_test(NAME vectoripc_full_duplex_stress COMMAND vectoripc_full_duplex_stress)
    add_test(NAME vectoripc_cancellation_stress COMMAND vectoripc_cancellation_stress)

    if(VIPC_BUILD_CPP_TESTS)
        add_executable(vectoripc_cpp_api
            tests/cpp_api.cpp
        )
        target_link_libraries(vectoripc_cpp_api PRIVATE vectoripc)
        target_include_directories(vectoripc_cpp_api PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        )
        if(MSVC)
            target_compile_options(vectoripc_cpp_api PRIVATE /W4 /WX /sdl /guard:cf)
            target_link_options(vectoripc_cpp_api PRIVATE /guard:cf)
        else()
            target_compile_options(vectoripc_cpp_api PRIVATE
                -Wall -Wextra -Wpedantic -Wshadow -Wconversion
            )
        endif()
        add_test(NAME vectoripc_cpp_api COMMAND vectoripc_cpp_api)
    endif()

    if(WIN32 AND VIPC_BUILD_EXTERNALOBJECT)
        add_executable(vectoripc_externalobject_smoke
            tests/externalobject_smoke.c
        )
        target_link_libraries(vectoripc_externalobject_smoke PRIVATE vectoripc)
        target_include_directories(vectoripc_externalobject_smoke PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/externalobject
        )
        if(MSVC)
            target_compile_options(vectoripc_externalobject_smoke PRIVATE /W4 /WX /sdl /guard:cf)
            target_link_options(vectoripc_externalobject_smoke PRIVATE /guard:cf)
        endif()
        add_test(
            NAME vectoripc_externalobject_smoke
            COMMAND vectoripc_externalobject_smoke
                $<TARGET_FILE:vectoripc_externalobject>
        )

        add_executable(vectoripc_externalobject_sessions
            tests/externalobject_sessions.c
        )
        target_link_libraries(vectoripc_externalobject_sessions PRIVATE
            vectoripc
            vectoripc_externalobject
        )
        target_include_directories(vectoripc_externalobject_sessions PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/externalobject
        )
        if(MSVC)
            target_compile_options(vectoripc_externalobject_sessions PRIVATE /W4 /WX /sdl /guard:cf)
            target_link_options(vectoripc_externalobject_sessions PRIVATE /guard:cf)
        endif()
        add_test(
            NAME vectoripc_externalobject_sessions
            COMMAND vectoripc_externalobject_sessions
        )
    endif()
endif()

if(VIPC_BUILD_BENCHMARKS)
    add_executable(vectoripc_ping_bench
        benchmarks/ping_bench.c
    )
    target_link_libraries(vectoripc_ping_bench PRIVATE vectoripc)
    target_include_directories(vectoripc_ping_bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    if(MSVC)
        target_compile_options(vectoripc_ping_bench PRIVATE /W4 /WX /sdl /guard:cf)
        target_link_options(vectoripc_ping_bench PRIVATE /guard:cf)
    endif()

    add_executable(vectoripc_matrix_bench
        benchmarks/matrix_bench.c
    )
    target_link_libraries(vectoripc_matrix_bench PRIVATE vectoripc)
    target_include_directories(vectoripc_matrix_bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    if(MSVC)
        target_compile_options(vectoripc_matrix_bench PRIVATE /W4 /WX /sdl /guard:cf)
        target_link_options(vectoripc_matrix_bench PRIVATE /guard:cf)
    endif()
endif()

install(
    TARGETS vectoripc
    EXPORT VectorIPCTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    DIRECTORY include/vectoripc
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    FILES LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/VectorIPC
)

if(TARGET vectoripc_externalobject)
    install(
        TARGETS vectoripc_externalobject
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(
        FILES
            src/adapters/externalobject/VectorIPC.jsx
            src/adapters/externalobject/VectorIPC.jsxinc
        DESTINATION ${CMAKE_INSTALL_DATADIR}/vectoripc/externalobject
    )
endif()

configure_package_config_file(
    cmake/VectorIPCConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/VectorIPCConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VectorIPC
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/VectorIPCConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)

install(
    EXPORT VectorIPCTargets
    FILE VectorIPCTargets.cmake
    NAMESPACE VectorIPC::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VectorIPC
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/VectorIPCConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/VectorIPCConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VectorIPC
)