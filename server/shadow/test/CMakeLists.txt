set(MODULE_NAME "TestShadow")
set(MODULE_PREFIX "TEST_SHADOW")

disable_warnings_for_directory(${CMAKE_CURRENT_BINARY_DIR})

set(${MODULE_PREFIX}_DRIVER ${MODULE_NAME}.c)

set(${MODULE_PREFIX}_TESTS
    TestShadowCaptureCompare.c
    TestShadowCodecOptions.c
    TestShadowGfxCodecOrder.c
    TestShadowConfigLayers.c
    TestShadowGfxStats.c
    TestShadowFrameRate.c
    TestShadowGfxStatsPeriod.c
    TestShadowGfxDefault.c
)

create_test_sourcelist(${MODULE_PREFIX}_SRCS ${${MODULE_PREFIX}_DRIVER} ${${MODULE_PREFIX}_TESTS})

# the 5 s line is private to the library: its file is built in
add_executable(${MODULE_NAME} ${${MODULE_PREFIX}_SRCS} ../shadow_gfx_stats.c)

target_link_libraries(${MODULE_NAME} PRIVATE freerdp-shadow freerdp winpr)

set_target_properties(${MODULE_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TESTING_OUTPUT_DIRECTORY}")

foreach(test ${${MODULE_PREFIX}_TESTS})
  get_filename_component(TestName ${test} NAME_WE)
  add_test(NAME ${TestName} COMMAND ${MODULE_NAME} ${TestName})
endforeach()

set_property(TARGET ${MODULE_NAME} PROPERTY FOLDER "Server/shadow/Test")
