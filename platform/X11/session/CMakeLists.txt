add_executable(isde-session-x11
    src/main.c
    src/session.c
    ${CMAKE_SOURCE_DIR}/platform/common/session/autostart.c
    ${CMAKE_SOURCE_DIR}/platform/common/session/child.c
    ${CMAKE_SOURCE_DIR}/platform/common/session/dbus.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/ewmh.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/dpms.c
)

target_compile_definitions(isde-session-x11 PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-session-x11
    isde
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_XKB
    ISDE::XCB_KEYSYMS
    ISDE::XCB_DPMS
    ISDE::XCB_SCREENSAVER
    ISDE::DBUS
)

add_executable(isde-confirm
    src/confirm.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/ewmh.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/randr.c
)

target_compile_definitions(isde-confirm PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-confirm
    isde
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_KEYSYMS
)

install(TARGETS isde-session-x11 isde-confirm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
