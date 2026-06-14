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
    ${CMAKE_SOURCE_DIR}/platform/X11/common/ewmh.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/randr.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/dpms.c
    ${CMAKE_SOURCE_DIR}/platform/common/dbus.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config-write.c
    ${CMAKE_SOURCE_DIR}/platform/common/vendor/tomlc99/toml.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-theme.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/theme-x11.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-xdg.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-dialog.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-cpufreq.c
)

target_include_directories(isde-settings PRIVATE
    ${CMAKE_SOURCE_DIR}/platform/common
    ${CMAKE_SOURCE_DIR}/platform/X11/common
)

target_compile_definitions(isde-settings PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-settings
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_EWMH
    ISDE::XCB_ICCCM
    ISDE::XCB_UTIL
    ISDE::DBUS
    ISDE::XCB_XKB
    ISDE::CAIRO
    ISDE::FONTCONFIG
    ISDE::XCB_RANDR
    ISDE::XCB_CURSOR
    ISDE::XCB_DPMS
    ${CMAKE_DL_LIBS}
)

install(TARGETS isde-settings RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
