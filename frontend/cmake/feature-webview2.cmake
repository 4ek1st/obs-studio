option(ENABLE_WEBVIEW2_FRONTEND "Build the WebView2 frontend on Windows" ON)
if(NOT ENABLE_WEBVIEW2_FRONTEND)
  return()
endif()

include("${CMAKE_SOURCE_DIR}/cmake/windows/webview2.cmake")
obs_find_webview2()
target_sources(
  obs-studio
  PRIVATE
    webview2/BridgeProtocol.cpp
    webview2/BridgeProtocol.hpp
    webview2/UiGeometry.cpp
    webview2/UiGeometry.hpp
    webview2/AudioMixerBridge.cpp
    webview2/AudioMixerBridge.hpp
    webview2/QtDialogBridge.cpp
    webview2/QtDialogBridge.hpp
    webview2/ExternalDrop.cpp
    webview2/ExternalDrop.hpp
    webview2/WebView2Drop.hpp
    webview2/WebView2Widget.cpp
    webview2/WebView2Widget.hpp
)
target_link_libraries(obs-studio PRIVATE OBS::WebView2)
set_property(SOURCE widgets/OBSBasic.cpp APPEND PROPERTY COMPILE_DEFINITIONS WEBVIEW2_AVAILABLE)
target_sources(obs-studio PRIVATE webview2/OBSWebView2.cpp webview2/OBSWebView2.hpp)
foreach(resource IN ITEMS index.html app.js style.css bridge.mjs dialog.html dialog.js dialog.css external-drop.mjs)
  target_add_resource(obs-studio "${CMAKE_CURRENT_SOURCE_DIR}/webview2/ui/${resource}" "${OBS_DATA_DESTINATION}/obs-studio/webview2")
endforeach()

option(ENABLE_WEBVIEW2_INTEGRATION_TESTS "Enable disposable portable OBS adapter tests" OFF)
if(ENABLE_WEBVIEW2_INTEGRATION_TESTS)
  set_property(SOURCE webview2/OBSWebView2.cpp APPEND PROPERTY COMPILE_DEFINITIONS OBS_WEBVIEW2_INTEGRATION_TESTS)
endif()
