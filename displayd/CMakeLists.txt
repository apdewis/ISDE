add_executable(isde-displayd
    src/main.c
)

target_compile_definitions(isde-displayd PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-displayd
    isde
    PkgConfig::XCB
    PkgConfig::DBUS
)

install(TARGETS isde-displayd RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
