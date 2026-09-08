cmake_minimum_required(VERSION 3.18)
project(lxqt_policykit_lifecycle_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(AGENT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Agent source tree under test")
find_package(Qt6 6.6 REQUIRED COMPONENTS Widgets Test)
find_package(PolkitQt6-1 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POLKIT REQUIRED IMPORTED_TARGET polkit-agent-1)
add_executable(test_agent_lifecycle
    test_agent_lifecycle.cpp
    agent_fixture.h
    review_cases.h
    lifecycle_cases.h
    attempt_cases.h
    ${AGENT_SOURCE_DIR}/src/policykitagent.cpp
    ${AGENT_SOURCE_DIR}/src/policykitagent.h
    ${AGENT_SOURCE_DIR}/src/policykitagentgui.cpp
    ${AGENT_SOURCE_DIR}/src/policykitagentgui.h
    ${AGENT_SOURCE_DIR}/src/policykitagentgui.ui)
target_include_directories(test_agent_lifecycle PRIVATE
    ${AGENT_SOURCE_DIR}/src ${POLKITQT-1_INCLUDE_DIR})
target_link_libraries(test_agent_lifecycle PRIVATE Qt6::Widgets Qt6::Test
    ${POLKITQT-1_CORE_LIBRARY} ${POLKITQT-1_AGENT_LIBRARY} PkgConfig::POLKIT)
# Only the native helper boundary is interposed. Session/AsyncResult/Qt and
# both complete production translation units are used without test rewriting.
target_link_options(test_agent_lifecycle PRIVATE -Wl,--export-dynamic)
target_compile_options(test_agent_lifecycle PRIVATE -Wall -Wextra -Werror -Wno-deprecated-declarations)
option(ENABLE_SANITIZERS "Instrument the application and test code" OFF)
if(ENABLE_SANITIZERS)
    add_library(lifecycle_sanitizers INTERFACE)
    target_compile_options(lifecycle_sanitizers INTERFACE
        -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    target_link_options(lifecycle_sanitizers INTERFACE -fsanitize=address,undefined)
    target_link_libraries(test_agent_lifecycle PRIVATE lifecycle_sanitizers)
    add_executable(sanitizer_probe sanitizer_probe.cpp)
    target_link_libraries(sanitizer_probe PRIVATE lifecycle_sanitizers)
endif()
enable_testing()
set(CASES cancel_password escape_password close_password cancel_retry escape_retry close_retry
    reject_retry unexpected_retry retry_success retry_limit backend_error backend_info
    external_cancel_password external_cancel_retry cancel_before_completion_delivery
    stale_delivery stale_dialog request_after_native_completion new_request reentrant_result finish_callback other_identity
    multiple_identities multi_prompt synchronous_completion destruction empty_identities
    busy_request native_callback_boundary repeated_cancel dead_identity second_identity_retry)
# stefonarch's cancellation report and tsujan's requested retry question in PR #173.
set(REVIEW_CASES
    review_three_saves_cancel review_three_saves_typed_cancel
    review_three_saves_escape review_three_saves_close
    review_three_saves_multi_identity review_three_saves_retry_cancel
    review_retry_contract review_retry_enter review_second_retry_cancel
    review_backend_error_consent review_empty_submit_is_attempt
    review_replaced_prompt_once review_cancelled_password_stays_cancelled
    review_backend_lockout_respected)
set(ATTEMPT_CASES
    no_response_failure
    no_response_error
    retry_startup_failure
    success_without_response
    cancel_before_first_prompt
    multi_challenge_attempt_budget
    cancel_second_challenge
    unselected_failure_no_budget
    delayed_identity_prompt
    owned_object_churn
    shutdown_owns_wrappers
    stale_backend_acknowledgement
    repeated_retry_answer)
foreach(case IN LISTS CASES REVIEW_CASES ATTEMPT_CASES)
    add_test(NAME ${case} COMMAND test_agent_lifecycle ${case})
    set_tests_properties(${case} PROPERTIES TIMEOUT 10
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;G_DEBUG=fatal-criticals;ASAN_OPTIONS=detect_leaks=0")
endforeach()
set_tests_properties(${REVIEW_CASES} PROPERTIES LABELS "pr-review-173")
set_tests_properties(${ATTEMPT_CASES} PROPERTIES LABELS "attempt-contract")
set_tests_properties(owned_object_churn PROPERTIES TIMEOUT 45)
if(ENABLE_SANITIZERS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME sanitizer_enforcement
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_sanitizers.py
                $<TARGET_FILE:sanitizer_probe>)
    set_tests_properties(sanitizer_enforcement PROPERTIES TIMEOUT 40 LABELS "sanitizer-control")
endif()
