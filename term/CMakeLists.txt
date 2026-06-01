add_executable(isde-term
    src/main.c
    src/term-widget.c
    src/term-pty.c
    src/term-tsm.c
    src/term-config.c
)

target_include_directories(isde-term PRIVATE src)

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

target_link_libraries(isde-term
    isde
    ISDE::ISW
    ISDE::XCB
    ISDE::XCB_KEYSYMS
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
