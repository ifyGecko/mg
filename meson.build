# meson.buid file for mg

# This file has been written by Han Boetes <hboetes@gmail.com> and is
# released in Public Domain.

project(
  'mg', 'c',
  version: run_command(
    'sh', '-c',
    'git describe --tags 2>/dev/null || echo 0-unknown',
    check: false).stdout().strip(),
  default_options: ['warning_level=1'],
)

host_system = host_machine.system()

if host_system == 'darwin'
  # macOS: Homebrew libbsd + ncurses are keg-only, so resolve them through
  # pkg-config with an explicit PKG_CONFIG_PATH instead of the default search.
  brew = find_program('brew')
  pkgconfig = find_program('pkg-config')
  libbsd_prefix  = run_command(brew, '--prefix', 'libbsd', check : true).stdout().strip()
  ncurses_prefix = run_command(brew, '--prefix', 'ncurses', check : true).stdout().strip()
  pkg_env = ['PKG_CONFIG_PATH=' + libbsd_prefix + '/lib/pkgconfig:' + ncurses_prefix + '/lib/pkgconfig']
  bsdlib_dep = declare_dependency(
    compile_args : run_command(pkgconfig, '--cflags', 'libbsd-overlay', env : pkg_env, check : true).stdout().split(),
    link_args    : run_command(pkgconfig, '--libs',   'libbsd-overlay', env : pkg_env, check : true).stdout().split(),
  )
  ncurses_dep = declare_dependency(
    compile_args : run_command(pkgconfig, '--cflags', 'ncurses', env : pkg_env, check : true).stdout().split(),
    link_args    : run_command(pkgconfig, '--libs',   'ncurses', env : pkg_env, check : true).stdout().split(),
  )
else
  bsdlib_dep  = dependency('libbsd-overlay')
  ncurses_dep = dependency('ncurses')
endif

add_global_arguments('-DREGEX', language : 'c')
if host_system == 'darwin'
  # openpty/login_tty/fparseln come from the system <util.h>; no libutil to link.
  add_global_arguments('-DHAVE_UTIL_H', language : 'c')
  add_global_arguments('-D_DARWIN_C_SOURCE', language : 'c')
else
  add_global_arguments('-D_GNU_SOURCE', language : 'c')
  add_global_arguments('-DHAVE_PTY_H', language : 'c')
endif
add_global_arguments('-Wno-strict-aliasing', language : 'c')
add_global_arguments('-Wno-deprecated-declarations', language : 'c')

executable(
  'mg', 'cinfo.c', 'echo.c', 'line.c', 'ttyio.c', 'log.c',
  'autoexec.c', 'bell.c', 'dir.c', 'dired.c', 'file.c',
  'fileio.c', 'grep.c', 'help.c', 'kbd.c', 'macro.c', 're_search.c',
  'region.c', 'search.c', 'shell.c', 'spawn.c', 'term.c', 'tty.c', 'ttykbd.c',
  'undo.c', 'version.c', 'window.c', 'word.c', 'yank.c', 'main.c',
  'basic.c', 'buffer.c', 'cmode.c', 'display.c', 'extend.c', 'hexmode.c', 'makemode.c',
  'funmap.c', 'interpreter.c', 'keymap.c', 'llm.c', 'match.c', 'modes.c',
  'paragraph.c', 'util.c',
  install: true,
  dependencies: [bsdlib_dep, ncurses_dep],
)

install_man('mg.1')
