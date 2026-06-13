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
    ${CMAKE_SOURCE_DIR}/platform/X11/common/randr.c
)

target_include_directories(isde-wm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(isde-wm PRIVATE _POSIX_C_SOURCE=200809L)

target_link_libraries(isde-wm
    isde
    ISDE::XCB
    ISDE::XCB_EWMH
    ISDE::XCB_ICCCM
    ISDE::XCB_KEYSYMS
    ISDE::XCB_XRM
    ISDE::XCB_UTIL
    ISDE::CAIRO
    ISDE::CAIRO_XCB
    ISDE::FONTCONFIG
    ISDE::FREETYPE
    ISDE::XCB_CURSOR
    m
)

install(TARGETS isde-wm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
