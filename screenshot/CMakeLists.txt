add_executable(isde-screenshot
    src/main.c
    src/capture.c
    src/preview.c
    src/save.c
)

target_compile_definitions(isde-screenshot PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-screenshot
    isde
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_XFIXES
    ISDE::CAIRO
    ISDE::DBUS
)

install(TARGETS isde-screenshot RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
