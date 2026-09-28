cmake_minimum_required(VERSION 3.28)
project(obs-control-parity-tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
get_filename_component(root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
enable_testing()

add_executable(output-controls-test ../output-controls-test.cpp
  "${root}/frontend/webview2/UiGeometry.cpp")
add_executable(tbar-native-test ../tbar-native-test.cpp
  "${root}/shared/qt/slider-ignorewheel/slider-ignorewheel.cpp")
add_executable(plugin-audit-dialog-test ../plugin-audit-dialog-test.cpp)

foreach(target output-controls-test tbar-native-test plugin-audit-dialog-test)
  target_include_directories(${target} PRIVATE "${root}/frontend/webview2"
    "${root}/shared/qt/slider-ignorewheel")
  target_link_libraries(${target} PRIVATE Qt6::Widgets)
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>;QT_QPA_PLATFORM=windows"
    TIMEOUT 30)
endforeach()
