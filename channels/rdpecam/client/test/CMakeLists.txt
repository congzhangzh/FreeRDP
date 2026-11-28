set(MODULE_NAME "TestRdpecamClient")
set(MODULE_PREFIX "TEST_RDPECAM_CLIENT")

set(${MODULE_PREFIX}_TESTS TestRdpecamFreeRDPCamera.c TestCameraWebH264.c)
create_test_sourcelist(${MODULE_PREFIX}_SRCS ${MODULE_NAME}.c ${${MODULE_PREFIX}_TESTS})
# the browser backend's frame code has no browser dependency: tested on every host
add_executable(${MODULE_NAME} ${${MODULE_PREFIX}_SRCS} ../emscripten/camera_web_h264.c)
target_link_libraries(${MODULE_NAME} PRIVATE freerdp winpr)
set_target_properties(${MODULE_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TESTING_OUTPUT_DIRECTORY}")
foreach(test ${${MODULE_PREFIX}_TESTS})
  get_filename_component(TestName ${test} NAME_WE)
  add_test(${TestName} ${TESTING_OUTPUT_DIRECTORY}/${MODULE_NAME} ${TestName})
endforeach()
set_property(TARGET ${MODULE_NAME} PROPERTY FOLDER "FreeRDP/Channels/rdpecam/Test")
