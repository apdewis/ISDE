add_executable(isde-settings
    src/main.c
    src/settings.c
    src/panel-input.c
    src/panel-keyboard.c
    src/panel-appearance.c
    src/panel-display.c
    src/panel-desktops.c
    src/panel-fonts.c
    src/panel-terminal.c
    src/panel-dm.c
    src/panel-power.c
)

target_compile_definitions(isde-settings PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-settings
    isde
    ISDE::ISW
    ISDE::XCB
    ISDE::DBUS
    ISDE::XCB_XKB
    ISDE::CAIRO
    ISDE::FONTCONFIG
    ${CMAKE_DL_LIBS}
)

install(TARGETS isde-settings RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
