# --- isde-tray-bt (system tray applet for BlueZ bluetooth management) ---

add_executable(isde-tray-bt
    src/main.c
    src/tray-bt.c
    src/bluez.c
    src/agent.c
    src/menu.c
)

target_compile_definitions(isde-tray-bt PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE
)

target_link_libraries(isde-tray-bt
    isde
    PkgConfig::ISW
    PkgConfig::XCB
    PkgConfig::XCB_UTIL
    PkgConfig::DBUS
)

install(TARGETS isde-tray-bt RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
