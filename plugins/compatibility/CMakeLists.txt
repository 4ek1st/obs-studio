cmake_minimum_required(VERSION 3.28)
project(obs33-plugin-compatibility LANGUAGES C CXX RC)

# Separate, opt-in build against the exact libobs already built by this fork.
# Nothing is downloaded or installed by the normal OBS build.
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This compatibility package is for Windows x64")
endif()
set(OBS_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "OBS source checkout")
set(OBS_BUILD_DIR "${OBS_SOURCE_DIR}/build_webview2_full" CACHE PATH "Existing OBS build")
set(OBS_DEPS_DIR "" CACHE PATH "OBS dependency SDK, including SIMDe and FFmpeg")
list(APPEND CMAKE_MODULE_PATH "${OBS_SOURCE_DIR}/cmake/finders")
list(APPEND CMAKE_PREFIX_PATH "${OBS_BUILD_DIR}/libobs" "${OBS_BUILD_DIR}/deps/w32-pthreads" "${OBS_DEPS_DIR}")
find_package(libobs CONFIG REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)
find_package(Git REQUIRED)

include(FetchContent)
FetchContent_Declare(compat_blur
  GIT_REPOSITORY https://github.com/FiniteSingularity/obs-composite-blur.git
  GIT_TAG 160f7ad4d464e85e24894187244cf71a3913aea9 # v1.5.2
  PATCH_COMMAND "${CMAKE_COMMAND}" "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    "-DPATCH=${CMAKE_CURRENT_LIST_DIR}/patches/obs-composite-blur-obs33.patch"
    -P "${CMAKE_CURRENT_LIST_DIR}/apply-patch.cmake"
  SOURCE_SUBDIR compatibility-build-only)
FetchContent_Declare(compat_shader
  GIT_REPOSITORY https://github.com/exeldro/obs-shaderfilter.git
  GIT_TAG 3517829fdd984a8c655973bf11a31a4852365ab7 # 2.6.0
  PATCH_COMMAND "${CMAKE_COMMAND}" "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    "-DPATCH=${CMAKE_CURRENT_LIST_DIR}/patches/obs-shaderfilter-obs33.patch"
    -P "${CMAKE_CURRENT_LIST_DIR}/apply-patch.cmake"
  SOURCE_SUBDIR compatibility-build-only)
FetchContent_MakeAvailable(compat_blur compat_shader)

function(add_compat_plugin name source version version_template)
  set(PROJECT_NAME "${name}")
  set(PROJECT_FULL_NAME "${name} (OBS 33 compatibility build)")
  set(PROJECT_VERSION "${version}+obs33.1")
  string(REPLACE "." ";" version_parts "${version}")
  list(GET version_parts 0 PROJECT_VERSION_MAJOR)
  list(GET version_parts 1 PROJECT_VERSION_MINOR)
  list(GET version_parts 2 PROJECT_VERSION_PATCH)
  set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/${name}")
  configure_file("${source}/${version_template}" "${generated}/version.h")
  # Composite Blur's tag also ships src/version.h. Match upstream's configure
  # step so quoted includes cannot silently use its unmodified version label.
  string(REGEX REPLACE "\\.in$" "" source_version_header "${version_template}")
  configure_file("${source}/${version_template}" "${source}/${source_version_header}")
  configure_file("${source}/resource.rc.in" "${generated}/resource.rc")
  add_library(${name} MODULE ${ARGN} "${generated}/resource.rc")
  target_link_libraries(${name} PRIVATE OBS::libobs)
  target_include_directories(${name} PRIVATE "${generated}")
  target_compile_features(${name} PRIVATE c_std_11)
  set_target_properties(${name} PROPERTIES PREFIX "")
  install(TARGETS ${name} LIBRARY DESTINATION "plugins/${name}" RUNTIME DESTINATION "plugins/${name}")
  install(DIRECTORY "${source}/data/" DESTINATION "plugins/${name}/data")
  install(FILES "${source}/LICENSE" "${source}/README.md" DESTINATION "plugins/${name}/upstream")
  install(FILES "${CMAKE_CURRENT_LIST_DIR}/patches/${name}-obs33.patch" DESTINATION "plugins/${name}/upstream")
endfunction()

set(blur "${compat_blur_SOURCE_DIR}/src")
add_compat_plugin(obs-composite-blur "${compat_blur_SOURCE_DIR}" 1.5.2 src/version.h.in
  "${blur}/obs-composite-blur-filter.c" "${blur}/obs-composite-blur-plugin.c" "${blur}/obs-utils.c"
  "${blur}/blur/gaussian-kernel.c" "${blur}/blur/gaussian.c" "${blur}/blur/box.c"
  "${blur}/blur/pixelate.c" "${blur}/blur/dual_kawase.c" "${blur}/blur/temporal.c")
add_compat_plugin(obs-shaderfilter "${compat_shader_SOURCE_DIR}" 2.6.0 version.h.in
  "${compat_shader_SOURCE_DIR}/obs-shaderfilter.c")
install(FILES "${CMAKE_CURRENT_LIST_DIR}/README.md" DESTINATION "plugins/obs33-compatibility")
