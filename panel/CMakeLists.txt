add_executable(isde-panel
    src/main.c
    src/panel.c
    src/taskbar.c
    src/startmenu.c
    src/tray.c
    src/clock.c
)

target_compile_definitions(isde-panel PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-panel
    isde
    PkgConfig::ISW
    PkgConfig::XCB
    PkgConfig::XCB_EWMH
    PkgConfig::XCB_ICCCM
    PkgConfig::XCB_UTIL
    PkgConfig::XCB_KEYSYMS
)

install(TARGETS isde-panel RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
