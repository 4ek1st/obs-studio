option(ENABLE_TWITCH_DEVICE_AUTH "Use public-client Twitch device authorization on Windows" OFF)
set(TWITCH_PUBLIC_CLIENTID "" CACHE STRING "Public Client ID for Twitch device authorization")
if(ENABLE_TWITCH_DEVICE_AUTH AND NOT WIN32)
  message(FATAL_ERROR "Twitch device authorization currently requires Windows credential protection")
endif()

if((ENABLE_TWITCH_DEVICE_AUTH OR (TWITCH_CLIENTID AND TWITCH_HASH MATCHES "^(0|[a-fA-F0-9]+)$")) AND TARGET OBS::browser-panels)
  target_sources(obs-studio PRIVATE oauth/TwitchAuth.cpp oauth/TwitchAuth.hpp)
  target_enable_feature(obs-studio "Twitch API connection" TWITCH_ENABLED)
  if(ENABLE_TWITCH_DEVICE_AUTH)
    target_sources(
      obs-studio PRIVATE
      oauth/TwitchDeviceFlow.cpp oauth/TwitchDeviceFlow.hpp
      oauth/TwitchDeviceLogin.cpp oauth/TwitchDeviceLogin.hpp
      oauth/TwitchTokenStore.cpp oauth/TwitchTokenStore.hpp
    )
    target_compile_definitions(obs-studio PRIVATE TWITCH_DEVICE_AUTH)
    set(TWITCH_HASH "0")
  endif()
else()
  target_disable_feature(obs-studio "Twitch API connection")
  set(TWITCH_CLIENTID "")
  set(TWITCH_HASH "0")
endif()
