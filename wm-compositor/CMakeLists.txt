add_executable(isde-wm-compositor
    src/main.c
    src/compositor.c
    ${CMAKE_SOURCE_DIR}/wm/src/wm.c
    ${CMAKE_SOURCE_DIR}/wm/src/frame.c
    ${CMAKE_SOURCE_DIR}/wm/src/ewmh.c
    ${CMAKE_SOURCE_DIR}/wm/src/placement.c
    ${CMAKE_SOURCE_DIR}/wm/src/desktops.c
    ${CMAKE_SOURCE_DIR}/wm/src/keys.c
    ${CMAKE_SOURCE_DIR}/wm/src/switcher.c
    ${CMAKE_SOURCE_DIR}/wm/src/render.c
)

target_include_directories(isde-wm-compositor PRIVATE
    ${CMAKE_SOURCE_DIR}/wm/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(isde-wm-compositor PRIVATE
    _POSIX_C_SOURCE=200809L
    ISDE_COMPOSITOR=1
)

target_link_libraries(isde-wm-compositor
    isde
    PkgConfig::XCB
    PkgConfig::XCB_EWMH
    PkgConfig::XCB_ICCCM
    PkgConfig::XCB_KEYSYMS
    PkgConfig::XCB_UTIL
    PkgConfig::CAIRO
    PkgConfig::CAIRO_XCB
    PkgConfig::FONTCONFIG
    PkgConfig::FREETYPE
    PkgConfig::XCB_CURSOR
    PkgConfig::EGL
    PkgConfig::GL
    PkgConfig::XCB_PRESENT
    PkgConfig::XCB_COMPOSITE
    PkgConfig::XCB_DAMAGE
    PkgConfig::XCB_RENDER
    m
)

install(TARGETS isde-wm-compositor RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
