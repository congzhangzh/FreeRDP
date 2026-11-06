# FreeRDP: A Remote Desktop Protocol Implementation
# freerdp-shadow-sesman: route each RDP connection to the GNOME session of the
# user it names (see README.md)
#
# Copyright 2026 Cong Zhang
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#	 http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set(MODULE_NAME "freerdp-shadow-sesman")

add_executable(${MODULE_NAME} sesman.c)
# the shadow server it starts for each user, where it is installed
target_compile_definitions(
  ${MODULE_NAME} PRIVATE SESMAN_SHADOW="${CMAKE_INSTALL_FULL_BINDIR}/$<TARGET_FILE_NAME:freerdp-shadow-cli>"
)

install(TARGETS ${MODULE_NAME} DESTINATION ${CMAKE_INSTALL_SBINDIR} COMPONENT server)
configure_file(freerdp-shadow-sesman.service.in ${CMAKE_CURRENT_BINARY_DIR}/freerdp-shadow-sesman.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/freerdp-shadow-sesman.service DESTINATION lib/systemd/system COMPONENT server)
install(FILES freerdp-shadow-sesman.rules DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/polkit-1/rules.d COMPONENT server)
