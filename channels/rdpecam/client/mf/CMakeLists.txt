# FreeRDP: A Remote Desktop Protocol Implementation
# FreeRDP cmake build script
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

define_channel_client_subsystem("rdpecam" "mf" "")

set(${MODULE_PREFIX}_SRCS camera_mf.c)

# mfplat.dll, mf.dll and mfreadwrite.dll are loaded at run time and the GUIDs are spelled out in
# camera_mf.c: the client does not need Media Foundation to start (Windows N editions, Windows
# Server without its Media Foundation feature). Only COM comes from an import library.
set(${MODULE_PREFIX}_LIBS winpr freerdp ole32)

include_directories(..)

add_channel_client_subsystem_library(${MODULE_PREFIX} ${MODULE_NAME} ${CHANNEL_NAME} "" TRUE "")

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
