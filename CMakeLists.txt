cmake_minimum_required (VERSION 3.5)
project (mg)

set (MG_SRC autoexec.c basic.c bell.c buffer.c cinfo.c dir.c display.c
	echo.c extend.c file.c fileio.c funmap.c help.c kbd.c keymap.c
	line.c llm.c macro.c main.c match.c modes.c paragraph.c
	re_search.c region.c search.c spawn.c tty.c ttyio.c ttykbd.c
	undo.c util.c version.c window.c word.c yank.c
	cmode.c dired.c grep.c hexmode.c interpreter.c makemode.c
)

add_executable (mg ${MG_SRC})

find_package (PkgConfig REQUIRED)

if(APPLE)
  # Homebrew libbsd and ncurses are keg-only, so their pkg-config files are not
  # on the default search path. Expose them before any pkg_check_modules call.
  execute_process(COMMAND brew --prefix libbsd
    OUTPUT_VARIABLE LIBBSD_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND brew --prefix ncurses
    OUTPUT_VARIABLE NCURSES_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(ENV{PKG_CONFIG_PATH}
    "${LIBBSD_PREFIX}/lib/pkgconfig:${NCURSES_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

pkg_check_modules (NCURSES REQUIRED ncurses)
string (REPLACE ";" " -I" NCURSES_FLAGS "${NCURSES_INCLUDE_DIRS}")
set (NCURSES_FLAGS "-I${NCURSES_FLAGS}")
target_link_libraries (mg ${NCURSES_LDFLAGS})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  pkg_check_modules (BSD REQUIRED libbsd-overlay)
  link_directories (${BSD_LIBRARY_DIRS})
  add_definitions(-DHAVE_PTY_H)
  string (REPLACE ";" " " LIBBSD_FLAGS "${BSD_CFLAGS}")
  target_link_libraries (mg ${BSD_LIBRARIES} util)
  set (CMAKE_C_FLAGS "-Wall -DREGEX -D_GNU_SOURCE ${LIBBSD_FLAGS} ${NCURSES_FLAGS} -L${NCURSES_LIBRARY_DIRS}")
elseif(APPLE)
  # macOS: libbsd-overlay supplies strtonum; openpty/login_tty/fparseln are in
  # the system <util.h> (HAVE_UTIL_H). No libutil.dylib exists, so do not link util.
  # Link via BSD_LDFLAGS so the keg-only -L path reaches the linker (link_directories
  # has no effect here because the target is already defined above).
  pkg_check_modules (BSD REQUIRED libbsd-overlay)
  add_definitions(-DHAVE_UTIL_H)
  string (REPLACE ";" " " LIBBSD_FLAGS "${BSD_CFLAGS}")
  target_link_libraries (mg ${BSD_LDFLAGS})
  set (CMAKE_C_FLAGS "-Wall -DREGEX -D_DARWIN_C_SOURCE ${LIBBSD_FLAGS} ${NCURSES_FLAGS}")
else()
  target_link_libraries (mg util)
  set (CMAKE_C_FLAGS "-Wall -DREGEX ${LIBBSD_FLAGS} ${NCURSES_FLAGS} -L${NCURSES_LIBRARY_DIRS}")
endif()
