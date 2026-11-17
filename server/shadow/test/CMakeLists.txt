set(MODULE_NAME "TestShadow")
set(MODULE_PREFIX "TEST_SHADOW")

disable_warnings_for_directory(${CMAKE_CURRENT_BINARY_DIR})

set(${MODULE_PREFIX}_DRIVER ${MODULE_NAME}.c)

set(${MODULE_PREFIX}_TESTS TestShadowCaptureCompare.c TestShadowH264Settings.c TestShadowCapsOrder.c)

create_test_sourcelist(${MODULE_PREFIX}_SRCS ${${MODULE_PREFIX}_DRIVER} ${${MODULE_PREFIX}_TESTS})

# shadow_caps_order.c is internal to the library: the test builds its own copy
add_executable(${MODULE_NAME} ${${MODULE_PREFIX}_SRCS} ../shadow_caps_order.c)

target_link_libraries(${MODULE_NAME} PRIVATE freerdp-shadow freerdp winpr)

set_target_properties(${MODULE_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TESTING_OUTPUT_DIRECTORY}")

foreach(test ${${MODULE_PREFIX}_TESTS})
  get_filename_component(TestName ${test} NAME_WE)
  add_test(NAME ${TestName} COMMAND ${MODULE_NAME} ${TestName})
endforeach()

set_property(TARGET ${MODULE_NAME} PROPERTY FOLDER "Server/shadow/Test")
