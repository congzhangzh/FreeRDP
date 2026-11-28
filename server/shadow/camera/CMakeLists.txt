# FreeRDP: A Remote Desktop Protocol Implementation
# Shadow server virtual camera: a client's camera (MS-RDPECAM) as a camera of the host
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

# Built into freerdp-shadow (server/shadow/CMakeLists.txt, WITH_SHADOW_CAMERA): the core, the
# file sink (tests), on Linux the v4l2loopback sink, on Windows the virtual camera.
set(SHADOW_CAMERA_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/shadow_camera.c
    ${CMAKE_CURRENT_SOURCE_DIR}/shadow_camera.h
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_decode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_decode.h
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_image.c
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_image.h
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_policy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_policy.h
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_sink.h
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_stats.c
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_stats.h
    ${CMAKE_CURRENT_SOURCE_DIR}/sink_file.c
    ${CMAKE_CURRENT_SOURCE_DIR}/placeholder.c
    ${CMAKE_CURRENT_SOURCE_DIR}/placeholder.h
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND SHADOW_CAMERA_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/sink_v4l2.c)
endif()
set(SHADOW_CAMERA_LIBS "")
if(WIN32)
  # the Windows virtual camera (win/: the sink, the frame ring and the media source DLL) and
  # Windows' own MJPEG decoder; Media Foundation is loaded at run time
  list(
    APPEND
    SHADOW_CAMERA_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_decode_mf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_decode_mf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/win/camera_mf_ids.h
    ${CMAKE_CURRENT_SOURCE_DIR}/win/camera_ring.c
    ${CMAKE_CURRENT_SOURCE_DIR}/win/camera_ring.h
    ${CMAKE_CURRENT_SOURCE_DIR}/win/sink_win.c
    ${CMAKE_CURRENT_SOURCE_DIR}/win/sink_win.h
  )
  set(SHADOW_CAMERA_LIBS ole32 advapi32)
  add_subdirectory(win)
endif()
set(SHADOW_CAMERA_SRCS ${SHADOW_CAMERA_SRCS} PARENT_SCOPE)
set(SHADOW_CAMERA_LIBS ${SHADOW_CAMERA_LIBS} PARENT_SCOPE)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
