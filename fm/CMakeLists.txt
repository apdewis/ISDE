add_executable(isde-fm
    src/main.c
    src/fm.c
    src/browser.c
    src/fileview.c
    src/navbar.c
    src/icons.c
    src/fileops.c
    src/clipboard.c
    src/dnd.c
    src/instance.c
    src/jobqueue.c
    src/progress.c
    src/places.c
)

target_compile_definitions(isde-fm PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

find_package(Threads REQUIRED)

target_link_libraries(isde-fm
    isde
    PkgConfig::ISW
    PkgConfig::XCB
    PkgConfig::XCB_EWMH
    PkgConfig::XCB_ICCCM
    PkgConfig::XCB_UTIL
    Threads::Threads
)

install(TARGETS isde-fm RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
