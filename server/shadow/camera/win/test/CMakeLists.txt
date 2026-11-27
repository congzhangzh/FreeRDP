# FreeRDP: A Remote Desktop Protocol Implementation
# Shadow server virtual camera: the Windows virtual camera, tests
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

set(MODULE_NAME "TestShadowCameraWin")
set(MODULE_PREFIX "TEST_SHADOW_CAMERA_WIN")

disable_warnings_for_directory(${CMAKE_CURRENT_BINARY_DIR})

set(${MODULE_PREFIX}_DRIVER ${MODULE_NAME}.c)
set(${MODULE_PREFIX}_TESTS TestShadowCameraWinSource.c TestShadowCameraWinReaders.c)

create_test_sourcelist(${MODULE_PREFIX}_SRCS ${${MODULE_PREFIX}_DRIVER} ${${MODULE_PREFIX}_TESTS})

add_executable(${MODULE_NAME} ${${MODULE_PREFIX}_SRCS} ../camera_ring.c)
target_link_libraries(${MODULE_NAME} PRIVATE mfplat mfreadwrite ole32 uuid advapi32)
add_dependencies(${MODULE_NAME} freerdp-shadow-camera-source)

set_target_properties(${MODULE_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TESTING_OUTPUT_DIRECTORY}")

add_test(NAME TestShadowCameraWinSource COMMAND ${MODULE_NAME} TestShadowCameraWinSource
                                                $<TARGET_FILE:freerdp-shadow-camera-source>
)

add_test(NAME TestShadowCameraWinReaders COMMAND ${MODULE_NAME} TestShadowCameraWinReaders)

set_property(TARGET ${MODULE_NAME} PROPERTY FOLDER "Server/shadow/Test")
