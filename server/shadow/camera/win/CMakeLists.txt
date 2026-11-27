# FreeRDP: A Remote Desktop Protocol Implementation
# Shadow server virtual camera: the Windows virtual camera
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

# freerdp-shadow-camera-source.dll: the media source Frame Server loads for the "FreeRDP Camera"
# virtual camera (C++: Media Foundation's COM interfaces). It links no FreeRDP library, only
# Windows and its own C runtime statically (Frame Server's svchost loads it). The shadow
# server installs it from beside freerdp-shadow-cli.exe (sink_win.c).
project(freerdp-shadow-camera-source LANGUAGES C CXX)

add_library(
  freerdp-shadow-camera-source SHARED source/camera_source.cpp source/camera_source.def camera_ring.c camera_ring.h
                                      camera_mf_ids.h
)
set_target_properties(
  freerdp-shadow-camera-source PROPERTIES PREFIX "" OUTPUT_NAME "freerdp-shadow-camera-source" CXX_STANDARD 17
                                          CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
)
if(MSVC)
  set_target_properties(freerdp-shadow-camera-source PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(freerdp-shadow-camera-source PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
  target_link_options(freerdp-shadow-camera-source PRIVATE -static)
endif()
target_compile_definitions(freerdp-shadow-camera-source PRIVATE WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(freerdp-shadow-camera-source PRIVATE mfplat mfuuid ole32 uuid advapi32)
install(TARGETS freerdp-shadow-camera-source RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
