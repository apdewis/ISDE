# Shared platform-layer sources (decoupled from the central libisde).
set(SESSION_PLATFORM_SRC
    ${CMAKE_SOURCE_DIR}/platform/common/dbus.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config-write.c
    ${CMAKE_SOURCE_DIR}/platform/common/vendor/tomlc99/toml.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-theme.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/theme-x11.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-xdg.c
)

set(SESSION_PLATFORM_INCLUDE
    ${CMAKE_SOURCE_DIR}/platform/common
    ${CMAKE_SOURCE_DIR}/platform/X11/common
)

add_executable(isde-session-x11
    src/main.c
    src/session.c
    ${CMAKE_SOURCE_DIR}/platform/common/session/autostart.c
    ${CMAKE_SOURCE_DIR}/platform/common/session/child.c
    ${CMAKE_SOURCE_DIR}/platform/common/session/dbus.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/ewmh.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/dpms.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/desktop.c
    ${SESSION_PLATFORM_SRC}
)

target_include_directories(isde-session-x11 PRIVATE ${SESSION_PLATFORM_INCLUDE})

target_compile_definitions(isde-session-x11 PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-session-x11
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_EWMH
    ISDE::XCB_ICCCM
    ISDE::XCB_UTIL
    ISDE::XCB_RANDR
    ISDE::XCB_XKB
    ISDE::XCB_KEYSYMS
    ISDE::XCB_DPMS
    ISDE::XCB_SCREENSAVER
    ISDE::CAIRO
    ISDE::FONTCONFIG
    ISDE::DBUS
)

add_executable(isde-confirm
    src/confirm.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/ewmh.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/randr.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/isde-monitor-xcb.c
    ${SESSION_PLATFORM_SRC}
)

target_include_directories(isde-confirm PRIVATE ${SESSION_PLATFORM_INCLUDE})

target_compile_definitions(isde-confirm PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-confirm
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_EWMH
    ISDE::XCB_ICCCM
    ISDE::XCB_UTIL
    ISDE::XCB_RANDR
    ISDE::XCB_KEYSYMS
    ISDE::CAIRO
    ISDE::FONTCONFIG
    ISDE::DBUS
)

install(TARGETS isde-session-x11 isde-confirm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
