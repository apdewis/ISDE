cmake_minimum_required(VERSION 3.16)
project(ISDE VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

# --- Core dependencies (all components) ---
pkg_check_modules(ISW REQUIRED IMPORTED_TARGET isw)
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

# --- Subdirectories ---
add_subdirectory(libisde)
