# --- isde-dm (privileged daemon) ---

add_executable(isde-dm
    src/main.c
    src/dm.c
    src/seat.c
    src/auth.c
    src/ipc.c
    src/session.c
    src/greeter_proc.c
    src/power.c
    src/dbus.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config-write.c
    ${CMAKE_SOURCE_DIR}/platform/common/vendor/tomlc99/toml.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-xdg.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-theme.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/theme-x11.c
    ${CMAKE_SOURCE_DIR}/platform/common/dbus.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/desktop.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/ewmh.c
)

target_include_directories(isde-dm PRIVATE
    ${CMAKE_SOURCE_DIR}/platform/common
    ${CMAKE_SOURCE_DIR}/platform/X11/common
)

target_compile_definitions(isde-dm PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE
    DM_CONFIG_PATH="${CMAKE_INSTALL_FULL_SYSCONFDIR}/isde/isde-dm.toml"
)

# Platform-specific source selection
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(isde-dm PRIVATE src/platform_linux.c)
    set(DM_PAM_DIR "/etc/pam.d" CACHE PATH "PAM service directory")
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    target_sources(isde-dm PRIVATE src/platform_freebsd.c)
    target_link_libraries(isde-dm PRIVATE util)
    set(DM_PAM_DIR "${CMAKE_INSTALL_SYSCONFDIR}/pam.d" CACHE PATH "PAM service directory")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

target_link_libraries(isde-dm
    PRIVATE
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_EWMH
    ISDE::XCB_ICCCM
    ISDE::XCB_UTIL
    ISDE::DBUS
    ISDE::CAIRO
    ISDE::LIBSEAT
    pam_imported
)

install(TARGETS isde-dm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES data/isde-dm.pam DESTINATION "${DM_PAM_DIR}" RENAME isde-dm)
install(FILES data/org.isde.DisplayManager.conf
        DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/system.d")
install(FILES data/isde-dm.toml
        DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/isde"
        RENAME isde-dm.toml.example)

# --- isde-greeter (unprivileged UI) ---

add_executable(isde-greeter
    src/greeter/main.c
    src/greeter/greeter.c
    src/greeter/clock.c
    src/greeter/sessions.c
    src/greeter/ipc.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config.c
    ${CMAKE_SOURCE_DIR}/platform/common/vendor/tomlc99/toml.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-xdg.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-theme.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/theme-x11.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/desktop.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/ewmh.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/randr.c
    ${CMAKE_SOURCE_DIR}/platform/common/dbus.c
)

target_include_directories(isde-greeter PRIVATE
    ${CMAKE_SOURCE_DIR}/platform/common
    ${CMAKE_SOURCE_DIR}/platform/X11/common
)

target_compile_definitions(isde-greeter PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE
    DM_CONFIG_PATH="${CMAKE_INSTALL_FULL_SYSCONFDIR}/isde/isde-dm.toml"
)

target_link_libraries(isde-greeter
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_EWMH
    ISDE::XCB_ICCCM
    ISDE::XCB_UTIL
    ISDE::XCB_RANDR
    ISDE::CAIRO
    ISDE::DBUS
)

install(TARGETS isde-greeter RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# --- Service unit files (opt-in install components) ---

install(FILES data/isde-dm.service
        DESTINATION "${ISDE_SYSTEMD_UNIT_DIR}"
        COMPONENT systemd
        EXCLUDE_FROM_ALL)

install(PROGRAMS data/runit/run data/runit/finish
        DESTINATION "${ISDE_RUNIT_SERVICE_DIR}/isde-dm"
        COMPONENT runit
        EXCLUDE_FROM_ALL)

configure_file(data/rc.d/isde-dm.in
        "${CMAKE_CURRENT_BINARY_DIR}/rc.d/isde-dm" @ONLY)
install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/rc.d/isde-dm"
        DESTINATION "${ISDE_RC_D_DIR}"
        COMPONENT rc_d
        EXCLUDE_FROM_ALL)
