add_library(isde SHARED
    src/isde-config.c
    src/isde-xdg.c
    src/isde-desktop.c
    src/isde-ewmh.c
    src/isde-ipc.c
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
        PkgConfig::XCB
        PkgConfig::XCB_EWMH
        PkgConfig::XCB_ICCCM
        PkgConfig::XCB_UTIL
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
