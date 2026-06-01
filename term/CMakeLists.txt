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
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_KEYSYMS
    ISDE::XCB_ICCCM
    ISDE::CAIRO
    ISDE::CAIRO_XCB
    ISDE::CAIRO_FT
    ISDE::FONTCONFIG
    ISDE::FREETYPE
    ISDE::TSM
    ISDE::DBUS
    util
)

install(TARGETS isde-term RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
