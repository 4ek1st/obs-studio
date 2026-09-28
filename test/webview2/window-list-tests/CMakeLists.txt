cmake_minimum_required(VERSION 3.28)
project(obs-window-list-tests LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "The window enumeration regression test requires Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(OBS_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(OBS_BUILD_DIR "${OBS_SOURCE_DIR}/build_webview2_full" CACHE PATH "Existing OBS build with libobs import library")
set(OBS_BUILD_CONFIGURATION "RelWithDebInfo" CACHE STRING "Configuration of the existing libobs build")
set(OBS_DEPS_DIR "${OBS_SOURCE_DIR}/.deps/obs-deps-2026-08-26-x64" CACHE PATH "OBS dependency package")
set(OBS_LIBRARY "${OBS_BUILD_DIR}/libobs/${OBS_BUILD_CONFIGURATION}/obs.lib" CACHE FILEPATH "Existing libobs import library")

if(NOT EXISTS "${OBS_LIBRARY}")
  message(FATAL_ERROR "Build libobs first or set OBS_LIBRARY to its existing import library.")
endif()

# Compile the production enumerator directly so this isolated regression runs
# against the edited source without replacing any running OBS DLL.
add_executable(window-list-test
  ../window-list-test.cpp
  ${OBS_SOURCE_DIR}/libobs/util/windows/window-helpers.c
  ${OBS_SOURCE_DIR}/libobs/util/windows/obfuscate.c
)
target_include_directories(window-list-test PRIVATE
  ${OBS_SOURCE_DIR}/libobs
  ${OBS_BUILD_DIR}/config
  ${OBS_DEPS_DIR}/include
)
target_compile_definitions(window-list-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(window-list-test PRIVATE "${OBS_LIBRARY}" dwmapi psapi)

enable_testing()
add_test(NAME window-list-selectors COMMAND window-list-test)
set_tests_properties(window-list-selectors PROPERTIES TIMEOUT 30)
