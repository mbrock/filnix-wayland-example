cmake_minimum_required(VERSION 3.20)
project(filnix-wayland-example CXX C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
  dbus-1 xkbcommon wayland-client)
pkg_get_variable(WL_PROTOCOLS wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(XDG_XML ${WL_PROTOCOLS}/stable/xdg-shell/xdg-shell.xml)
add_custom_command(
  OUTPUT xdg-shell.h xdg-shell.c
  COMMAND ${WAYLAND_SCANNER} client-header ${XDG_XML} xdg-shell.h
  COMMAND ${WAYLAND_SCANNER} private-code ${XDG_XML} xdg-shell.c
  DEPENDS ${XDG_XML})

add_executable(hello-wayland main.cpp ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell.c)
target_include_directories(hello-wayland PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(hello-wayland PRIVATE PkgConfig::DEPS)
install(TARGETS hello-wayland)
