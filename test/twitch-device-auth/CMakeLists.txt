cmake_minimum_required(VERSION 3.28)
project(obs-twitch-device-auth-tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)
enable_testing()
add_executable(twitch-device-auth-test
  twitch-device-auth-test.cpp
  ../../frontend/oauth/TwitchDeviceFlow.cpp
  ../../frontend/oauth/TwitchDeviceFlow.hpp
  ../../frontend/oauth/TwitchTokenStore.cpp
)
target_include_directories(twitch-device-auth-test PRIVATE ../../frontend/oauth)
target_compile_definitions(twitch-device-auth-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(twitch-device-auth-test PRIVATE Qt6::Core Qt6::Network crypt32)
add_test(NAME twitch-device-auth COMMAND twitch-device-auth-test)
add_executable(twitch-device-dialog-test
  twitch-device-dialog-test.cpp
  ../../frontend/oauth/TwitchDeviceLogin.cpp
  ../../frontend/oauth/TwitchDeviceLogin.hpp
  ../../frontend/oauth/TwitchDeviceFlow.cpp
  ../../frontend/oauth/TwitchDeviceFlow.hpp
)
target_include_directories(twitch-device-dialog-test PRIVATE ../../frontend/oauth)
target_link_libraries(twitch-device-dialog-test PRIVATE Qt6::Network Qt6::Widgets)
add_test(NAME twitch-device-dialog COMMAND twitch-device-dialog-test)
set_tests_properties(twitch-device-dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal" TIMEOUT 25)
add_executable(twitch-native-docks-test
  twitch-native-docks-test.cpp
  ../../frontend/oauth/TwitchNativeDocks.cpp
  ../../frontend/oauth/TwitchNativeDocks.hpp
)
target_include_directories(twitch-native-docks-test PRIVATE ../../frontend/oauth)
target_link_libraries(twitch-native-docks-test PRIVATE Qt6::Network Qt6::Widgets)
add_test(NAME twitch-native-docks COMMAND twitch-native-docks-test)
set_tests_properties(twitch-native-docks PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal" TIMEOUT 25)
add_executable(twitch-native-docks-live
  twitch-native-docks-live.cpp
  ../../frontend/oauth/TwitchNativeDocks.cpp
  ../../frontend/oauth/TwitchNativeDocks.hpp
  ../../frontend/oauth/TwitchTokenStore.cpp
)
target_include_directories(twitch-native-docks-live PRIVATE ../../frontend/oauth)
target_compile_definitions(twitch-native-docks-live PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(twitch-native-docks-live PRIVATE Qt6::Network Qt6::Widgets crypt32)
add_executable(twitch-live-check twitch-live-check.cpp
  ../../frontend/oauth/TwitchDeviceFlow.cpp ../../frontend/oauth/TwitchDeviceFlow.hpp
  ../../frontend/oauth/TwitchTokenStore.cpp)
target_include_directories(twitch-live-check PRIVATE ../../frontend/oauth)
target_compile_definitions(twitch-live-check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(twitch-live-check PRIVATE Qt6::Core Qt6::Network crypt32)
