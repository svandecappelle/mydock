cmake_minimum_required(VERSION 3.16)
project(macdock VERSION 0.1.0 DESCRIPTION "A macOS-style dock for X11 Linux desktops" LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)  # g_autoptr/g_autofree rely on GNU cleanup attributes
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Portable code, then one backend per platform for window tracking (wm.h),
# installed apps (appinfo.h) and the dock window's needs (platform.h).
set(SOURCES
  src/main.c
  src/config.c
  src/apps.c
  src/dock.c
  src/preferences.c
  src/themes.c
)

find_package(PkgConfig REQUIRED)
if(WIN32)
  pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET gtk+-3.0)
  list(APPEND SOURCES src/wm_win32.c src/appinfo_win32.c src/platform_win32.c src/win32_util.c)
  set(PLATFORM_DEFINITIONS _WIN32_WINNT=0x0A00 WINVER=0x0A00 COBJMACROS)
  set(PLATFORM_LIBS user32 gdi32 shell32 ole32 dwmapi)
  set(EXECUTABLE_TYPE WIN32)  # a GUI program: no console window
else()
  pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
    gtk+-3.0
    gio-unix-2.0
    libwnck-3.0
    x11
    xext
  )
  list(APPEND SOURCES src/wm_wnck.c src/appinfo_desktop.c src/platform_x11.c)
  set(PLATFORM_DEFINITIONS WNCK_I_KNOW_THIS_IS_UNSTABLE)
endif()

add_executable(macdock ${EXECUTABLE_TYPE} ${SOURCES})
target_link_libraries(macdock PRIVATE PkgConfig::DEPS ${PLATFORM_LIBS} m)
target_compile_definitions(macdock PRIVATE
  ${PLATFORM_DEFINITIONS}
  MACDOCK_VERSION="${PROJECT_VERSION}"
)
target_compile_options(macdock PRIVATE -Wall -Wextra -Wno-unused-parameter)

include(GNUInstallDirs)
install(TARGETS macdock RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(NOT WIN32)
  install(FILES data/macdock.desktop data/macdock-settings.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()
