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
)

target_include_directories(isde-wm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(isde-wm PRIVATE _POSIX_C_SOURCE=200809L)

target_link_libraries(isde-wm
    isde
    PkgConfig::XCB
    PkgConfig::XCB_EWMH
    PkgConfig::XCB_ICCCM
    PkgConfig::XCB_KEYSYMS
    PkgConfig::XCB_XRM
    PkgConfig::XCB_UTIL
    PkgConfig::CAIRO
    PkgConfig::CAIRO_XCB
    PkgConfig::FONTCONFIG
    PkgConfig::FREETYPE
    PkgConfig::XCB_CURSOR
    m
)

install(TARGETS isde-wm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
