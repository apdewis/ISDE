add_executable(isde-session
    src/main.c
    src/session.c
    src/autostart.c
    src/child.c
)

target_compile_definitions(isde-session PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-session
    isde
    PkgConfig::ISW
    PkgConfig::XCB
    PkgConfig::XCB_XKB
    PkgConfig::XCB_KEYSYMS
    PkgConfig::DBUS
)

add_executable(isde-confirm
    src/confirm.c
)

target_compile_definitions(isde-confirm PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-confirm
    isde
    PkgConfig::ISW
    PkgConfig::XCB
    PkgConfig::XCB_KEYSYMS
)

install(TARGETS isde-session isde-confirm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
