# FreeRDP: A Remote Desktop Protocol Implementation
# FreeRDP cmake build script
#
# Copyright 2024 Oleg Turovski <oleg2104@hotmail.com>
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

if(WITH_V4L)

  define_channel_client_subsystem("rdpecam" "v4l" "")

  set(${MODULE_PREFIX}_SRCS camera_v4l.c)
  set(${MODULE_PREFIX}_LIBS winpr freerdp)

  if(WITH_RDPECAM_UVC_H264)
    find_package(libusb-1.0 REQUIRED)
    freerdp_client_pc_add_requires_private("libusb-1.0")
    include_directories(SYSTEM ${LIBUSB_1_INCLUDE_DIRS})
    list(APPEND ${MODULE_PREFIX}_SRCS uvc_h264.c)
    list(APPEND ${MODULE_PREFIX}_LIBS ${LIBUSB_1_LIBRARIES})
    add_compile_definitions("WITH_RDPECAM_UVC_H264")
  endif()

  include_directories(..)

  add_channel_client_subsystem_library(${MODULE_PREFIX} ${MODULE_NAME} ${CHANNEL_NAME} "" TRUE "")

endif()
