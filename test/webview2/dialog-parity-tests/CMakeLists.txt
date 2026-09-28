cmake_minimum_required(VERSION 3.28)
project(obs-dialog-parity-tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
get_filename_component(root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include("${root}/cmake/windows/webview2.cmake")
obs_find_webview2()
add_executable(dialog-parity-test ../dialog-parity-test.cpp
  "${root}/frontend/webview2/QtDialogBridge.cpp"
  "${root}/frontend/webview2/WebView2Widget.cpp"
  "${root}/frontend/webview2/WebView2Widget.hpp"
  "${root}/frontend/webview2/BridgeProtocol.cpp"
  "${root}/frontend/webview2/ExternalDrop.cpp"
  "${root}/frontend/components/ClickableLabel.hpp"
  "${root}/shared/qt/obs-spinbox/AutoSelectLineEdit.cpp"
  "${root}/shared/qt/obs-spinbox/AutoSelectLineEdit.hpp"
  "${root}/shared/qt/obs-spinbox/SpinBox.cpp"
  "${root}/shared/qt/obs-spinbox/SpinBox.hpp"
  "${root}/shared/qt/obs-spinbox/DoubleSpinBox.cpp"
  "${root}/shared/qt/obs-spinbox/DoubleSpinBox.hpp")
target_include_directories(dialog-parity-test PRIVATE "${root}/frontend/webview2"
  "${root}/frontend/components" "${root}/shared/qt/obs-spinbox")
target_link_libraries(dialog-parity-test PRIVATE Qt6::Widgets OBS::WebView2)
target_compile_definitions(dialog-parity-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(dialog-parity-test PRIVATE OBS_DIALOG_TEST_FONT="${root}/frontend/forms/fonts/OpenSans-Regular.ttf")
enable_testing()
add_test(NAME dialog-parity COMMAND dialog-parity-test)
add_executable(dialog-lifecycle-test ../dialog-lifecycle-test.cpp
  "${root}/frontend/webview2/QtDialogBridge.cpp"
  "${root}/frontend/webview2/WebView2Widget.cpp"
  "${root}/frontend/webview2/WebView2Widget.hpp"
  "${root}/frontend/webview2/BridgeProtocol.cpp"
  "${root}/frontend/webview2/ExternalDrop.cpp")
target_include_directories(dialog-lifecycle-test PRIVATE "${root}/frontend/webview2")
target_link_libraries(dialog-lifecycle-test PRIVATE Qt6::Widgets OBS::WebView2)
target_compile_definitions(dialog-lifecycle-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME dialog-lifecycle COMMAND dialog-lifecycle-test "${root}/frontend/webview2/ui")
add_executable(dialog-presentation-test ../dialog-presentation-test.cpp
  "${root}/frontend/webview2/QtDialogBridge.cpp"
  "${root}/frontend/webview2/WebView2Widget.cpp"
  "${root}/frontend/webview2/WebView2Widget.hpp"
  "${root}/frontend/webview2/BridgeProtocol.cpp"
  "${root}/frontend/webview2/ExternalDrop.cpp")
target_include_directories(dialog-presentation-test PRIVATE "${root}/frontend/webview2")
target_link_libraries(dialog-presentation-test PRIVATE Qt6::Widgets OBS::WebView2 dwmapi)
target_compile_definitions(dialog-presentation-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
  OBS_DIALOG_TEST_FONT="${root}/frontend/forms/fonts/OpenSans-Regular.ttf")
# Composed screen sampling needs an unobstructed interactive desktop. Run this
# executable explicitly, with an evidence directory, instead of on headless CI.
set_tests_properties(dialog-parity dialog-lifecycle PROPERTIES
  ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
  ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>")
