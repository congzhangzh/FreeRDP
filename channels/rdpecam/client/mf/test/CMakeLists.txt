# FreeRDP: A Remote Desktop Protocol Implementation
# MS-RDPECAM Media Foundation camera HAL tests
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

# The HAL is compiled in with its test hooks (CAM_MF_TESTING) and driven as camera_device_main.c
# does; no camera is needed. A cross build runs them with CMAKE_CROSSCOMPILING_EMULATOR (wine).
set(MODULE_NAME "TestRdpecamMF")
set(TEST_RDPECAM_MF_TESTS TestCameraMFNoMediaFoundation.c TestCameraMFBadFile.c TestCameraMFCaptureError.c)
create_test_sourcelist(TEST_RDPECAM_MF_SRCS TestRdpecamMF.c ${TEST_RDPECAM_MF_TESTS})
add_executable(${MODULE_NAME} ${TEST_RDPECAM_MF_SRCS} camera_mf_test.c camera_mf_test.h ../camera_mf.c)
target_compile_definitions(${MODULE_NAME} PRIVATE CAM_MF_TESTING)
target_include_directories(${MODULE_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(${MODULE_NAME} PRIVATE winpr ole32)
set_target_properties(${MODULE_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TESTING_OUTPUT_DIRECTORY}")
foreach(test ${TEST_RDPECAM_MF_TESTS})
  get_filename_component(TestName ${test} NAME_WE)
  add_test(NAME ${TestName} COMMAND ${MODULE_NAME} ${TestName})
  # 77: Media Foundation can not play the generated test video here
  set_tests_properties(${TestName} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endforeach()
set_property(TARGET ${MODULE_NAME} PROPERTY FOLDER "FreeRDP/Channels/rdpecam/Test")
