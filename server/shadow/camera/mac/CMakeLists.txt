# FreeRDP: A Remote Desktop Protocol Implementation
# macOS Shadow Server camera: a CoreMediaIO Camera Extension, the sink helper the shadow side
# pushes the client's frames with, VideoToolbox decoding, and their tests
#
# Copyright 2026 Cong Zhang <congzhangzh@gmail.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Two ways to build it:
#  - inside FreeRDP: server/shadow/CMakeLists.txt adds this directory on macOS with
#    -DWITH_SHADOW_CAMERA_MAC=ON; the shadow subsystem and its agent link
#    freerdp-shadow-camera-mac, freerdp-shadow-cli takes --camera-extension;
#  - on its own (the release kit does this until then, and the parts without Apple frameworks
#    build anywhere):
#      cmake -G Ninja -S server/shadow/camera/mac -B build-camera -DBUILD_TESTING=ON \
#            -DCAMERA_MAC_BUNDLE_ID=<app id> -DCAMERA_MAC_TEAM_ID=<team id>
#      cmake --build build-camera && ctest --test-dir build-camera --output-on-failure
#
# Products (macOS):
#  - <app id>.camera.systemextension: the extension bundle, unsigned (the packager signs it,
#    inside out, with its entitlements: see the release kit's build/macos/sign-host.sh);
#  - freerdp-shadow-camera: activate / deactivate / status / pattern (camera_activate.h);
#  - libfreerdp-shadow-camera-mac.a: the sink helper, the decoder, the activation.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(freerdp-shadow-camera-mac LANGUAGES C)
  include(CTest)
endif()

# the ids: ONE variable, the host app's bundle id; everything else derives from it
set(CAMERA_MAC_BUNDLE_ID "io.github.congzhangzh.freerdp-shadow"
    CACHE STRING "the bundle id of the app that carries the camera extension"
)
# a placeholder for unsigned (ad hoc) builds, which macOS never activates; a real build gets
# the Developer ID team (the kit's MACOS_TEAM_ID)
set(CAMERA_MAC_TEAM_ID "UNSIGNED00" CACHE STRING "the Apple Developer team id that signs the app")
set(CAMERA_MAC_VERSION "0" CACHE STRING "the extension's CFBundleVersion")
set(CAMERA_MAC_ID "${CAMERA_MAC_BUNDLE_ID}.camera")
set(CAMERA_MAC_APP_GROUP "${CAMERA_MAC_TEAM_ID}.${CAMERA_MAC_BUNDLE_ID}")
set(CAMERA_MAC_MACH_SERVICE "${CAMERA_MAC_APP_GROUP}.camera")
if(NOT CAMERA_MAC_TEAM_ID MATCHES "^[A-Z0-9]+$")
  message(FATAL_ERROR "CAMERA_MAC_TEAM_ID: ${CAMERA_MAC_TEAM_ID} is not a team id")
endif()

# the placeholder is shared with the other platforms' cameras (one picture everywhere)
set(CAMERA_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

# --- plain C: no Apple framework, so it builds and is tested anywhere ---------------------------
add_library(
  freerdp-shadow-camera-frame STATIC
  ${CAMERA_SHARED_DIR}/placeholder.h
  ${CAMERA_SHARED_DIR}/placeholder.c
  camera_config.h
  camera_frame.h
  camera_frame.c
  camera_h264.h
  camera_h264.c
  camera_readers.h
  camera_readers.c
)
# the build tree's headers only: FreeRDP's static install exports these libraries
# (FreeRDP-ShadowTargets) and installs no camera header
target_include_directories(
  freerdp-shadow-camera-frame PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                                     $<BUILD_INTERFACE:${CAMERA_SHARED_DIR}>
)
set_target_properties(freerdp-shadow-camera-frame PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)

if(BUILD_TESTING)
  add_executable(TestCameraFrame test/TestCameraFrame.c)
  target_link_libraries(TestCameraFrame PRIVATE freerdp-shadow-camera-frame)
  add_test(NAME TestCameraFrame COMMAND TestCameraFrame)
endif()

if(NOT APPLE OR IOS)
  return()
endif()

# --- macOS ------------------------------------------------------------------------------------
# Standalone, the .m files are Objective-C sources. In the FreeRDP tree they are compiled as the
# shadow's own .m files are (server/shadow/Mac): by the C compiler, clang taking Objective-C from
# the extension. Enabling OBJC in a subdirectory of a project without it breaks the generate step
# (CMAKE_OBJC_COMPILE_OBJECT not set, kit run 46033570427).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_language(OBJC)
endif()
# CMIOExtension: macOS 12.3; the kit's host is macOS 13 (ScreenCaptureKit's sound)
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0")
endif()
if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "12.3")
  message(FATAL_ERROR "the camera extension needs macOS 12.3 or later (CMIOExtension)")
endif()

find_library(CAM_FOUNDATION Foundation REQUIRED)
find_library(CAM_COREMEDIA CoreMedia REQUIRED)
find_library(CAM_COREMEDIAIO CoreMediaIO REQUIRED)
find_library(CAM_COREVIDEO CoreVideo REQUIRED)
find_library(CAM_IOSURFACE IOSurface REQUIRED)
find_library(CAM_VIDEOTOOLBOX VideoToolbox REQUIRED)
find_library(CAM_SYSTEMEXTENSIONS SystemExtensions REQUIRED)
find_library(CAM_SECURITY Security REQUIRED)

set(CAMERA_OBJC_OPTIONS -fobjc-arc -Wall)

# the shadow side: the sink helper, the decoder, the activation
add_library(
  freerdp-shadow-camera-mac STATIC
  camera_sink.h
  camera_sink.m
  camera_decode.h
  camera_decode.m
  camera_activate.h
  camera_activate.m
)
target_compile_options(freerdp-shadow-camera-mac PRIVATE ${CAMERA_OBJC_OPTIONS})
set_target_properties(freerdp-shadow-camera-mac PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(
  freerdp-shadow-camera-mac
  PUBLIC freerdp-shadow-camera-frame
         ${CAM_FOUNDATION}
         ${CAM_COREMEDIA}
         ${CAM_COREMEDIAIO}
         ${CAM_COREVIDEO}
         ${CAM_IOSURFACE}
         ${CAM_VIDEOTOOLBOX}
         ${CAM_SYSTEMEXTENSIONS}
)

# the camera commands on their own (until freerdp-shadow-cli takes --camera-extension)
add_executable(freerdp-shadow-camera tools/camera_tool.m)
target_compile_options(freerdp-shadow-camera PRIVATE ${CAMERA_OBJC_OPTIONS})
target_link_libraries(freerdp-shadow-camera PRIVATE freerdp-shadow-camera-mac)

# the extension: <app id>.camera.systemextension/Contents/{Info.plist,MacOS/<app id>.camera}
configure_file(extension/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/camera-Info.plist @ONLY)
add_executable(freerdp-shadow-camera-extension MACOSX_BUNDLE extension/CameraProvider.h extension/CameraProvider.m extension/main.m)
set_target_properties(
  freerdp-shadow-camera-extension
  PROPERTIES OUTPUT_NAME "${CAMERA_MAC_ID}"
             BUNDLE_EXTENSION "systemextension"
             MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/camera-Info.plist"
)
target_compile_options(freerdp-shadow-camera-extension PRIVATE ${CAMERA_OBJC_OPTIONS})
target_link_libraries(
  freerdp-shadow-camera-extension
  PRIVATE freerdp-shadow-camera-frame
          ${CAM_FOUNDATION}
          ${CAM_COREMEDIA}
          ${CAM_COREMEDIAIO}
          ${CAM_COREVIDEO}
          ${CAM_IOSURFACE}
          ${CAM_SECURITY}
)

# what the packager needs to know, in one file it can read (the kit checks the built bundle
# against it)
file(
  WRITE ${CMAKE_CURRENT_BINARY_DIR}/camera-ids.txt
  "bundle_id=${CAMERA_MAC_BUNDLE_ID}\ncamera_id=${CAMERA_MAC_ID}\nteam_id=${CAMERA_MAC_TEAM_ID}\n"
  "app_group=${CAMERA_MAC_APP_GROUP}\nmach_service=${CAMERA_MAC_MACH_SERVICE}\n"
  "device_uid=AB9AC9A2-6D08-4DD3-BC75-25A3E66BE765\n"
)

if(BUILD_TESTING)
  add_executable(TestCameraSink test/TestCameraSink.m)
  target_compile_options(TestCameraSink PRIVATE ${CAMERA_OBJC_OPTIONS})
  target_link_libraries(TestCameraSink PRIVATE freerdp-shadow-camera-mac)
  add_test(NAME TestCameraSink COMMAND TestCameraSink)

  find_library(CAM_COREGRAPHICS CoreGraphics REQUIRED)
  find_library(CAM_IMAGEIO ImageIO REQUIRED)
  add_executable(TestCameraDecode test/TestCameraDecode.m)
  target_compile_options(TestCameraDecode PRIVATE ${CAMERA_OBJC_OPTIONS})
  target_link_libraries(TestCameraDecode PRIVATE freerdp-shadow-camera-mac ${CAM_COREGRAPHICS} ${CAM_IMAGEIO})
  add_test(NAME TestCameraDecode COMMAND TestCameraDecode)

  # the extension's readers count (on demand), with its own objects in this process
  add_executable(
    TestCameraExtension test/TestCameraExtension.m extension/CameraProvider.h extension/CameraProvider.m
  )
  target_include_directories(TestCameraExtension PRIVATE extension)
  target_compile_options(TestCameraExtension PRIVATE ${CAMERA_OBJC_OPTIONS})
  target_link_libraries(
    TestCameraExtension
    PRIVATE freerdp-shadow-camera-frame
            ${CAM_FOUNDATION}
            ${CAM_COREMEDIA}
            ${CAM_COREMEDIAIO}
            ${CAM_COREVIDEO}
            ${CAM_IOSURFACE}
            ${CAM_SECURITY}
  )
  add_test(NAME TestCameraExtension COMMAND TestCameraExtension)
  set_tests_properties(TestCameraExtension PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # inside FreeRDP: installed with the server, the packager moves the bundle into the app
  install(TARGETS freerdp-shadow-camera DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT server)
  install(TARGETS freerdp-shadow-camera-extension BUNDLE DESTINATION ${CMAKE_INSTALL_LIBDIR}/freerdp-shadow COMPONENT server)
endif()
