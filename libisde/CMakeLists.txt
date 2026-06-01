add_library(isde SHARED
    src/isde-config.c
    src/isde-config-write.c
    src/isde-xdg.c
    src/isde-desktop.c
    src/isde-ewmh.c
    src/isde-ipc.c
    src/isde-dbus.c
    src/isde-mime.c
    src/isde-theme.c
    src/isde-dialog.c
    src/isde-filechooser.c
    src/isde-tray.c
    src/isde-randr.c
    src/isde-power.c
    src/isde-dpms.c
    src/isde-cpufreq.c
    vendor/tomlc99/toml.c
)

target_compile_definitions(isde PRIVATE _POSIX_C_SOURCE=200809L)

target_include_directories(isde
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/tomlc99
)

target_link_libraries(isde
    PUBLIC
        ISDE::XCB
        ISDE::XCB_EWMH
        ISDE::XCB_ICCCM
        ISDE::XCB_UTIL
        ISDE::XCB_RANDR
        ISDE::ISW
    PRIVATE
        ISDE::DBUS
        ISDE::XCB_DPMS
        ISDE::FONTCONFIG
)

set_target_properties(isde PROPERTIES
    VERSION   "${PROJECT_VERSION}"
    SOVERSION "0"
    OUTPUT_NAME "isde"
)

# --- pkg-config file ---
configure_file(isde.pc.in isde.pc @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/isde.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

# --- Install ---
install(TARGETS isde
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/isde/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/isde")
