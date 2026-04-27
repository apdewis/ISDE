# --- isde-tray-net (system tray applet for ConnMan network management) ---

add_executable(isde-tray-net
    src/main.c
    src/tray-net.c
    src/connman.c
    src/agent.c
    src/menu.c
)

target_compile_definitions(isde-tray-net PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE
)

target_link_libraries(isde-tray-net
    isde
    PkgConfig::ISW
    PkgConfig::XCB
    PkgConfig::XCB_UTIL
    PkgConfig::DBUS
)

install(TARGETS isde-tray-net RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
