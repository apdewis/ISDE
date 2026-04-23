cmake_minimum_required(VERSION 3.16)
project(ISDE VERSION 0.1.0 LANGUAGES C)

# Default CMAKE_INSTALL_PREFIX to /usr unless the user overrides it.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

# FreeBSD (and other systems) install to /usr/local; ensure pkg-config finds
# .pc files there.  This is a no-op when the path doesn't exist.
if(EXISTS "/usr/local/libdata/pkgconfig")
    set(ENV{PKG_CONFIG_PATH}
        "$ENV{PKG_CONFIG_PATH}:/usr/local/libdata/pkgconfig:/usr/local/lib/pkgconfig")
endif()

find_package(PkgConfig REQUIRED)

# --- Core dependencies (all components) ---
pkg_check_modules(ISW REQUIRED IMPORTED_TARGET isw>=0.4.0)

# If libISW is not installed, allow overriding its include/lib paths.
# Usage: cmake -B build -DISW_LOCAL_DIR=/home/user/libISW
if(ISW_LOCAL_DIR)
    set_property(TARGET PkgConfig::ISW PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${ISW_LOCAL_DIR}/include")
    set_property(TARGET PkgConfig::ISW PROPERTY
        INTERFACE_LINK_LIBRARIES "${ISW_LOCAL_DIR}/build/libISW.so")
endif()
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
pkg_check_modules(XCB_EWMH REQUIRED IMPORTED_TARGET xcb-ewmh)
pkg_check_modules(XCB_ICCCM REQUIRED IMPORTED_TARGET xcb-icccm)
pkg_check_modules(XCB_KEYSYMS REQUIRED IMPORTED_TARGET xcb-keysyms)
pkg_check_modules(XCB_XFIXES REQUIRED IMPORTED_TARGET xcb-xfixes)
pkg_check_modules(XCB_SHAPE REQUIRED IMPORTED_TARGET xcb-shape)
pkg_check_modules(XCB_UTIL REQUIRED IMPORTED_TARGET xcb-util)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo>=1.12.0)
pkg_check_modules(CAIRO_XCB REQUIRED IMPORTED_TARGET cairo-xcb>=1.12.0)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(SM REQUIRED IMPORTED_TARGET sm)
pkg_check_modules(ICE REQUIRED IMPORTED_TARGET ice)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
pkg_check_modules(XCB_XKB REQUIRED IMPORTED_TARGET xcb-xkb)
pkg_check_modules(XCB_RANDR REQUIRED IMPORTED_TARGET xcb-randr)
pkg_check_modules(XCB_CURSOR REQUIRED IMPORTED_TARGET xcb-cursor)
pkg_check_modules(LIBSEAT REQUIRED IMPORTED_TARGET libseat)

# PAM has no pkg-config file on most distros — find it manually
find_library(PAM_LIBRARY pam REQUIRED)
find_path(PAM_INCLUDE_DIR security/pam_appl.h REQUIRED)
add_library(pam_imported INTERFACE IMPORTED)
set_target_properties(pam_imported PROPERTIES
    INTERFACE_LINK_LIBRARIES "${PAM_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PAM_INCLUDE_DIR}")

# --- Subdirectories ---
add_subdirectory(libisde)
add_subdirectory(wm)
add_subdirectory(session)
add_subdirectory(panel)
add_subdirectory(fm)
add_subdirectory(settings)
add_subdirectory(dm)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
endif()
add_subdirectory(mountd)
add_subdirectory(tray-mount)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
add_subdirectory(tray-audio)
add_subdirectory(term)
add_subdirectory(common)
