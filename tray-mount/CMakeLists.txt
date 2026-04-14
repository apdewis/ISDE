# --- isde-tray-mount (system tray applet for removable disks) ---

add_executable(isde-tray-mount
    src/main.c
    src/tray-mount.c
    src/dbus.c
    src/menu.c
)

target_compile_definitions(isde-tray-mount PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE
)

target_link_libraries(isde-tray-mount
    isde
    PkgConfig::ISW
    PkgConfig::XCB
    PkgConfig::XCB_UTIL
    PkgConfig::DBUS
)

install(TARGETS isde-tray-mount RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
