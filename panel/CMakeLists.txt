add_executable(isde-panel-x11
    src/main.c
    src/panel.c
    src/panel-x11.c
    src/taskbar.c
    src/startmenu.c
    src/tray.c
    src/tray-net.c
    src/tray-net-connman.c
    src/tray-net-agent.c
    src/tray-net-menu.c
    src/tray-audio.c
    src/tray-audio-pipewire.c
    src/tray-audio-popup.c
    src/tray-battery.c
    src/tray-battery-popup.c
    src/tray-bt.c
    src/tray-bt-bluez.c
    src/tray-bt-agent.c
    src/tray-bt-menu.c
    src/tray-mount.c
    src/tray-mount-dbus.c
    src/tray-mount-popup.c
    src/tray-mount-password.c
    src/clock.c
    src/calendar.c
    #src/pager.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-dialog.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-power.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-cpufreq.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/ewmh.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/randr.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/isde-monitor-xcb.c
    ${CMAKE_SOURCE_DIR}/platform/common/dbus.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/desktop.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config-write.c
    ${CMAKE_SOURCE_DIR}/platform/common/vendor/tomlc99/toml.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-theme.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/theme-x11.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-xdg.c
)

target_include_directories(isde-panel-x11 PRIVATE
    ${CMAKE_SOURCE_DIR}/platform/common
    ${CMAKE_SOURCE_DIR}/platform/X11/common
)

target_compile_definitions(isde-panel-x11 PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-panel-x11
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_EWMH
    ISDE::XCB_ICCCM
    ISDE::XCB_UTIL
    ISDE::XCB_KEYSYMS
    ISDE::XKBCOMMON
    ISDE::CAIRO
    ISDE::XCB_CURSOR
    ISDE::DBUS
    ISDE::XCB_RANDR
    ISDE::PIPEWIRE
    m
)

install(TARGETS isde-panel-x11 RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
