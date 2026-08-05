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

project('lambdock', 'c',
  version: '0.3.0',
  meson_version: '>= 0.62.0',
  default_options: [
    'c_std=gnu17',            # Modern standard + POSIX/GNU extensions required by GTK/Wayland
    'warning_level=3',        # -Wall -Wextra -Wpedantic
    'werror=true',            # Treat warnings as errors for project code
    'b_lto=true',              # Link-Time Optimization (LTO)
    'b_pie=true',              # Position-Independent Executable
  ]
)

gnome = import('gnome')
cc = meson.get_compiler('c')

# --- Pedantic & Strict Compiler Warnings for Project Code ---
desired_warning_flags = [
  '-Wpedantic',
  '-Wshadow',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wredundant-decls',
  '-Wimplicit-fallthrough',
  '-Wvla',                    # Disallow Variable-Length Arrays
  '-Wundef',
  '-Wformat=2',
  '-Wformat-security',
  '-Werror=implicit-function-declaration',
  '-Werror=incompatible-pointer-types',
  '-Werror=int-conversion',
  '-Wno-overlength-strings',
]

add_project_arguments(
  cc.get_supported_arguments(desired_warning_flags),
  language: 'c'
)

# Required POSIX feature macro under strict C modes
add_project_arguments('-D_GNU_SOURCE', language: 'c')

# --- Dependencies ---
gtk_dep = dependency('gtk4', include_type: 'system')
guile_dep = dependency('guile-3.0', include_type: 'system')
layer_shell_dep = dependency('gtk4-layer-shell-0', include_type: 'system')
wayland_client_dep = dependency('wayland-client', include_type: 'system')

# --- Wayland Code Generation ---
wlr_protocols_dep = dependency('wlr-protocols', required: false)

if wlr_protocols_dep.found()
  wlr_pkgdatadir = wlr_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')
  toplevel_xml = wlr_pkgdatadir / 'unstable/wlr-foreign-toplevel-management-unstable-v1.xml'
else
  toplevel_xml = files('protocols/wlr-foreign-toplevel-management-unstable-v1.xml')
endif

wayland_scanner = find_program('wayland-scanner')

wl_proto_src = custom_target(
  'wlr-foreign-toplevel-protocol-c',
  input: toplevel_xml,
  output: 'wlr-foreign-toplevel-management-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

wl_proto_h = custom_target(
  'wlr-foreign-toplevel-protocol-h',
  input: toplevel_xml,
  output: 'wlr-foreign-toplevel-management-unstable-v1-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

# --- Compile GLib Resources ---
lambdock_resources = gnome.compile_resources(
  'lambdock-resources',
  'resources/lambdock.gresource.xml',
  source_dir: 'resources',
  c_name: 'lambdock',
)

# --- Path Configuration & Preprocessor Defines ---
datadir = get_option('prefix') / get_option('datadir')
scheme_install_path = datadir / 'guile/site/3.0'

add_project_arguments(
  '-DLAMBDOCK_SCHEME_DIR="' + scheme_install_path + '"',
  '-DDEFAULT_GSK_RENDERER="gl"',
  '-DPACKAGE_VERSION="' + meson.project_version() + '"',
  language: 'c'
)

# --- Scheme Module Installation ---
install_subdir('scheme/lambdock', install_dir: scheme_install_path)

inc = include_directories('src')

sources = files(
  'src/main.c',
  'src/ui_helpers.c',
  'src/guile_bridge.c',
  'src/dock_window.c',
  'src/toplevel_tracker.c',
)

executable('lambdock',
  sources,
  lambdock_resources,
  wl_proto_src,
  wl_proto_h,
  include_directories: inc,
  dependencies: [
    gtk_dep,
    guile_dep,
    layer_shell_dep,
    wayland_client_dep,
  ],
  gnu_symbol_visibility: 'hidden',
  install: true
)
