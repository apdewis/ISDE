add_executable(isde-fm
    src/main.c
    src/fm.c
    src/dialogs.c
    src/context.c
    src/shortcuts.c
    src/navigate.c
    src/mounts.c
    src/browser.c
    src/fileview.c
    src/navbar.c
    src/icons.c
    src/fileops.c
    src/clipboard.c
    src/dnd.c
    src/daemon.c
    src/jobqueue.c
    src/places.c
    src/fm_mountd.c
    src/actions.c
    src/thumbs.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/ewmh.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/desktop.c
    ${CMAKE_SOURCE_DIR}/platform/common/dbus.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config.c
    ${CMAKE_SOURCE_DIR}/platform/common/vendor/tomlc99/toml.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-theme.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/theme-x11.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-xdg.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-mime.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-dialog.c
)

target_include_directories(isde-fm PRIVATE
    ${CMAKE_SOURCE_DIR}/platform/common
    ${CMAKE_SOURCE_DIR}/platform/X11/common
)

target_compile_definitions(isde-fm PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

find_package(Threads REQUIRED)

target_link_libraries(isde-fm
    isw-graph
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_EWMH
    ISDE::XCB_ICCCM
    ISDE::XCB_UTIL
    Threads::Threads
    ISDE::DBUS
    ISDE::CAIRO
    ISDE::LIBJPEG
    ISDE::XCB_CURSOR
)

install(TARGETS isde-fm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
