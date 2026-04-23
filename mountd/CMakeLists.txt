# --- isde-mountd (privileged disk mount daemon) ---

add_executable(isde-mountd
    src/main.c
    src/mountd.c
    src/dbus.c
    src/devices.c
    src/mount_ops.c
)

target_compile_definitions(isde-mountd PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE
)

# Platform-specific source selection
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(isde-mountd PRIVATE src/platform_linux.c)
    target_link_libraries(isde-mountd PkgConfig::UDEV)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    target_sources(isde-mountd PRIVATE src/platform_freebsd.c)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

target_link_libraries(isde-mountd
    isde
    PkgConfig::DBUS
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
