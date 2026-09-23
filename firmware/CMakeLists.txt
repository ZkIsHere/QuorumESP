# QuorumESP firmware — ESP-IDF project scaffold (Phase 2 ready, no protocol yet).
#
# Layout follows AGENTS.md §6. Each subdir is an IDF component, registered
# via EXTRA_COMPONENT_DIRS so the core never hard-depends on one file tree.
cmake_minimum_required(VERSION 3.16)

# IDF_PATH must point at a local ESP-IDF checkout (v5.x recommended).
# Windows: install via the Espressif online installer, then use the
# "ESP-IDF PowerShell" environment before running idf.py.
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/qdevice
    ${CMAKE_CURRENT_SOURCE_DIR}/quorum
    ${CMAKE_CURRENT_SOURCE_DIR}/network
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/storage
    ${CMAKE_CURRENT_SOURCE_DIR}/watchdog
    ${CMAKE_CURRENT_SOURCE_DIR}/web
    ${CMAKE_CURRENT_SOURCE_DIR}/ota
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(QuorumESP)
