# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>

# ------------------------------------------------------------------------------
# Project Initialization & Build Options
# ------------------------------------------------------------------------------
# Defines the project name and primary language ('c').
# Sets target version metadata and compiler flags used system-wide.
project('lambdock', 'c',
  version: '0.21.0',
  meson_version: '>= 0.62.0',
  default_options: [
    'c_std=gnu17',            # Target standard C17 with GNU extensions
    'warning_level=3',        # Turn on strict compiler warnings (enables -Wall, -Wextra, -Wpedantic)
    'werror=true',            # Treat compiler warnings as fatal errors
    'b_lto=true',             # Link-Time Optimization (LTO) for smaller, faster binaries
    'b_pie=true',             # Position-Independent Executable (PIE) for security/ASLR
    'buildtype=release',      # Optimized and stripped binaries
    'strip=true'
  ]
)

# Import the GNOME module which has many utilities
# like compiling GLib resource bundles
gnome = import('gnome')

# Get an object representing the underlying C compiler
cc = meson.get_compiler('c')


# ------------------------------------------------------------------------------
# Strict Compiler Warning Flags
# ------------------------------------------------------------------------------
# Extra pedantic C compiler flags designed to catch bugs early in development.
desired_warning_flags = [
  '-Wpedantic',               # Strictly enforce ISO C standards compliance
  '-Wshadow',                 # Warn when a local variable shadows another variable in an outer scope
  '-Wstrict-prototypes',      # Warn if functions aren't declared with explicit parameter types
  '-Wmissing-prototypes',     # Warn if global functions are defined without a prior declaration
  '-Wredundant-decls',        # Warn if something is declared twice in the same scope
  '-Wimplicit-fallthrough',   # Warn when a switch case falls through without explicit intent
  '-Wvla',                    # Disallow Variable-Length Arrays (VLAs) to avoid stack security issues
  '-Wundef',                  # Warn if an uninitialized macro name is used in an #if expression
  '-Wformat=2',               # Enable strict printf/scanf format string security checks
  '-Wformat-security',        # Warn about potential format string security vulnerabilities
  '-Werror=implicit-function-declaration', # Fail build if a function is used without declaration
  '-Werror=incompatible-pointer-types',     # Fail build on mismatched pointer type assignments
  '-Werror=int-conversion',                 # Fail build on implicit pointer-to-integer conversions
  '-Wno-overlength-strings',  # Suppress warnings if string constants exceed basic C limits
]

# `cc.get_supported_arguments` filters the list above so that only flags supported
# by the user's specific compiler version are passed to the compile command.
add_project_arguments(
  cc.get_supported_arguments(desired_warning_flags),
  language: 'c'
)

# Expose GNU extensions in standard system C headers (like strdup, asprintf, etc.)
add_project_arguments('-D_GNU_SOURCE', language: 'c')


# ------------------------------------------------------------------------------
# Third-Party Library Dependencies
# ------------------------------------------------------------------------------
# Finds libraries installed on the system using pkg-config.
# `include_type: 'system'` tells the compiler to treat headers as system files
# so warnings inside external library code won't trigger `-Werror`.

gtk_dep = dependency('gtk4', include_type: 'system')               # GTK4 GUI toolkit framework
guile_dep = dependency('guile-3.0', include_type: 'system')         # GNU Guile 3.0 Scheme interpreter
layer_shell_dep = dependency('gtk4-layer-shell-0', include_type: 'system') # GTK4 Wayland desktop layer integration
wayland_client_dep = dependency('wayland-client', include_type: 'system')   # Core Wayland client C API

# libm: the dock uses round(), cos() and fabs() for the fisheye and the shelf
# geometry. Some toolchains resolve those through another library's dependencies
# and some do not, so link it explicitly instead of relying on luck.
libm_dep = cc.find_library('m', required: false)

# Shared dependency array for the main executable and multi-dependency tests
lambdock_deps = [
    gtk_dep,
    guile_dep,
    layer_shell_dep,
    wayland_client_dep,
    libm_dep,
]

# ------------------------------------------------------------------------------
# Wayland Code Generation (XML Protocol Scanner)
# ------------------------------------------------------------------------------
# Wayland protocol extensions are defined as XML files. We run `wayland-scanner`
# to turn those XML protocol definitions into usable C source (.c) and header (.h) files.

# Check if `wlr-protocols` package is installed on the build machine
wlr_protocols_dep = dependency('wlr-protocols', required: false)

# If system package exists, use its path; otherwise, fall back to bundled XML in `protocols/`
if wlr_protocols_dep.found()
  wlr_pkgdatadir = wlr_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')
  toplevel_xml = wlr_pkgdatadir / 'unstable/wlr-foreign-toplevel-management-unstable-v1.xml'
else
  toplevel_xml = files('protocols/wlr-foreign-toplevel-management-unstable-v1.xml')
endif

# Locate the `wayland-scanner` binary tool on the host system
wayland_scanner = find_program('wayland-scanner')

# Rule: Compile XML into C source code (.c) for foreign window management tracking
wl_proto_src = custom_target(
  'wlr-foreign-toplevel-protocol-c',
  input: toplevel_xml,
  output: 'wlr-foreign-toplevel-management-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# Rule: Compile XML into C header (.h) for foreign window management tracking
wl_proto_h = custom_target(
  'wlr-foreign-toplevel-protocol-h',
  input: toplevel_xml,
  output: 'wlr-foreign-toplevel-management-unstable-v1-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

wl_proto_sources = [wl_proto_src, wl_proto_h]


# ------------------------------------------------------------------------------
# GLib Resource Compilation
# ------------------------------------------------------------------------------
# Packages static UI resources (XML layouts, CSS, icons) directly into C code,
# embedding them straight into the final binary.
lambdock_resources = gnome.compile_resources(
  'lambdock-resources',
  'resources/lambdock.gresource.xml',
  source_dir: 'resources',
  c_name: 'lambdock',
)


# ------------------------------------------------------------------------------
# Installation Paths & C Preprocessor Macros
# ------------------------------------------------------------------------------
# Construct the path where Scheme files will be installed (e.g. `/usr/share/guile/site/3.0`)
datadir = get_option('prefix') / get_option('datadir')
scheme_install_path = datadir / 'guile/site/3.0'

# Pass dynamic build parameters directly into the C code via preprocessor definitions (-D)
add_project_arguments(
  '-DLAMBDOCK_SCHEME_DIR="' + scheme_install_path + '"', # Location of embedded scheme scripts
  '-DDEFAULT_GSK_RENDERER="gl"',                       # Default rendering engine for GTK
  '-DPACKAGE_VERSION="' + meson.project_version() + '"',# Pass project version string into C
  language: 'c'
)


# ------------------------------------------------------------------------------
# Scheme (Lisp) Code Installation
# ------------------------------------------------------------------------------
# Instructs `meson install` to copy local Scheme source code into the Guile system module path
install_subdir('scheme/lambdock', install_dir: scheme_install_path)


# ------------------------------------------------------------------------------
# Desktop Integration (.desktop entry, AppStream metadata & application icon)
# ------------------------------------------------------------------------------
# Reverse-DNS application identifier, kept in sync with the GtkApplication ID
# used in src/main.c and with the Flatpak manifest.
lambdock_app_id = 'org.jointhefreeworld.lambdock'

# Install a freedesktop.org compliant desktop entry so lambdock shows up in
# application launchers, Rofi/dmenu, GNOME Shell search, and similar tools.
lambdock_desktop_file = files('data' / lambdock_app_id + '.desktop')
install_data(
  lambdock_desktop_file,
  install_dir: get_option('datadir') / 'applications',
)

# Install AppStream metadata so software centers (GNOME Software), Flathub and
# other catalogues can present lambdock with its description and screenshots.
lambdock_metainfo_file = files('data' / lambdock_app_id + '.metainfo.xml')
install_data(
  lambdock_metainfo_file,
  install_dir: get_option('datadir') / 'metainfo',
)

# Install the application icon into the hicolor icon theme under lambdock's
# reverse-DNS application ID, so `Icon=org.jointhefreeworld.lambdock` resolves
# in launchers and window/desktop menus.
install_data(
  'resources/img/agnus-dei.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps',
  rename: lambdock_app_id + '.svg',
)

# Validate the installed desktop entry when `desktop-file-validate` is present.
desktop_file_validate = find_program('desktop-file-validate', required: false)
if desktop_file_validate.found()
  test('desktop_entry', desktop_file_validate, args: lambdock_desktop_file)
else
  message('desktop-file-validate not found: skipping desktop entry validation')
endif

# Validate the AppStream metadata when `appstreamcli` is present. Network
# access is disabled so the check stays hermetic inside build sandboxes.
appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('appstream_metainfo', appstreamcli,
       args: ['validate', '--no-net', lambdock_metainfo_file])
else
  message('appstreamcli not found: skipping AppStream metadata validation')
endif


# ------------------------------------------------------------------------------
# i18n, localization and internationalization
# ------------------------------------------------------------------------------

i18n = import('i18n')

# Define and pass the locale directory to the C compiler
localedir = join_paths(get_option('prefix'), get_option('localedir'))
add_project_arguments('-DLOCALEDIR="' + localedir + '"', language: 'c')

# Include the po directory for translations
subdir('po')

# ------------------------------------------------------------------------------
# Source Code Definitions & Executable Build
# ------------------------------------------------------------------------------
# Specify inclusion directory for headers (`#include "something.h"` looking inside `src/`)
inc = include_directories('src')

# List of all C source code files needed to build the project
sources = files(
  'src/main.c',
  'src/accessibility.c',
  'src/app_window.c',
  'src/core/config_values.c',
  'src/core/dock_click.c',
  'src/core/dock_dims.c',
  'src/core/dock_motion.c',
  'src/core/item_text.c',
  'src/core/render_stats.c',
  'src/core/tracker_rules.c',
  'src/dock_registry.c',
  'src/dock_window.c',
  'src/guile_bridge.c',
  'src/toplevel_tracker.c',
  'src/ui/theme_engine.c',
  'src/ui/widget_utils.c',
  'src/ui/icon_resolver.c',
  'src/ui/icon_zoom.c',
  'src/ui/popover_menu.c',
  'src/ui/dock_item.c',
  'src/ui/dock_item_windows.c',
  'src/ui/dock_dnd.c',
  'src/ui/app_actions.c',
  'src/ui/desktop_entry.c',
  'src/ui/stack.c',
  'src/ui/launchpad.c',
  'src/ui/meter.c',
  'src/ui/clock.c',
  'src/core/clock_face.c',
  'src/ui/shelf.c',
  'src/ui/shelf_follow.c',
  'src/ui/preferences.c',
  'src/ui/trash.c',
)

# Compiles and links the final binary application executable
executable('lambdock',
  sources,                  # Application C source files
  lambdock_resources,       # Embedded GLib resources (.gresource C file)
  wl_proto_src,             # Generated Wayland C source code
  wl_proto_h,               # Generated Wayland C header code
  include_directories: inc, # Include path (`-I src`)
  dependencies: lambdock_deps,
  gnu_symbol_visibility: 'hidden', # Keep non-exported internal functions hidden from the executable's symbol table
  install: true                    # Instructs `meson install` to copy binary to binary path (e.g. `/usr/bin/lambdock`)
)

# ------------------------------------------------------------------------------
# Test Suite Configuration
# ------------------------------------------------------------------------------
test_env = environment()
# The accessibility tests read labels back with `gtk_test_accessible_*()', which
# needs a GtkATContext. GTK's "test" backend provides one without an
# accessibility bus, so those assertions run on headless CI instead of aborting
# when the environment turns accessibility off with `GTK_A11Y=none'.
test_env.set('GTK_A11Y', 'test')

test_stubs = files('tests/test_stubs.c')

# Shared non-main sources for tests requiring full application context
full_app_sources = files(
  'src/accessibility.c',
  'src/app_window.c',
  'src/core/config_values.c',
  'src/core/dock_click.c',
  'src/core/dock_dims.c',
  'src/core/dock_motion.c',
  'src/core/item_text.c',
  'src/core/render_stats.c',
  'src/core/tracker_rules.c',
  'src/dock_registry.c',
  'src/dock_window.c',
  'src/guile_bridge.c',
  'src/toplevel_tracker.c',
  'src/ui/theme_engine.c',
  'src/ui/widget_utils.c',
  'src/ui/icon_resolver.c',
  'src/ui/icon_zoom.c',
  'src/ui/popover_menu.c',
  'src/ui/dock_item.c',
  'src/ui/dock_item_windows.c',
  'src/ui/dock_dnd.c',
  'src/ui/app_actions.c',
  'src/ui/desktop_entry.c',
  'src/ui/stack.c',
  'src/ui/launchpad.c',
  'src/ui/meter.c',
  'src/ui/clock.c',
  'src/core/clock_face.c',
  'src/ui/shelf.c',
  'src/ui/shelf_follow.c',
  'src/ui/preferences.c',
  'src/ui/trash.c',
)
full_test_sources = [test_stubs, full_app_sources, wl_proto_sources, lambdock_resources]

unit_tests = [
  {
    'id': 'theme_engine',
    'extra_sources': [test_stubs, 'src/ui/theme_engine.c', 'src/ui/shelf.c',
                      'src/ui/icon_zoom.c', lambdock_resources],
    'deps': [gtk_dep, guile_dep, layer_shell_dep, libm_dep],
  },
  {
    'id': 'popover_menu',
    'extra_sources': full_test_sources,
  },
  {
    'id': 'dock_item',
    'extra_sources': full_test_sources,
  },
  {
    'id': 'icon_resolver',
    'extra_sources': ['src/ui/icon_resolver.c', lambdock_resources],
    'deps': [gtk_dep, libm_dep],
  },
  {
    'id': 'icon_zoom',
    'extra_sources': ['src/ui/icon_zoom.c'],
    'deps': [gtk_dep, libm_dep],
  },
  {
    'id': 'dock_window',
    'extra_sources': full_test_sources,
  },
  {
    'id': 'shelf',
    'extra_sources': full_test_sources,
  },
  {
    'id': 'desktop_entry',
    'extra_sources': ['src/ui/desktop_entry.c'],
    'deps': [dependency('gio-2.0'), dependency('glib-2.0')],
  },
  {
    'id': 'config_values',
    'extra_sources': ['src/core/config_values.c', 'src/ui/shelf.c',
                      'src/ui/icon_zoom.c', lambdock_resources],
  },
  {
    'id': 'dock_registry',
    'extra_sources': ['src/dock_registry.c'],
  },
  {
    # Deliberately no GTK and no Wayland here: the tracker's decisions are
    # booleans in, booleans out, and linking only this file proves it.
    'id': 'tracker_rules',
    'extra_sources': ['src/core/tracker_rules.c'],
    'deps': [dependency('glib-2.0')],
  },
  {
    'id': 'dock_dims',
    'extra_sources': ['src/core/dock_dims.c', 'src/ui/shelf.c',
                      'src/ui/icon_zoom.c', lambdock_resources],
  },
  {
    # Click policy: counts and flags in, one enum out. No GTK, no Wayland.
    'id': 'dock_click',
    'extra_sources': ['src/core/dock_click.c'],
    'deps': [dependency('glib-2.0')],
  },
  {
    # Deliberately no GTK here: dock_motion.c is pure arithmetic, and the test
    # binary proves it by linking nothing else.
    'id': 'dock_motion',
    'extra_sources': ['src/core/dock_motion.c'],
    'deps': [dependency('glib-2.0'), libm_dep],
  },
  {
    # Same deal for the item wording: strings in, strings out, no widgets.
    'id': 'item_text',
    'extra_sources': ['src/core/item_text.c'],
    'deps': [dependency('glib-2.0'), libm_dep],
  },
  {
    'id': 'stack',
    'extra_sources': full_test_sources,
  },
  {
    'id': 'accessibility',
    'extra_sources': full_test_sources,
  },
  {
    'id': 'launchpad',
    'extra_sources': full_test_sources,
  },
  {
    'id': 'dock_dnd',
    'extra_sources': full_test_sources,
  },
  {
    # Clock face geometry: hand positions and the squircle, as arithmetic. No
    # GTK here either -- the widget only paints what this returns.
    'id': 'clock_face',
    'extra_sources': ['src/core/clock_face.c'],
    'deps': [dependency('glib-2.0'), libm_dep],
  },
  {
    'id': 'meter',
    'extra_sources': ['src/ui/meter.c', 'src/core/config_values.c',
                      'src/ui/icon_zoom.c', lambdock_resources],
    'deps': [gtk_dep, guile_dep, layer_shell_dep, libm_dep],
  },
  {
    'id': 'guile_bridge',
    'name': 'guile_bridge_integration_tests',
    'extra_sources': full_test_sources,
  },
  {
    # The real Scheme modules behind the real C bridge: proves that a settings
    # file plus the two override files in $XDG_STATE_HOME are honoured on the
    # first load (test_guile_bridge mocks Scheme and cannot see this).
    'id': 'config_startup',
    'extra_sources': full_test_sources,
  },
  {
    # The per-frame budget, measured: a hover sweep must not resize the surface,
    # must reuse the geometry cache, and one reflection repaint must stay within
    # its band budget.
    'id': 'render_budget',
    'name': 'render_budget_tests',
    'extra_sources': full_test_sources,
  },
  {
    'id': 'preferences',
    'extra_sources': full_test_sources,
  },
  {
    'id': 'trash',
    'extra_sources': ['src/ui/trash.c'],
    'deps': [dependency('gio-2.0'), dependency('glib-2.0')],
  },
  {
    'id': 'app_actions',
    'extra_sources': ['src/ui/app_actions.c', 'src/ui/desktop_entry.c'],
    'deps': [dependency('gio-2.0'), dependency('glib-2.0')],
  },
]

foreach t : unit_tests
  test_id = t['id']
  test_name = t.get('name', test_id + '_unit_tests')
  test_deps = t.get('deps', lambdock_deps)

  exe = executable(
    'test_' + test_id,
    'tests/test_' + test_id + '.c',
    t['extra_sources'],
    include_directories: inc,
    dependencies: test_deps,
  )
  # Run the C tests from the source root: the Guile bridge resolves its
  # `scheme` load path relative to the working directory, so this makes the
  # tests exercise the modules in this tree instead of a possibly stale copy
  # installed under the system Guile site directory.
  test(test_name, exe, env: test_env, workdir: meson.project_source_root())
endforeach

# ------------------------------------------------------------------------------
# Pure-Scheme (Guile) Test Suite
# ------------------------------------------------------------------------------
# Exercises the embedded configuration engine (state/items/presets/loader/core)
# directly under Guile -- no display server or C runtime required. The driver
# script locates a Guile interpreter and exits 77 when none is available, which
# Meson reports as "skipped".
guile_prog = find_program('guile', required: false)
scheme_test_driver = find_program('tests/run_scheme_tests.sh', required: false)

if guile_prog.found() and scheme_test_driver.found()
  test('scheme_unit_tests', scheme_test_driver, env: test_env)
else
  message('Guile interpreter not found: skipping pure-Scheme unit tests')
endif
