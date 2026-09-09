cmake_minimum_required(VERSION 3.18)
project(lxqt_policykit_desktop_tests LANGUAGES CXX)

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
