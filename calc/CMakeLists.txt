add_executable(isde-calc
    src/main.c
    src/actions.c
    src/math.c
)

target_compile_definitions(isde-calc PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE IEEE HAVE_STRLCPY)

target_link_libraries(isde-calc
    isde
    PkgConfig::ISW
    PkgConfig::XCB
    PkgConfig::DBUS
    m
)

install(TARGETS isde-calc RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
