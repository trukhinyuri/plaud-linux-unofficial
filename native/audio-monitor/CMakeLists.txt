# Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
# SPDX-License-Identifier: Apache-2.0
#
# Linux AudioMonitor addon (audio_monitor_linux.node) for Plaud Desktop.
# Built with cmake-js: `npm run build` -> build/Release/audio_monitor_linux.node

cmake_minimum_required(VERSION 3.16)
project(audio_monitor_linux VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The SPA headers use C99 compound literals inside inline functions; GNU
# extensions (gnu++17) are required to compile them as C++.
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3>=0.3.48)
find_package(Threads REQUIRED)

# --- core library: everything except the N-API glue, shared with the selftest
add_library(audio_monitor_core STATIC
  src/json.cc
  src/proc_util.cc
  src/app_catalog.cc
  src/app_identity.cc
  src/webrtc_evidence.cc
  src/window_titles.cc
  src/meeting_state.cc
  src/meeting_detector.cc
  src/voice_selector.cc
  src/speaker_selector.cc
  src/device_filter.cc
  src/pw_engine.cc
  src/meter_stream.cc
  src/mic_monitor.cc
  src/speaker_monitor.cc
)
target_include_directories(audio_monitor_core PUBLIC src)
target_link_libraries(audio_monitor_core PUBLIC PkgConfig::PIPEWIRE Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(audio_monitor_core PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -Wno-pedantic)
target_compile_definitions(audio_monitor_core PUBLIC AUDIO_MONITOR_VERSION="${PROJECT_VERSION}")

# --- N-API addon
add_definitions(-DNAPI_VERSION=8 -DNAPI_DISABLE_CPP_EXCEPTIONS -DNODE_ADDON_API_DISABLE_DEPRECATED)
include_directories(${CMAKE_JS_INC})
execute_process(
  COMMAND node -p "require('node-addon-api').include"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE NODE_ADDON_API_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REPLACE "\"" "" NODE_ADDON_API_DIR "${NODE_ADDON_API_DIR}")

add_library(${PROJECT_NAME} SHARED src/addon.cc ${CMAKE_JS_SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" SUFFIX ".node" OUTPUT_NAME "audio_monitor_linux")
target_include_directories(${PROJECT_NAME} PRIVATE ${NODE_ADDON_API_DIR})
target_link_libraries(${PROJECT_NAME} PRIVATE audio_monitor_core ${CMAKE_JS_LIB})
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter)
# Keep the addon self-contained: no undefined symbols except Node/N-API ones,
# hide everything that is not the module entry point.
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--exclude-libs,ALL -Wl,--as-needed)

# --- native selftest (pure-logic unit tests run by `npm test`)
add_executable(audio_monitor_selftest test/native/selftest.cc)
target_link_libraries(audio_monitor_selftest PRIVATE audio_monitor_core)
target_compile_options(audio_monitor_selftest PRIVATE -Wall -Wextra -Wno-unused-parameter)

# --- browser stand-in for the integration tests: an executable named "chrome"
add_executable(audio_monitor_fake_chrome test/native/fake_chrome.c)
target_compile_options(audio_monitor_fake_chrome PRIVATE -Wall -Wextra)
set_target_properties(audio_monitor_fake_chrome PROPERTIES OUTPUT_NAME chrome)

# Test executables next to the addon (cmake-js sets CMAKE_LIBRARY_OUTPUT_DIRECTORY
# to build/Release): build/Release/audio_monitor_selftest, build/Release/test-bin/chrome.
if(CMAKE_LIBRARY_OUTPUT_DIRECTORY)
  set(AM_TEST_OUTPUT_DIR "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
else()
  set(AM_TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}")
endif()
set_target_properties(audio_monitor_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${AM_TEST_OUTPUT_DIR}")
set_target_properties(audio_monitor_fake_chrome PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${AM_TEST_OUTPUT_DIR}/test-bin")
