add_executable(isde-displayd
    src/main.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config.c
    ${CMAKE_SOURCE_DIR}/platform/common/vendor/tomlc99/toml.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-xdg.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-theme.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/theme-x11.c
    ${CMAKE_SOURCE_DIR}/platform/common/dbus.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/randr.c
)

target_include_directories(isde-displayd PRIVATE
    ${CMAKE_SOURCE_DIR}/platform/common
    ${CMAKE_SOURCE_DIR}/platform/X11/common
)

target_compile_definitions(isde-displayd PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

target_link_libraries(isde-displayd
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_RANDR
    ISDE::XCB_DPMS
    ISDE::XCB_EWMH
    ISDE::XCB_ICCCM
    ISDE::DBUS
    ISDE::CAIRO
)

install(TARGETS isde-displayd RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
