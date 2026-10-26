# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
#
# Builds recorder_linux.node. Driven by cmake-js (`npm run build`), which
# passes CMAKE_JS_INC (Node-API + node-addon-api headers) and the output
# directory. scripts/build-deps.mjs must have produced deps/webrtc-apm.json.

cmake_minimum_required(VERSION 3.19)
project(recorder_linux LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release")
endif()

# --- vendored webrtc-audio-processing 2.1 (static, built by build-deps.mjs) ---
if(DEFINED ENV{PLAUD_DEPS_DIR})
  set(PLAUD_DEPS_DIR "$ENV{PLAUD_DEPS_DIR}")
else()
  set(PLAUD_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps")
endif()
set(WEBRTC_MANIFEST "${PLAUD_DEPS_DIR}/webrtc-apm.json")
if(NOT EXISTS "${WEBRTC_MANIFEST}")
  message(FATAL_ERROR "${WEBRTC_MANIFEST} not found: run `node scripts/build-deps.mjs` (npm run build does).")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WEBRTC_MANIFEST}")
file(READ "${WEBRTC_MANIFEST}" WEBRTC_JSON)

function(json_string_list out_var json key)
  string(JSON count LENGTH "${json}" "${key}")
  set(items "")
  if(count GREATER 0)
    math(EXPR last "${count} - 1")
    foreach(i RANGE ${last})
      string(JSON item GET "${json}" "${key}" ${i})
      list(APPEND items "${item}")
    endforeach()
  endif()
  set(${out_var} "${items}" PARENT_SCOPE)
endfunction()

json_string_list(WEBRTC_INCLUDE_DIRS "${WEBRTC_JSON}" includeDirs)
json_string_list(WEBRTC_DEFINES "${WEBRTC_JSON}" defines)
json_string_list(WEBRTC_LIBS "${WEBRTC_JSON}" libs)
foreach(lib IN LISTS WEBRTC_LIBS)
  if(NOT EXISTS "${lib}")
    message(FATAL_ERROR "Vendored library ${lib} is missing: rerun `node scripts/build-deps.mjs`.")
  endif()
endforeach()

# --- system libraries ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3>=0.3.48)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
pkg_check_modules(OGG REQUIRED IMPORTED_TARGET ogg)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
# Xlib/XRandR headers only: the X11 screenshot backend dlopen()s the libraries.
pkg_check_modules(X11HDR REQUIRED x11 xrandr)
find_package(Threads REQUIRED)

file(GLOB_RECURSE RECORDER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(${PROJECT_NAME} SHARED ${RECORDER_SOURCES} ${CMAKE_JS_SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" SUFFIX ".node")

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" ${CMAKE_JS_INC})
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${WEBRTC_INCLUDE_DIRS} ${X11HDR_INCLUDE_DIRS})
target_compile_definitions(${PROJECT_NAME} PRIVATE NAPI_VERSION=8 NAPI_CPP_EXCEPTIONS ${WEBRTC_DEFINES})
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -fno-strict-aliasing)

# computeAecLightMetrics reproduces the official numbers bit-for-bit only
# without automatic FMA contraction.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/aec_metrics/aec_metrics.cpp"
                            PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

target_link_libraries(${PROJECT_NAME} PRIVATE
  -Wl,--start-group ${WEBRTC_LIBS} -Wl,--end-group
  PkgConfig::PIPEWIRE PkgConfig::OPUS PkgConfig::OGG PkgConfig::DBUS PkgConfig::PNG PkgConfig::ZLIB
  Threads::Threads ${CMAKE_DL_LIBS} ${CMAKE_JS_LIB})
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--exclude-libs,ALL -Wl,--gc-sections -Wl,--as-needed)
