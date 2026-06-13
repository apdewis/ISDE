add_executable(isde-wm-compositor
    src/main.c
    src/compositor.c
    ${CMAKE_SOURCE_DIR}/platform/X11/wm/src/wm.c
    ${CMAKE_SOURCE_DIR}/platform/X11/wm/src/frame.c
    ${CMAKE_SOURCE_DIR}/platform/X11/wm/src/ewmh.c
    ${CMAKE_SOURCE_DIR}/platform/X11/wm/src/placement.c
    ${CMAKE_SOURCE_DIR}/platform/X11/wm/src/desktops.c
    ${CMAKE_SOURCE_DIR}/platform/X11/wm/src/keys.c
    ${CMAKE_SOURCE_DIR}/platform/X11/wm/src/switcher.c
    ${CMAKE_SOURCE_DIR}/platform/X11/wm/src/render.c
)

target_include_directories(isde-wm-compositor PRIVATE
    ${CMAKE_SOURCE_DIR}/platform/X11/wm/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(isde-wm-compositor PRIVATE
    _POSIX_C_SOURCE=200809L
    ISDE_COMPOSITOR=1
)

target_link_libraries(isde-wm-compositor
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
    ISDE::EGL
    ISDE::GL
    ISDE::XCB_PRESENT
    ISDE::XCB_COMPOSITE
    ISDE::XCB_DAMAGE
    ISDE::XCB_RENDER
    ISDE::XCB_SHAPE
    m
)

install(TARGETS isde-wm-compositor RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
