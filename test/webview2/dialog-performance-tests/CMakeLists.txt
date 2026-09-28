cmake_minimum_required(VERSION 3.28)
project(obs-dialog-performance-tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
get_filename_component(root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include("${root}/cmake/windows/webview2.cmake")
obs_find_webview2()
add_executable(dialog-performance-test ../dialog-performance-test.cpp
  "${root}/frontend/webview2/QtDialogBridge.cpp"
  "${root}/frontend/webview2/WebView2Widget.cpp"
  "${root}/frontend/webview2/WebView2Widget.hpp"
  "${root}/frontend/webview2/BridgeProtocol.cpp"
  "${root}/frontend/webview2/ExternalDrop.cpp")
target_include_directories(dialog-performance-test PRIVATE "${root}/frontend/webview2")
target_link_libraries(dialog-performance-test PRIVATE Qt6::Widgets OBS::WebView2)
target_compile_definitions(dialog-performance-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
enable_testing()
add_test(NAME dialog-performance COMMAND dialog-performance-test)
add_executable(host-startup-test ../host-test.cpp
  "${root}/frontend/webview2/WebView2Widget.cpp"
  "${root}/frontend/webview2/WebView2Widget.hpp"
  "${root}/frontend/webview2/BridgeProtocol.cpp"
  "${root}/frontend/webview2/ExternalDrop.cpp")
target_include_directories(host-startup-test PRIVATE "${root}/frontend/webview2")
target_link_libraries(host-startup-test PRIVATE Qt6::Widgets OBS::WebView2)
target_compile_definitions(host-startup-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME host-startup COMMAND host-startup-test "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")
add_executable(host-native-siblings-test ../host-native-siblings-test.cpp
  "${root}/frontend/webview2/QtDialogBridge.cpp"
  "${root}/frontend/webview2/WebView2Widget.cpp"
  "${root}/frontend/webview2/WebView2Widget.hpp"
  "${root}/frontend/webview2/BridgeProtocol.cpp"
  "${root}/frontend/webview2/ExternalDrop.cpp")
target_include_directories(host-native-siblings-test PRIVATE "${root}/frontend/webview2")
target_link_libraries(host-native-siblings-test PRIVATE Qt6::Widgets OBS::WebView2)
target_compile_definitions(host-native-siblings-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME host-native-siblings COMMAND host-native-siblings-test "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")
add_executable(bridge-protocol-test ../bridge-test.cpp "${root}/frontend/webview2/BridgeProtocol.cpp")
target_include_directories(bridge-protocol-test PRIVATE "${root}/frontend/webview2")
target_link_libraries(bridge-protocol-test PRIVATE Qt6::Core)
add_test(NAME bridge-protocol COMMAND bridge-protocol-test)
add_executable(drop-origin-test ../drop-test.cpp "${root}/frontend/webview2/ExternalDrop.cpp" "${root}/frontend/webview2/BridgeProtocol.cpp")
target_include_directories(drop-origin-test PRIVATE "${root}/frontend/webview2")
target_link_libraries(drop-origin-test PRIVATE Qt6::Widgets)
add_test(NAME drop-origin COMMAND drop-origin-test)
