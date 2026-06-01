set(KPLOT_SOURCES
    vendor/kplot/array.c
    vendor/kplot/border.c
    vendor/kplot/bucket.c
    vendor/kplot/buffer.c
    vendor/kplot/colours.c
    vendor/kplot/draw.c
    vendor/kplot/grid.c
    vendor/kplot/hist.c
    vendor/kplot/label.c
    vendor/kplot/kdata.c
    vendor/kplot/kplot.c
    vendor/kplot/margin.c
    vendor/kplot/mean.c
    vendor/kplot/plotctx.c
    vendor/kplot/stddev.c
    vendor/kplot/tic.c
    vendor/kplot/vector.c
)

add_library(isw-graph SHARED
    src/GraphCommon.c
    src/GraphLine.c
    src/GraphBar.c
    src/GraphScatter.c
    ${KPLOT_SOURCES}
)

target_compile_definitions(isw-graph PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_include_directories(isw-graph
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/kplot>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/kplot
)

target_link_libraries(isw-graph
    PUBLIC
        ISDE::ISW
        ISDE::CAIRO
    PRIVATE
        m
)

set_target_properties(isw-graph PROPERTIES
    VERSION   "${PROJECT_VERSION}"
    SOVERSION "0"
    OUTPUT_NAME "isw-graph"
)

# --- pkg-config file ---
configure_file(isw-graph.pc.in isw-graph.pc @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/isw-graph.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

# --- Install ---
install(TARGETS isw-graph
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/isw-graph/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/isw-graph"
    PATTERN "*P.h" EXCLUDE)

# --- Demo ---
add_executable(isw-graph-demo demo/demo.c)
target_link_libraries(isw-graph-demo
    isw-graph ISDE::ISW ISDE::XCB_ICCCM m)
