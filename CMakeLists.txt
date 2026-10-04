cmake_minimum_required(VERSION 3.16)
project(macdock VERSION 0.1.0 DESCRIPTION "A macOS-style dock for X11 Linux desktops" LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)  # g_autoptr/g_autofree rely on GNU cleanup attributes
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
  gtk+-3.0
  gio-unix-2.0
  libwnck-3.0
  x11
)

add_executable(macdock
  src/main.c
  src/config.c
  src/apps.c
  src/dock.c
  src/preferences.c
)
target_link_libraries(macdock PRIVATE PkgConfig::DEPS m)
target_compile_definitions(macdock PRIVATE
  WNCK_I_KNOW_THIS_IS_UNSTABLE
  MACDOCK_VERSION="${PROJECT_VERSION}"
)
target_compile_options(macdock PRIVATE -Wall -Wextra -Wno-unused-parameter)

include(GNUInstallDirs)
install(TARGETS macdock RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/macdock.desktop data/macdock-settings.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
