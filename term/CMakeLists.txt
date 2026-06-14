add_executable(isde-term
    src/main.c
    src/term-widget.c
    src/term-pty.c
    src/term-tsm.c
    src/term-config.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-config.c
    ${CMAKE_SOURCE_DIR}/platform/common/vendor/tomlc99/toml.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-xdg.c
    ${CMAKE_SOURCE_DIR}/platform/common/isde-theme.c
    ${CMAKE_SOURCE_DIR}/platform/X11/common/theme-x11.c
    ${CMAKE_SOURCE_DIR}/platform/common/dbus.c
)

target_include_directories(isde-term PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/platform/common
    ${CMAKE_SOURCE_DIR}/platform/X11/common
)

target_compile_definitions(isde-term PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)

# libtsm's tsm_screen_draw_cb cell-id parameter is uint32_t in the upstream
# API but uint64_t in the Aetf fork, with no version macro to tell them apart.
# Detect which the installed copy uses so draw_cell_cb's signature matches
# exactly — clang errors on a function-pointer-type mismatch, GCC only warns.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES ISDE::TSM)
set(CMAKE_REQUIRED_FLAGS -Werror)
check_c_source_compiles("
#include <stdint.h>
#include <stddef.h>
#include <libtsm.h>
static int cb(struct tsm_screen *c, uint64_t id, const uint32_t *ch, size_t len,
              unsigned int w, unsigned int x, unsigned int y,
              const struct tsm_screen_attr *a, tsm_age_t age, void *d)
{ (void)c;(void)id;(void)ch;(void)len;(void)w;(void)x;(void)y;(void)a;(void)age;(void)d; return 0; }
int main(void) { tsm_screen_draw_cb p = cb; (void)p; return 0; }
" TSM_DRAW_ID_UINT64)
unset(CMAKE_REQUIRED_LIBRARIES)
unset(CMAKE_REQUIRED_FLAGS)
if(TSM_DRAW_ID_UINT64)
    target_compile_definitions(isde-term PRIVATE TSM_DRAW_ID_TYPE=uint64_t)
else()
    target_compile_definitions(isde-term PRIVATE TSM_DRAW_ID_TYPE=uint32_t)
endif()

# Custom RGB palettes (tsm_vte_set_custom_palette + the TSM_COLOR_* enum) are an
# Aetf-fork extension; upstream libtsm only offers built-in named palettes.
include(CheckSymbolExists)
set(CMAKE_REQUIRED_LIBRARIES ISDE::TSM)
check_symbol_exists(tsm_vte_set_custom_palette libtsm.h HAVE_TSM_CUSTOM_PALETTE)
# The OSC callback (used for terminal-driven window titles) is likewise an
# Aetf-fork extension absent from upstream libtsm.
check_symbol_exists(tsm_vte_set_osc_cb libtsm.h HAVE_TSM_OSC_CB)
unset(CMAKE_REQUIRED_LIBRARIES)
if(HAVE_TSM_CUSTOM_PALETTE)
    target_compile_definitions(isde-term PRIVATE HAVE_TSM_CUSTOM_PALETTE)
endif()
if(HAVE_TSM_OSC_CB)
    target_compile_definitions(isde-term PRIVATE HAVE_TSM_OSC_CB)
endif()

target_link_libraries(isde-term
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_ICCCM
    ISDE::CAIRO
    ISDE::CAIRO_XCB
    ISDE::CAIRO_FT
    ISDE::FONTCONFIG
    ISDE::FREETYPE
    ISDE::TSM
    ISDE::DBUS
    util
)

install(TARGETS isde-term RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
