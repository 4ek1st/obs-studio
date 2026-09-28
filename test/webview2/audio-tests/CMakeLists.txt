cmake_minimum_required(VERSION 3.28)
project(obs-webview2-audio-tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
enable_testing()
get_filename_component(OBS_SOURCE_ROOT "../../.." ABSOLUTE)
set(OBS_AUDIO_TEST_BUILD "${OBS_SOURCE_ROOT}/build_webview2_baseline" CACHE PATH "Existing isolated OBS build for import libraries")
set(OBS_AUDIO_TEST_DEPS "${OBS_SOURCE_ROOT}/.deps/obs-deps-2026-08-26-x64/bin" CACHE PATH "OBS dependency DLL directory")
add_executable(audio-test ../audio-test.cpp "${OBS_SOURCE_ROOT}/frontend/webview2/AudioMixerBridge.cpp")
target_include_directories(audio-test PRIVATE
  "${OBS_SOURCE_ROOT}/frontend/webview2"
  "${OBS_SOURCE_ROOT}/frontend"
  "${OBS_SOURCE_ROOT}/frontend/api"
  "${OBS_SOURCE_ROOT}/libobs"
  "${OBS_SOURCE_ROOT}/shared/qt/slider-ignorewheel"
  "${OBS_SOURCE_ROOT}/shared/qt/idian/include"
  "${OBS_AUDIO_TEST_BUILD}/config")
target_compile_definitions(audio-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(audio-test PRIVATE Qt6::Widgets
  "${OBS_AUDIO_TEST_BUILD}/libobs/RelWithDebInfo/obs.lib"
  "${OBS_AUDIO_TEST_BUILD}/frontend/api/RelWithDebInfo/obs-frontend-api.lib")
add_test(NAME audio-adapter COMMAND audio-test)
set_tests_properties(audio-adapter PROPERTIES ENVIRONMENT_MODIFICATION
  "PATH=path_list_prepend:${OBS_AUDIO_TEST_BUILD}/rundir/RelWithDebInfo/bin/64bit;PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:${OBS_AUDIO_TEST_DEPS}")
