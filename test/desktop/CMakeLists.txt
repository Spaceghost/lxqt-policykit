cmake_minimum_required(VERSION 3.18)
project(lxqt_policykit_desktop_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(AGENT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Production source tree")

find_package(Qt6 6.6 REQUIRED COMPONENTS Widgets Test)
find_package(PolkitQt6-1 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POLKIT REQUIRED IMPORTED_TARGET polkit-agent-1)

add_executable(desktop_agent
    desktop_agent.cpp
    ${AGENT_SOURCE_DIR}/src/policykitagent.cpp
    ${AGENT_SOURCE_DIR}/src/policykitagent.h
    ${AGENT_SOURCE_DIR}/src/policykitagentgui.cpp
    ${AGENT_SOURCE_DIR}/src/policykitagentgui.h
    ${AGENT_SOURCE_DIR}/src/policykitagentgui.ui
)
target_include_directories(desktop_agent PRIVATE
    ${AGENT_SOURCE_DIR}/src ${POLKITQT-1_INCLUDE_DIR}
)
target_link_libraries(desktop_agent PRIVATE
    Qt6::Widgets Qt6::Test ${POLKITQT-1_CORE_LIBRARY}
    ${POLKITQT-1_AGENT_LIBRARY} PkgConfig::POLKIT
)
# Reuse only the deterministic native authentication boundary. No GUI operations
# are interposed: the production dialogs communicate with the real display server.
target_link_options(desktop_agent PRIVATE -Wl,--export-dynamic)
target_compile_options(desktop_agent PRIVATE
    -Wall -Wextra -Werror -Wno-deprecated-declarations -Wno-unused-function
)

# Generate bindings from the distribution's protocol definitions, not vendored code.
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_get_variable(WLR_PROTOCOLS_DIR wlr-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(POINTER_XML "${WLR_PROTOCOLS_DIR}/unstable/wlr-virtual-pointer-unstable-v1.xml")
if(NOT EXISTS "${POINTER_XML}")
    message(FATAL_ERROR "The wlr-protocols development package is required")
endif()
add_custom_command(OUTPUT virtual-pointer-client.h
    COMMAND ${WAYLAND_SCANNER} client-header ${POINTER_XML} virtual-pointer-client.h
    DEPENDS ${POINTER_XML} VERBATIM)
add_custom_command(OUTPUT virtual-pointer-protocol.c
    COMMAND ${WAYLAND_SCANNER} private-code ${POINTER_XML} virtual-pointer-protocol.c
    DEPENDS ${POINTER_XML} VERBATIM)
add_executable(desktop_pointer desktop_pointer.c virtual-pointer-client.h virtual-pointer-protocol.c)
set_target_properties(desktop_pointer PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(desktop_pointer PRIVATE PkgConfig::WAYLAND_CLIENT)
target_compile_options(desktop_pointer PRIVATE -Wall -Wextra -Werror)

# A persistent keyboard prevents device/keymap changes from dismissing popups.
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
set(KEYBOARD_XML "${CMAKE_CURRENT_SOURCE_DIR}/virtual-keyboard.xml")
add_custom_command(OUTPUT virtual-keyboard-client.h
    COMMAND ${WAYLAND_SCANNER} client-header ${KEYBOARD_XML} virtual-keyboard-client.h
    DEPENDS ${KEYBOARD_XML} VERBATIM)
add_custom_command(OUTPUT virtual-keyboard-protocol.c
    COMMAND ${WAYLAND_SCANNER} private-code ${KEYBOARD_XML} virtual-keyboard-protocol.c
    DEPENDS ${KEYBOARD_XML} VERBATIM)
add_executable(desktop_keyboard desktop_keyboard.c virtual-keyboard-client.h virtual-keyboard-protocol.c)
set_target_properties(desktop_keyboard PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(desktop_keyboard PRIVATE PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON)
target_compile_options(desktop_keyboard PRIVATE -Wall -Wextra -Werror)
