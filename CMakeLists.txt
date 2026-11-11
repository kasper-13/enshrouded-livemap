cmake_minimum_required(VERSION 3.28)
project(enshrouded_livemap VERSION 1.9.2 LANGUAGES CXX ASM_MASM) # x-release-please-version
if(MSVC AND MSVC_VERSION LESS 1950)
    message(FATAL_ERROR "livemap needs Visual Studio 2026 (MSVC 14.50) or newer; found MSVC ${MSVC_VERSION}")
endif()
set(LIVEMAP_LOCAL_SUFFIX "" CACHE STRING "Optional local compatibility build identifier")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the Wine prefix of the server image needs no extra runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE)
# Declared before safetyhook: the first FetchContent_Declare wins, so safetyhook's own Zydis tag is ignored.
include(cmake/zydis.cmake)
FetchContent_Declare(
    safetyhook
    GIT_REPOSITORY https://github.com/cursey/safetyhook.git
    # renovate: depName=cursey/safetyhook datasource=github-tags
    GIT_TAG 19223663fb8a573253ffb2e82da87cc354bf5c16 # v0.7.0
)
FetchContent_MakeAvailable(safetyhook)

include(cmake/builds.cmake)
set(LIVEMAP_BUILDS_JSON "${CMAKE_CURRENT_SOURCE_DIR}/data/builds.json")
set(LIVEMAP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LIVEMAP_BUILDS_JSON}")
livemap_generate_builds("${LIVEMAP_BUILDS_JSON}" "${LIVEMAP_GENERATED_DIR}/builds_generated.h")

add_library(livemap_core STATIC src/scan.cpp src/finds.cpp src/link.cpp src/text.cpp src/bases.cpp src/a2s.cpp src/discovered.cpp src/explored.cpp src/fault.cpp)
target_include_directories(livemap_core PUBLIC src "${LIVEMAP_GENERATED_DIR}")

# The DLL is named iphlpapi.dll because the server loads it in place of the real one.
add_library(iphlpapi SHARED
    src/main.cpp
    src/config.cpp
    src/log.cpp
    src/memory.cpp
    src/fault_handler.cpp
    src/ecs.cpp
    src/world.cpp
    src/web.cpp
    src/announce.cpp
    src/public_ip.cpp
    src/proxy.cpp
    src/proxy_stubs.asm
    src/proxy.def
)
target_link_libraries(iphlpapi PRIVATE livemap_core safetyhook::safetyhook ws2_32 bcrypt winhttp)
target_compile_definitions(iphlpapi PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS
    LIVEMAP_VERSION="${PROJECT_VERSION}${LIVEMAP_LOCAL_SUFFIX}")
target_compile_options(iphlpapi PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive->)

# Offline check against an enshrouded_server.exe on disk: would the DLL hook this build?
add_executable(finds_test tests/finds_test.cpp)
target_link_libraries(finds_test PRIVATE livemap_core)
enable_testing()
add_test(NAME finds COMMAND finds_test)
add_executable(link_test tests/link_test.cpp)
target_link_libraries(link_test PRIVATE livemap_core)
add_test(NAME link COMMAND link_test)
add_executable(text_test tests/text_test.cpp)
target_link_libraries(text_test PRIVATE livemap_core)
add_test(NAME text COMMAND text_test)
add_executable(bases_test tests/bases_test.cpp)
target_link_libraries(bases_test PRIVATE livemap_core)
add_test(NAME bases COMMAND bases_test)
add_executable(a2s_test tests/a2s_test.cpp)
target_link_libraries(a2s_test PRIVATE livemap_core)
add_test(NAME a2s COMMAND a2s_test)
add_executable(discovered_test tests/discovered_test.cpp)
target_link_libraries(discovered_test PRIVATE livemap_core)
add_test(NAME discovered COMMAND discovered_test)
add_executable(explored_test tests/explored_test.cpp)
target_link_libraries(explored_test PRIVATE livemap_core)
add_test(NAME explored COMMAND explored_test)
add_executable(fault_test tests/fault_test.cpp)
target_link_libraries(fault_test PRIVATE livemap_core)
add_test(NAME fault_line COMMAND fault_test)
add_executable(ecs_test tests/ecs_test.cpp src/ecs.cpp src/memory.cpp)
target_compile_definitions(ecs_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(ecs_test PRIVATE /UNDEBUG)
add_test(NAME ecs_param_entity COMMAND ecs_test)

add_executable(livemap_selftest tools/selftest.cpp)
target_link_libraries(livemap_selftest PRIVATE livemap_core)
target_compile_definitions(livemap_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

add_executable(builds_test tests/builds_test.cpp)
target_link_libraries(builds_test PRIVATE livemap_core)
add_test(NAME builds_table COMMAND builds_test)
add_test(NAME builds_generator
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/generator_test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/builds_generator_test.cmake")
add_executable(scan_test tests/scan_test.cpp)
target_link_libraries(scan_test PRIVATE livemap_core)
target_compile_definitions(scan_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
add_test(NAME scan_synthetic_builds COMMAND scan_test)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import pytest, jsonschema" RESULT_VARIABLE LIVEMAP_PYTEST_STATUS OUTPUT_QUIET ERROR_QUIET)
if(NOT LIVEMAP_PYTEST_STATUS EQUAL 0)
    message(FATAL_ERROR "pytest or jsonschema is missing from ${Python3_EXECUTABLE}; install them with: "
        "\"${Python3_EXECUTABLE}\" -m pip install --require-hashes --no-deps -r \"${CMAKE_CURRENT_SOURCE_DIR}/tools/requirements-test.txt\"")
endif()
# pytest and jsonschema come from tools/requirements-test.txt; the cache and bytecode stay out of the source tree.
add_test(NAME tools_paths COMMAND "${Python3_EXECUTABLE}" -B -m pytest -q -p no:cacheprovider "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_paths.py")
add_test(NAME tools_hook_check COMMAND "${Python3_EXECUTABLE}" -B -m pytest -q -p no:cacheprovider "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_hook_check.py")
add_test(NAME tools_data_only COMMAND "${Python3_EXECUTABLE}" -B -m pytest -q -p no:cacheprovider "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_data_only.py")
add_test(NAME tools_boot_test COMMAND "${Python3_EXECUTABLE}" -B -m pytest -q -p no:cacheprovider "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_boot_test.py")
add_test(NAME tools_compat_workflow COMMAND "${Python3_EXECUTABLE}" -B -m pytest -q -p no:cacheprovider "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_compat_workflow.py")

# A stand-in for the game (the executable) loads the mod side (the DLL) and faults on purpose, so the two are separate
# images; CI runs the same test under Wine with both files.
add_library(fault_gate_mod SHARED tests/fault_gate_mod.cpp src/fault_handler.cpp src/log.cpp src/memory.cpp)
target_link_libraries(fault_gate_mod PRIVATE livemap_core safetyhook::safetyhook)
target_compile_definitions(fault_gate_mod PRIVATE FAULT_GATE_EXPORTS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(fault_gate_mod PRIVATE /W4 /permissive-)
add_executable(fault_gate tests/fault_gate.cpp)
target_link_libraries(fault_gate PRIVATE fault_gate_mod)
target_compile_definitions(fault_gate PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(fault_gate PRIVATE /W4 /permissive-)
add_test(NAME fault_gate COMMAND "${Python3_EXECUTABLE}" -B -m pytest -q -p no:cacheprovider "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_fault_gate.py")
set_tests_properties(fault_gate PROPERTIES ENVIRONMENT "FAULT_GATE_EXE=$<TARGET_FILE:fault_gate>")

add_executable(mining_test tests/mining_test.cpp)
add_test(NAME mining_register_layouts COMMAND mining_test)
# assert() checks must stay on in Release.
target_compile_options(mining_test PRIVATE /UNDEBUG)
add_executable(compat_test tests/compat_test.cpp)
target_link_libraries(compat_test PRIVATE livemap_core)
target_compile_definitions(compat_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(compat_test PRIVATE /UNDEBUG)
set(LIVEMAP_TEST_OLD_EXE "" CACHE FILEPATH "Private Steam 26083076 fixture")
set(LIVEMAP_TEST_NEW_EXE "" CACHE FILEPATH "Private Steam 26319703 fixture")
set(LIVEMAP_TEST_HOTFIX_EXE "" CACHE FILEPATH "Private Steam 26404334 fixture")
if(LIVEMAP_TEST_OLD_EXE AND LIVEMAP_TEST_NEW_EXE)
    add_test(NAME old_build_hook_sites COMMAND livemap_selftest "${LIVEMAP_TEST_OLD_EXE}")
    add_test(NAME patch1_hook_sites COMMAND livemap_selftest "${LIVEMAP_TEST_NEW_EXE}")
    if(LIVEMAP_TEST_HOTFIX_EXE)
        add_test(NAME hotfix4_hook_sites COMMAND livemap_selftest "${LIVEMAP_TEST_HOTFIX_EXE}")
        add_test(NAME executable_guard_regressions COMMAND compat_test "${LIVEMAP_TEST_OLD_EXE}" "${LIVEMAP_TEST_NEW_EXE}" "${LIVEMAP_TEST_HOTFIX_EXE}")
    else()
        add_test(NAME executable_guard_regressions COMMAND compat_test "${LIVEMAP_TEST_OLD_EXE}" "${LIVEMAP_TEST_NEW_EXE}")
    endif()
endif()

add_subdirectory(tools/compat)
