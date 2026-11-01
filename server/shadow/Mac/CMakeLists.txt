# The screen is captured with ScreenCaptureKit (macOS 12.3), the sound it plays
# too (macOS 13); input is posted with Quartz events; AppKit gives the cursor
# the clients draw; IOKit keeps the display awake while clients are connected.
# (WITH_SHADOW is off by default for older targets; this is for an explicit ON)
if(CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "12.3")
  message(FATAL_ERROR "the mac shadow server subsystem needs macOS 12.3 or later (ScreenCaptureKit)")
endif()

find_library(SCREENCAPTUREKIT ScreenCaptureKit REQUIRED)
find_library(COREMEDIA CoreMedia REQUIRED)
find_library(COREVIDEO CoreVideo REQUIRED)
find_library(FOUNDATION Foundation REQUIRED)
find_library(APPLICATIONSERVICES ApplicationServices REQUIRED)
find_library(IOKIT IOKit REQUIRED)
find_library(APPKIT AppKit REQUIRED)
find_package(PAM)

set(LIBS ${SCREENCAPTUREKIT} ${COREMEDIA} ${COREVIDEO} ${FOUNDATION} ${APPLICATIONSERVICES} ${IOKIT} ${APPKIT})

if(PAM_FOUND)
  add_compile_definitions(WITH_PAM)
  include_directories(SYSTEM ${PAM_INCLUDE_DIR})
  list(APPEND LIBS ${PAM_LIBRARY})
else()
  message("building without PAM authentication support")
endif()

add_compile_definitions(WITH_SHADOW_MAC)
find_library(IOSURFACE IOSurface REQUIRED)
list(APPEND LIBS ${IOSURFACE})

set(MAC_COMMON mac_capture.h mac_capture.m mac_cursor.h mac_cursor.m mac_input.h mac_input.c mac_ipc.h)
add_library(
  freerdp-shadow-subsystem-impl STATIC mac_shadow.h mac_shadow.c mac_remote.h mac_remote.c ${MAC_COMMON}
)
set_source_files_properties(mac_capture.m mac_cursor.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
# without ARC: the agent releases its XPC objects itself (xpc_release)
set_source_files_properties(mac_agent.m PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
target_link_libraries(freerdp-shadow-subsystem-impl PRIVATE ${LIBS})

# The agent: the server's capture and input in a session when the server runs as a root
# daemon (launchd starts the agent in the session on the console; see mac_ipc.h)
add_executable(freerdp-shadow-agent mac_agent.m ${MAC_COMMON})
target_link_libraries(freerdp-shadow-agent PRIVATE winpr freerdp ${LIBS})
install(TARGETS freerdp-shadow-agent DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT server)
