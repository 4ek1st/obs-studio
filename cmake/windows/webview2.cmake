include_guard(GLOBAL)

function(obs_find_webview2)
  if(TARGET OBS::WebView2)
    return()
  endif()
  get_filename_component(obs_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)
  set(version "1.0.4191.47")
  set(package "${obs_root}/.deps/microsoft.web.webview2.${version}.nupkg")
  set(sdk "${obs_root}/.deps/webview2-${version}")
  if(NOT EXISTS "${sdk}/build/native/include/WebView2.h")
    file(MAKE_DIRECTORY "${obs_root}/.deps" "${sdk}")
    file(
      DOWNLOAD "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${version}/microsoft.web.webview2.${version}.nupkg"
      "${package}"
      EXPECTED_HASH SHA256=f492bbf547d0da329553b6727435b677579b1e9f91cc9e4a1ad029366d5f23d0
      TLS_VERIFY ON
      STATUS result
    )
    list(GET result 0 code)
    if(NOT code EQUAL 0)
      message(FATAL_ERROR "WebView2 SDK download failed: ${result}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${package}" DESTINATION "${sdk}")
  endif()
  if(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
    set(architecture arm64)
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(architecture x64)
  else()
    set(architecture x86)
  endif()
  add_library(OBS::WebView2 STATIC IMPORTED GLOBAL)
  set_target_properties(
    OBS::WebView2
    PROPERTIES
      IMPORTED_LOCATION "${sdk}/build/native/${architecture}/WebView2LoaderStatic.lib"
      INTERFACE_INCLUDE_DIRECTORIES "${sdk}/build/native/include"
      INTERFACE_LINK_LIBRARIES "version;shlwapi;ole32"
  )
endfunction()
