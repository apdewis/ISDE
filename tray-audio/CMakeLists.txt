# --- isde-tray-audio (system tray applet for audio volume control) ---

add_executable(isde-tray-audio
    src/main.c
    src/tray-audio.c
    src/pipewire.c
    src/popup.c
)

target_compile_definitions(isde-tray-audio PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE
)

target_link_libraries(isde-tray-audio
    isde
    PkgConfig::ISW
    PkgConfig::XCB
    PkgConfig::XCB_UTIL
    PkgConfig::PIPEWIRE
    m
)

install(TARGETS isde-tray-audio RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
