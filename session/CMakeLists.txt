add_executable(isde-session
    src/main.c
    src/session.c
    src/autostart.c
    src/child.c
)

target_compile_definitions(isde-session PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-session
    isde
    PkgConfig::XCB
    PkgConfig::SM
    PkgConfig::ICE
)

install(TARGETS isde-session RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
