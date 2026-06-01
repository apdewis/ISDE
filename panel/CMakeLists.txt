add_executable(isde-panel
    src/main.c
    src/panel.c
    src/taskbar.c
    src/startmenu.c
    src/tray.c
    src/clock.c
    src/calendar.c
    src/pager.c
)

target_compile_definitions(isde-panel PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-panel
    isde
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_EWMH
    ISDE::XCB_ICCCM
    ISDE::XCB_UTIL
    ISDE::XCB_KEYSYMS
    ISDE::XKBCOMMON
    ISDE::CAIRO
    ISDE::XCB_CURSOR
)

install(TARGETS isde-panel RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
