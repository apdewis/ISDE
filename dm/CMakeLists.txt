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
    isde
    PkgConfig::XCB
    PkgConfig::DBUS
    PkgConfig::LIBSEAT
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
)

target_compile_definitions(isde-greeter PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE
    DM_CONFIG_PATH="${CMAKE_INSTALL_FULL_SYSCONFDIR}/isde/isde-dm.toml"
)

target_link_libraries(isde-greeter
    isde
    PkgConfig::ISW
    PkgConfig::XCB
    PkgConfig::XCB_RANDR
    PkgConfig::XCB_UTIL
)

install(TARGETS isde-greeter RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
