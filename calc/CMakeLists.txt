add_executable(isde-calc
    src/main.c
    src/actions.c
    src/math.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config.c
    ${CMAKE_SOURCE_DIR}/platform/common/vendor/tomlc99/toml.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-xdg.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-theme.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/theme-x11.c
    ${CMAKE_SOURCE_DIR}/platform/common/dbus.c
)

target_include_directories(isde-calc PRIVATE
    ${CMAKE_SOURCE_DIR}/platform/common
    ${CMAKE_SOURCE_DIR}/platform/X11/common
)

target_compile_definitions(isde-calc PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE IEEE HAVE_STRLCPY)

target_link_libraries(isde-calc
    ISDE::ISW
    ISDE::XCB_ICCCM
    ISDE::DBUS
    m
)

install(TARGETS isde-calc RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
