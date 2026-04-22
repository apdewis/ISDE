pkg_check_modules(TSM REQUIRED IMPORTED_TARGET libtsm)

add_executable(isde-term
    src/main.c
    src/term-widget.c
    src/term-pty.c
    src/term-tsm.c
    src/term-config.c
)

target_include_directories(isde-term PRIVATE src)

target_compile_definitions(isde-term PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-term
    isde
    PkgConfig::ISW
    PkgConfig::XCB
    PkgConfig::XCB_KEYSYMS
    PkgConfig::XCB_ICCCM
    PkgConfig::CAIRO
    PkgConfig::CAIRO_XCB
    PkgConfig::FONTCONFIG
    PkgConfig::FREETYPE
    PkgConfig::TSM
    PkgConfig::DBUS
    util
)

install(TARGETS isde-term RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
