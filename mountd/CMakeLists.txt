# --- isde-mountd (privileged disk mount daemon) ---

add_executable(isde-mountd
    src/main.c
    src/mountd.c
    src/dbus.c
    src/devices.c
    src/mount_ops.c
    src/luks.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config.c
    ${CMAKE_SOURCE_DIR}/platform/common/vendor/tomlc99/toml.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-xdg.c
    ${CMAKE_SOURCE_DIR}/platform/common/dbus.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-theme.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/theme-x11.c
)

target_include_directories(isde-mountd PRIVATE
    ${CMAKE_SOURCE_DIR}/platform/common
    ${CMAKE_SOURCE_DIR}/platform/X11/common
)

# Platform-specific source selection.  On FreeBSD the source files rely on
# BSD-visible APIs (nmount, getmntinfo, GEOM, CAM, devd) and must NOT define
# _POSIX_C_SOURCE, which would hide them; the defines below are Linux-only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(isde-mountd PRIVATE
        _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
    target_sources(isde-mountd PRIVATE src/platform_linux.c)
    target_link_libraries(isde-mountd
        ISDE::UDEV ISDE::CRYPTSETUP ISDE::BLKID)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    target_sources(isde-mountd PRIVATE src/platform_freebsd.c)
    target_link_libraries(isde-mountd geom cam)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

target_link_libraries(isde-mountd
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_ICCCM
    ISDE::CAIRO
    ISDE::DBUS
)

install(TARGETS isde-mountd RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES data/org.isde.DiskManager.conf
        DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/system.d")

# --- Service unit files (opt-in install components) ---

install(FILES data/isde-mountd.service
        DESTINATION "${ISDE_SYSTEMD_UNIT_DIR}"
        COMPONENT systemd
        EXCLUDE_FROM_ALL)

install(PROGRAMS data/runit/run
        DESTINATION "${ISDE_RUNIT_SERVICE_DIR}/isde-mountd"
        COMPONENT runit
        EXCLUDE_FROM_ALL)

configure_file(data/rc.d/isde-mountd.in
        "${CMAKE_CURRENT_BINARY_DIR}/rc.d/isde-mountd" @ONLY)
install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/rc.d/isde-mountd"
        DESTINATION "${ISDE_RC_D_DIR}"
        COMPONENT rc_d
        EXCLUDE_FROM_ALL)
