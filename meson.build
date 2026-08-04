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
  version: '0.1.0',
  meson_version: '>= 0.62.0',
  default_options: [
    'c_std=gnu17',            # Modern standard + POSIX/GNU extensions required by GTK/Wayland
    'warning_level=3',        # -Wall -Wextra -Wpedantic
    'werror=true',            # Treat warnings as errors
    'b_lto=true',             # Link-Time Optimization (LTO)
    'b_pie=true',             # Position-Independent Executable (security hardening)
  ]
)

cc = meson.get_compiler('c')
wayland = import('wayland')  # Built-in Meson module for Wayland code generation

# --- Pedantic & Strict Compiler Warnings ---
desired_warning_flags = [
  '-Wpedantic',
  '-Wshadow',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wredundant-decls',
  '-Wimplicit-fallthrough',
  '-Wvla',                    # Disallow Variable-Length Arrays (prevents stack overhead & safety issues)
  '-Wundef',
  '-Wformat=2',
  '-Wformat-security',
  '-Werror=implicit-function-declaration',
  '-Werror=incompatible-pointer-types',
  '-Werror=int-conversion',
]

add_project_arguments(
  cc.get_supported_arguments(desired_warning_flags),
  language: 'c'
)

# GNU source definitions required when building under high strictness modes with POSIX APIs
add_project_arguments('-D_GNU_SOURCE', language: 'c')

# --- Dependencies ---
gtk_dep = dependency('gtk4', version: '>= 4.0')
guile_dep = dependency('guile-3.0', version: '>= 3.0')
layer_shell_dep = dependency('gtk4-layer-shell-0')
wayland_client_dep = dependency('wayland-client', version: '>= 1.20')

# Protocol location and Wayland code generation
wlr_protocols_dep = dependency('wlr-protocols')
wlr_pkgdatadir = wlr_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')
toplevel_xml = wlr_pkgdatadir / 'unstable/wlr-foreign-toplevel-management-unstable-v1.xml'

wl_proto_src = wayland.scan_xml(
  toplevel_xml,
  client: true,
  public: false
)

# --- Path Configuration & Preprocessor Defines ---
datadir = get_option('prefix') / get_option('datadir')
pkgdatadir = datadir / 'lambdock'
scheme_install_path = datadir / 'guile/site/3.0'

add_project_arguments(
  '-DLAMBDOCK_SCHEME_DIR="' + scheme_install_path + '"',
  '-DDOCK_CSS_PATH="' + (pkgdatadir / 'css') + '"',
  '-DDEFAULT_GSK_RENDERER="gl"',
  '-DPACKAGE_VERSION="' + meson.project_version() + '"',
  language: 'c'
)

# --- Directory Installations ---
install_subdir('scheme/lambdock', install_dir: scheme_install_path)
install_subdir('resources/css', install_dir: pkgdatadir)

sources = files(
  'src/main.c',
  'src/ui_helpers.c',
  'src/guile_bridge.c',
  'src/dock_window.c',
  'src/toplevel_tracker.c',
)

executable('lambdock',
  sources,
  wl_proto_src,
  dependencies: [
    gtk_dep,
    guile_dep,
    layer_shell_dep,
    wayland_client_dep,
  ],
  gnu_symbol_visibility: 'hidden',  # Hides dynamic symbols to reduce executable size and speed up calls
  install: true
)
