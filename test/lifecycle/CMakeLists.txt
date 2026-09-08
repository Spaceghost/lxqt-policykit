cmake_minimum_required(VERSION 3.18)
project(lxqt_policykit_lifecycle_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(AGENT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Agent source tree under test")
find_package(Qt6 6.6 REQUIRED COMPONENTS Widgets)
find_package(PolkitQt6-1 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POLKIT REQUIRED IMPORTED_TARGET polkit-agent-1)
add_executable(test_agent_lifecycle
    test_agent_lifecycle.cpp
    ${AGENT_SOURCE_DIR}/src/policykitagent.cpp
    ${AGENT_SOURCE_DIR}/src/policykitagent.h
    ${AGENT_SOURCE_DIR}/src/policykitagentgui.cpp
    ${AGENT_SOURCE_DIR}/src/policykitagentgui.h
    ${AGENT_SOURCE_DIR}/src/policykitagentgui.ui)
target_include_directories(test_agent_lifecycle PRIVATE
    ${AGENT_SOURCE_DIR}/src ${POLKITQT-1_INCLUDE_DIR})
target_link_libraries(test_agent_lifecycle PRIVATE Qt6::Widgets
    ${POLKITQT-1_CORE_LIBRARY} ${POLKITQT-1_AGENT_LIBRARY} PkgConfig::POLKIT)
# Only the native helper boundary is interposed. Session/AsyncResult/Qt and
# both complete production translation units are used without test rewriting.
target_link_options(test_agent_lifecycle PRIVATE -Wl,--export-dynamic)
target_compile_options(test_agent_lifecycle PRIVATE -Wall -Wextra -Werror -Wno-deprecated-declarations)
option(ENABLE_SANITIZERS "Instrument the application and test code" OFF)
if(ENABLE_SANITIZERS)
    target_compile_options(test_agent_lifecycle PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(test_agent_lifecycle PRIVATE -fsanitize=address,undefined)
endif()
enable_testing()
set(CASES cancel_password escape_password close_password cancel_retry escape_retry close_retry
    reject_retry unexpected_retry retry_success retry_limit backend_error backend_info
    external_cancel_password external_cancel_retry cancel_before_completion_delivery
    stale_delivery stale_dialog request_after_native_completion new_request reentrant_result finish_callback other_identity
    multiple_identities multi_prompt synchronous_completion destruction empty_identities
    busy_request native_callback_boundary repeated_cancel dead_identity second_identity_retry)
foreach(case IN LISTS CASES)
    add_test(NAME ${case} COMMAND test_agent_lifecycle ${case})
    set_tests_properties(${case} PROPERTIES TIMEOUT 10
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;G_DEBUG=fatal-criticals;ASAN_OPTIONS=detect_leaks=0")
endforeach()
