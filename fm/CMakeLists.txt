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
    src/instance.c
    src/jobqueue.c
    src/places.c
    src/fm_mountd.c
    src/actions.c
    src/thumbs.c
)

target_compile_definitions(isde-fm PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

find_package(Threads REQUIRED)

target_link_libraries(isde-fm
    isde
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
