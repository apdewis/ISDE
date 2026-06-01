add_executable(isde-session
    src/main.c
    src/session.c
    src/autostart.c
    src/child.c
)

target_compile_definitions(isde-session PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-session
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
)

target_compile_definitions(isde-confirm PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-confirm
    isde
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_KEYSYMS
)

install(TARGETS isde-session isde-confirm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
