# FreeRDP: A Remote Desktop Protocol Implementation
# Shadow server virtual camera: unit tests
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

set(MODULE_NAME "TestShadowCamera")
set(MODULE_PREFIX "TEST_SHADOW_CAMERA")

disable_warnings_for_directory(${CMAKE_CURRENT_BINARY_DIR})

set(${MODULE_PREFIX}_DRIVER ${MODULE_NAME}.c)

set(${MODULE_PREFIX}_TESTS TestShadowCameraFormat.c TestShadowCameraOwner.c TestShadowCameraDemand.c TestShadowCameraPlaceholder.c
                           TestShadowCameraImage.c TestShadowCameraFileSink.c TestShadowCameraStats.c
)
# the parts that need no channel and no client: their files are built in
set(${MODULE_PREFIX}_PARTS ../camera_image.c ../camera_policy.c ../camera_decode.c ../placeholder.c ../sink_file.c ../camera_stats.c)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND ${MODULE_PREFIX}_TESTS TestShadowCameraV4l2.c)
  list(APPEND ${MODULE_PREFIX}_PARTS ../sink_v4l2.c)
endif()

create_test_sourcelist(${MODULE_PREFIX}_SRCS ${${MODULE_PREFIX}_DRIVER} ${${MODULE_PREFIX}_TESTS})

add_executable(${MODULE_NAME} ${${MODULE_PREFIX}_SRCS} ${${MODULE_PREFIX}_PARTS})
target_include_directories(${MODULE_NAME} PRIVATE ..)
target_link_libraries(${MODULE_NAME} PRIVATE freerdp winpr)

set_target_properties(${MODULE_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TESTING_OUTPUT_DIRECTORY}")

foreach(test ${${MODULE_PREFIX}_TESTS})
  get_filename_component(TestName ${test} NAME_WE)
  add_test(NAME ${TestName} COMMAND ${MODULE_NAME} ${TestName})
endforeach()

set_property(TARGET ${MODULE_NAME} PROPERTY FOLDER "Server/shadow/Test")
