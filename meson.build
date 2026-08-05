# lambdock

# Copyright © Josep Bigorra <jjbigorra@gmail.com>

# lambdock is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# lambdock is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with lambdock.  If not, see <https://www.gnu.org/licenses/>.

# ------------------------------------------------------------------------------
# Project Initialization & Build Options
# ------------------------------------------------------------------------------
# Defines the project name and primary language ('c').
# Sets target version metadata and compiler flags used system-wide.
project('lambdock', 'c',
  version: '0.3.4',
  meson_version: '>= 0.62.0',
  default_options: [
    'c_std=gnu17',            # Target standard C17 with GNU extensions
    'warning_level=3',        # Turn on strict compiler warnings (enables -Wall, -Wextra, -Wpedantic)
    'werror=true',            # Treat compiler warnings as fatal errors
    'b_lto=true',             # Link-Time Optimization (LTO) for smaller, faster binaries
    'b_pie=true',             # Position-Independent Executable (PIE) for security/ASLR
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
# Source Code Definitions & Executable Build
# ------------------------------------------------------------------------------
# Specify inclusion directory for headers (`#include "something.h"` looking inside `src/`)
inc = include_directories('src')

# List of all C source code files needed to build the project
sources = files(
  'src/main.c',
  'src/dock_window.c',
  'src/guile_bridge.c',
  'src/toplevel_tracker.c',
  'src/ui/theme_engine.c',
  'src/ui/icon_resolver.c',
  'src/ui/popover_menu.c',
  'src/ui/dock_item.c',
)

# Compiles and links the final binary application executable
executable('lambdock',
  sources,                  # Application C source files
  lambdock_resources,       # Embedded GLib resources (.gresource C file)
  wl_proto_src,             # Generated Wayland C source code
  wl_proto_h,               # Generated Wayland C header code
  include_directories: inc, # Include path (`-I src`)
  dependencies: [           # External system dependencies linked via pkg-config
    gtk_dep,
    guile_dep,
    layer_shell_dep,
    wayland_client_dep,
  ],
  gnu_symbol_visibility: 'hidden', # Keep non-exported internal functions hidden from the executable's symbol table
  install: true                    # Instructs `meson install` to copy binary to binary path (e.g. `/usr/bin/lambdock`)
)
