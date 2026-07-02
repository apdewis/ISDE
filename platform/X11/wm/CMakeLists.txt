add_executable(isde-wm
    src/main.c
    src/wm.c
    src/frame.c
    src/ewmh.c
    src/placement.c
    src/desktops.c
    src/keys.c
    src/switcher.c
    src/render.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/randr.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/isde-monitor-xcb.c
    ${CMAKE_SOURCE_DIR}/platform/common/dbus.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config-write.c
    ${CMAKE_SOURCE_DIR}/platform/common/vendor/tomlc99/toml.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-theme.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/theme-x11.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-xdg.c
)

target_include_directories(isde-wm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/platform/common
    ${CMAKE_SOURCE_DIR}/platform/X11/common
)

target_compile_definitions(isde-wm PRIVATE _POSIX_C_SOURCE=200809L)

target_link_libraries(isde-wm
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_EWMH
    ISDE::XCB_ICCCM
    ISDE::XCB_KEYSYMS
    ISDE::XCB_XRM
    ISDE::XCB_UTIL
    ISDE::XCB_RANDR
    ISDE::XCB_SYNC
    ISDE::CAIRO
    ISDE::CAIRO_XCB
    ISDE::FONTCONFIG
    ISDE::FREETYPE
    ISDE::XCB_CURSOR
    ISDE::DBUS
    m
)

install(TARGETS isde-wm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
