cmake_minimum_required(VERSION 3.16)
project(ISDE VERSION 0.15.0 LANGUAGES C)

# Default CMAKE_INSTALL_PREFIX to /usr unless the user overrides it.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_SKIP_INSTALL_RPATH ON)

include(GNUInstallDirs)

# Service unit install destinations (used by dm/ and mountd/ for the optional
# `install-systemd`, `install-runit` and `install-rc_d` components).
set(ISDE_SYSTEMD_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/system"
    CACHE PATH "systemd system unit directory")
set(ISDE_RUNIT_SERVICE_DIR "/etc/sv"
    CACHE PATH "runit service directory (sv tree)")
set(ISDE_RC_D_DIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/rc.d"
    CACHE PATH "FreeBSD rc.d service directory")

# Dependencies are resolved into ISDE::<NAME> imported targets.  On Linux this
# is done via pkg-config; on FreeBSD (which we build without pkg-config) the
# libraries and headers are located directly under /usr/local.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # pkg-config is only consulted on Linux.  The /usr/local search path below
    # is a no-op when the directory doesn't exist.
    if(EXISTS "/usr/local/libdata/pkgconfig")
        set(ENV{PKG_CONFIG_PATH}
            "$ENV{PKG_CONFIG_PATH}:/usr/local/libdata/pkgconfig:/usr/local/lib/pkgconfig")
    endif()
    find_package(PkgConfig REQUIRED)
endif()

# Resolve an external dependency into an ISDE::<NAME> INTERFACE target.
#   PC           pkg-config module spec used on Linux (may carry a version).
#   LIBS         library names located with find_library on FreeBSD.
#   HEADERS      headers located with find_path on FreeBSD (each adds its dir).
#   HEADER_PATHS extra search roots for HEADERS (for non /usr/local/include dirs).
function(isde_dependency NAME)
    cmake_parse_arguments(DEP "" "PC" "LIBS;HEADERS;HEADER_PATHS" ${ARGN})
    if(TARGET ISDE::${NAME})
        return()
    endif()
    add_library(ISDE::${NAME} INTERFACE IMPORTED GLOBAL)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        pkg_check_modules(${NAME} REQUIRED IMPORTED_TARGET ${DEP_PC})
        set_property(TARGET ISDE::${NAME} PROPERTY
            INTERFACE_LINK_LIBRARIES PkgConfig::${NAME})
    else()
        # FreeBSD: ports install under /usr/local.  Link by name through
        # /usr/local/lib and expose /usr/local/include plus any package-specific
        # include dirs (HEADER_PATHS); the compiler and linker resolve the rest.
        # This avoids per-dependency find_library/find_path name guessing, which
        # breaks the moment any single library's .so name or header layout
        # differs from the guess.
        set(_incs /usr/local/include ${DEP_HEADER_PATHS})
        list(REMOVE_DUPLICATES _incs)
        set_property(TARGET ISDE::${NAME} PROPERTY
            INTERFACE_LINK_LIBRARIES "${DEP_LIBS}")
        set_property(TARGET ISDE::${NAME} PROPERTY
            INTERFACE_LINK_DIRECTORIES /usr/local/lib)
        set_property(TARGET ISDE::${NAME} PROPERTY
            INTERFACE_INCLUDE_DIRECTORIES "${_incs}")
    endif()
endfunction()

# --- Core dependencies (all components) ---
# If libISW is not installed, allow overriding its include/lib paths instead of
# locating an installed copy.  Usage: cmake -B build -DISW_LOCAL_DIR=/home/user/libISW
if(ISW_LOCAL_DIR)
    add_library(ISDE::ISW_so SHARED IMPORTED GLOBAL)
    set_target_properties(ISDE::ISW_so PROPERTIES
        IMPORTED_LOCATION "${ISW_LOCAL_DIR}/build/libISW.so")
    add_library(ISDE::ISW INTERFACE IMPORTED GLOBAL)
    set_property(TARGET ISDE::ISW PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${ISW_LOCAL_DIR}/include")
    set_property(TARGET ISDE::ISW PROPERTY
        INTERFACE_LINK_LIBRARIES ISDE::ISW_so)
else()
    isde_dependency(ISW         PC isw>=0.11.0          LIBS ISW
        HEADERS ISW/Intrinsic.h  HEADER_PATHS /usr/local/include/isw)
endif()
isde_dependency(XCB             PC xcb                 LIBS xcb             HEADERS xcb/xcb.h)
isde_dependency(XCB_EWMH        PC xcb-ewmh            LIBS xcb-ewmh        HEADERS xcb/xcb_ewmh.h)
isde_dependency(XCB_ICCCM       PC xcb-icccm           LIBS xcb-icccm       HEADERS xcb/xcb_icccm.h)
isde_dependency(XCB_KEYSYMS     PC xcb-keysyms         LIBS xcb-keysyms     HEADERS xcb/xcb_keysyms.h)
isde_dependency(XCB_XRM         PC xcb-xrm             LIBS xcb-xrm         HEADERS xcb/xcb_xrm.h)
isde_dependency(XCB_XFIXES      PC xcb-xfixes          LIBS xcb-xfixes      HEADERS xcb/xfixes.h)
isde_dependency(XCB_SHAPE       PC xcb-shape           LIBS xcb-shape       HEADERS xcb/shape.h)
isde_dependency(XCB_UTIL        PC xcb-util            LIBS xcb-util        HEADERS xcb/xcb_aux.h)
isde_dependency(CAIRO           PC cairo>=1.12.0       LIBS cairo
    HEADERS cairo.h cairo/cairo.h  HEADER_PATHS /usr/local/include/cairo)
isde_dependency(CAIRO_XCB       PC cairo-xcb>=1.12.0   LIBS cairo           HEADERS cairo/cairo-xcb.h)
isde_dependency(CAIRO_FT        PC cairo-ft>=1.12.0    LIBS cairo           HEADERS cairo/cairo-ft.h)
isde_dependency(FONTCONFIG      PC fontconfig          LIBS fontconfig      HEADERS fontconfig/fontconfig.h)
isde_dependency(FREETYPE        PC freetype2           LIBS freetype
    HEADERS ft2build.h  HEADER_PATHS /usr/local/include/freetype2)
isde_dependency(DBUS            PC dbus-1              LIBS dbus-1
    HEADERS dbus/dbus.h dbus/dbus-arch-deps.h
    HEADER_PATHS /usr/local/include/dbus-1.0 /usr/local/lib/dbus-1.0/include)
isde_dependency(LIBJPEG         PC libjpeg             LIBS jpeg            HEADERS jpeglib.h)
isde_dependency(XCB_XKB         PC xcb-xkb             LIBS xcb-xkb         HEADERS xcb/xkb.h)
isde_dependency(XKBCOMMON       PC xkbcommon           LIBS xkbcommon       HEADERS xkbcommon/xkbcommon.h)
isde_dependency(XCB_RANDR       PC xcb-randr           LIBS xcb-randr       HEADERS xcb/randr.h)
isde_dependency(XCB_CURSOR      PC xcb-cursor          LIBS xcb-cursor      HEADERS xcb/xcb_cursor.h)
isde_dependency(XCB_DPMS        PC xcb-dpms            LIBS xcb-dpms        HEADERS xcb/dpms.h)
isde_dependency(XCB_SCREENSAVER PC xcb-screensaver     LIBS xcb-screensaver HEADERS xcb/screensaver.h)
isde_dependency(LIBSEAT         PC libseat             LIBS seat            HEADERS libseat.h)
isde_dependency(TSM             PC libtsm              LIBS tsm             HEADERS libtsm.h)

# --- Optional compositor WM build ---
option(ISDE_BUILD_COMPOSITOR "Build isde-wm-compositor (requires EGL + OpenGL)" OFF)
if(ISDE_BUILD_COMPOSITOR)
    isde_dependency(EGL           PC egl             LIBS EGL           HEADERS EGL/egl.h)
    isde_dependency(GL            PC gl              LIBS GL            HEADERS GL/gl.h)
    isde_dependency(XCB_PRESENT   PC xcb-present     LIBS xcb-present   HEADERS xcb/present.h)
    isde_dependency(XCB_COMPOSITE PC xcb-composite   LIBS xcb-composite HEADERS xcb/composite.h)
    isde_dependency(XCB_DAMAGE    PC xcb-damage      LIBS xcb-damage    HEADERS xcb/damage.h)
    isde_dependency(XCB_RENDER    PC xcb-render      LIBS xcb-render    HEADERS xcb/render.h)
endif()

# PAM has no pkg-config file on most distros — find it manually
find_library(PAM_LIBRARY pam REQUIRED)
find_path(PAM_INCLUDE_DIR security/pam_appl.h REQUIRED)
add_library(pam_imported INTERFACE IMPORTED)
set_target_properties(pam_imported PROPERTIES
    INTERFACE_LINK_LIBRARIES "${PAM_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PAM_INCLUDE_DIR}")

# --- Subdirectories ---
#add_subdirectory(libisde)
add_subdirectory(platform/X11/wm)
add_subdirectory(platform/X11/session)
add_subdirectory(panel)
add_subdirectory(fm)
add_subdirectory(settings)
add_subdirectory(displayd)
add_subdirectory(dm)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    isde_dependency(UDEV       PC libudev       LIBS udev       HEADERS libudev.h)
    isde_dependency(CRYPTSETUP PC libcryptsetup LIBS cryptsetup HEADERS libcryptsetup.h)
    isde_dependency(BLKID      PC blkid         LIBS blkid      HEADERS blkid/blkid.h)
endif()
add_subdirectory(mountd)
#add_subdirectory(tray-mount)
isde_dependency(PIPEWIRE PC libpipewire-0.3 LIBS pipewire-0.3
    HEADERS pipewire/pipewire.h spa/utils/defs.h
    HEADER_PATHS /usr/local/include/pipewire-0.3 /usr/local/include/spa-0.2)
#add_subdirectory(tray-audio)
#add_subdirectory(tray-battery)
#add_subdirectory(tray-net)
#add_subdirectory(tray-bt)
add_subdirectory(platform/X11/term)
add_subdirectory(isw-graph)
add_subdirectory(calc)
#add_subdirectory(screenshot)
add_subdirectory(common)
if(ISDE_BUILD_COMPOSITOR)
    add_subdirectory(platform/X11/wm-compositor)
endif()

# --- Optional service-unit install targets ---------------------------------
# These are NOT part of `make install`. Invoke explicitly:
#   cmake --build build --target install-systemd
#   cmake --build build --target install-runit
#   cmake --build build --target install-rc_d      # FreeBSD

add_custom_target(install-systemd
    COMMAND "${CMAKE_COMMAND}"
            -DCMAKE_INSTALL_COMPONENT=systemd
            -P "${CMAKE_BINARY_DIR}/cmake_install.cmake"
    COMMENT "Installing systemd unit files")

add_custom_target(install-runit
    COMMAND "${CMAKE_COMMAND}"
            -DCMAKE_INSTALL_COMPONENT=runit
            -P "${CMAKE_BINARY_DIR}/cmake_install.cmake"
    COMMENT "Installing runit service files")

add_custom_target(install-rc_d
    COMMAND "${CMAKE_COMMAND}"
            -DCMAKE_INSTALL_COMPONENT=rc_d
            -P "${CMAKE_BINARY_DIR}/cmake_install.cmake"
    COMMENT "Installing FreeBSD rc.d service scripts")
