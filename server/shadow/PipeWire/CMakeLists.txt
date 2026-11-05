# The sound of a Linux session, for any subsystem: what its default sink plays goes to
# the clients, what a client's microphone hears comes out of a virtual source.
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

add_library(freerdp-shadow-pipewire STATIC shadow_pipewire_audio.h shadow_pipewire_audio.c)
set_target_properties(freerdp-shadow-pipewire PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(freerdp-shadow-pipewire PUBLIC PkgConfig::PIPEWIRE)
target_include_directories(freerdp-shadow-pipewire INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
