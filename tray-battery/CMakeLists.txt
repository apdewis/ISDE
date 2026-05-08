# --- isde-tray-battery (system tray applet for battery status) ---

add_executable(isde-tray-battery
    src/main.c
    src/tray-battery.c
    src/popup.c
)

target_compile_definitions(isde-tray-battery PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE
)

target_link_libraries(isde-tray-battery
    isde
    PkgConfig::ISW
    PkgConfig::XCB
    PkgConfig::XCB_UTIL
)

install(TARGETS isde-tray-battery RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
