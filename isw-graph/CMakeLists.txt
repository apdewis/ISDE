# --- kplot config.h: oconfigure feature detection reproduced in CMake ---
# configure_file fills vendor/kplot/config.h.in (the verbatim oconfigure
# template) from the checks below. No shell ./configure is run.
include(CheckSymbolExists)
include(CheckIncludeFile)
include(CheckCSourceCompiles)

# Match oconfigure: probe with the GNU/BSD extensions visible.
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE -D_DEFAULT_SOURCE)

check_symbol_exists(arc4random          "stdlib.h"      HAVE_ARC4RANDOM)
check_symbol_exists(explicit_bzero      "string.h"      HAVE_EXPLICIT_BZERO)
# oconfigure keys HAVE_ERR on the full BSD family; glibc lacks errc().
check_symbol_exists(errc                "err.h"         HAVE_ERR)
check_symbol_exists(getprogname         "stdlib.h"      HAVE_GETPROGNAME)
check_symbol_exists(getexecname         "stdlib.h"      HAVE_GETEXECNAME)
check_symbol_exists(memset_s            "string.h"      HAVE_MEMSET_S)
check_symbol_exists(memmem              "string.h"      HAVE_MEMMEM)
check_symbol_exists(memrchr             "string.h"      HAVE_MEMRCHR)
check_symbol_exists(reallocarray        "stdlib.h"      HAVE_REALLOCARRAY)
check_symbol_exists(recallocarray       "stdlib.h"      HAVE_RECALLOCARRAY)
check_symbol_exists(strlcat             "string.h"      HAVE_STRLCAT)
check_symbol_exists(strlcpy             "string.h"      HAVE_STRLCPY)
check_symbol_exists(strndup             "string.h"      HAVE_STRNDUP)
check_symbol_exists(strnlen             "string.h"      HAVE_STRNLEN)
check_symbol_exists(strtonum            "stdlib.h"      HAVE_STRTONUM)
check_symbol_exists(mkfifoat            "sys/stat.h"    HAVE_MKFIFOAT)
check_symbol_exists(mknodat             "sys/stat.h"    HAVE_MKNODAT)
check_symbol_exists(setresgid           "unistd.h"      HAVE_SETRESGID)
check_symbol_exists(setresuid           "unistd.h"      HAVE_SETRESUID)
check_symbol_exists(pledge              "unistd.h"      HAVE_PLEDGE)
check_symbol_exists(unveil              "unistd.h"      HAVE_UNVEIL)
check_symbol_exists(timingsafe_bcmp     "string.h"      HAVE_TIMINGSAFE_BCMP)
check_symbol_exists(readpassphrase      "readpassphrase.h" HAVE_READPASSPHRASE)
check_symbol_exists(scan_scaled         "util.h"        HAVE_SCAN_SCALED)
set(CMAKE_REQUIRED_LIBRARIES crypt)
check_symbol_exists(crypt               "crypt.h"       HAVE_CRYPT)
check_symbol_exists(crypt_newhash       "crypt.h"       HAVE_CRYPT_NEWHASH)
unset(CMAKE_REQUIRED_LIBRARIES)
check_symbol_exists(PATH_MAX            "limits.h"      HAVE_PATH_MAX)
check_symbol_exists(INFTIM              "poll.h"        HAVE_INFTIM)
check_symbol_exists(_PASSWORD_LEN       "pwd.h"         HAVE_PASSWORD_LEN)
check_symbol_exists(SOCK_NONBLOCK       "sys/socket.h"  HAVE_SOCK_NONBLOCK)
check_symbol_exists(WAIT_ANY            "sys/wait.h"    HAVE_WAIT_ANY)

check_include_file("endian.h"               HAVE_ENDIAN_H)
check_include_file("sys/endian.h"           HAVE_SYS_ENDIAN_H)
check_include_file("sys/byteorder.h"        HAVE_SYS_BYTEORDER_H)
check_include_file("libkern/OSByteOrder.h"  HAVE_OSBYTEORDER_H)
check_include_file("sys/mkdev.h"            HAVE_SYS_MKDEV_H)
check_include_file("sys/sysmacros.h"        HAVE_SYS_SYSMACROS_H)
check_include_file("fts.h"                  HAVE_FTS)
check_include_file("md5.h"                  HAVE_MD5)
check_include_file("sha2.h"                 HAVE_SHA2_H)
check_include_file("termios.h"              HAVE_TERMIOS)
check_include_file("sys/capsicum.h"         HAVE_CAPSICUM)
check_include_file("sha2.h"                 HAVE_SHA2)
check_include_file("linux/landlock.h"       HAVE_LANDLOCK)
check_include_file("linux/seccomp.h"        HAVE_SECCOMP_HEADER)

check_c_source_compiles(
    "extern char *__progname; int main(void){return *__progname;}"
    HAVE___PROGNAME)
check_c_source_compiles(
    "#define _GNU_SOURCE\n#include <errno.h>\nint main(void){return *program_invocation_short_name;}"
    HAVE_PROGRAM_INVOCATION_SHORT_NAME)
set(CMAKE_REQUIRED_LIBRARIES resolv)
check_c_source_compiles(
    "#include <netinet/in.h>\n#include <resolv.h>\nint main(void){return b64_ntop((void*)0,0,(void*)0,0);}"
    HAVE_B64_NTOP)
unset(CMAKE_REQUIRED_LIBRARIES)

# oconfigure always ships its own queue.h/tree.h/blowfish, so these stay 0
# (the bundled copies live unconditionally in config.h.in).
set(HAVE_SYS_QUEUE 0)
set(HAVE_SYS_TREE 0)
set(HAVE_BLOWFISH 0)
set(HAVE_SANDBOX_INIT 0)
set(HAVE_SYSTRACE 0)

unset(CMAKE_REQUIRED_DEFINITIONS)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/kplot/config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/kplot-gen/config.h)

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

# The Graph sources need _POSIX_C_SOURCE; the kplot sources must NOT get it.
# kplot's config.h (included first in every kplot .c) sets the correct per-OS
# feature macros itself (oconfigure design). Forcing _POSIX_C_SOURCE on them
# defines it before config.h runs, which on FreeBSD clears __BSD_VISIBLE and
# hides reallocarray (declared in <stdlib.h> under #if __BSD_VISIBLE).
set_source_files_properties(
    src/GraphCommon.c
    src/GraphLine.c
    src/GraphBar.c
    src/GraphScatter.c
    PROPERTIES COMPILE_DEFINITIONS _POSIX_C_SOURCE=200809L)

target_include_directories(isw-graph
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/kplot>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/kplot
        ${CMAKE_CURRENT_BINARY_DIR}/kplot-gen
)

target_link_libraries(isw-graph
    PUBLIC
        ISDE::ISW
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
