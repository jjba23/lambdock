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

{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  glib,
  wrapGAppsHook4,
  xvfb-run,
  dbus,
  fontconfig,
  hicolor-icon-theme,
  adwaita-icon-theme,
  gtk4,
  gtk4-layer-shell,
  guile,
  gettext,
  wayland,
  wayland-protocols,
  wlr-protocols,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "lambdock";
  version = "0.13.2";

  strictDeps = true;
  __structuredAttrs = true;

  src = ./.;

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
    glib
    wrapGAppsHook4
    gettext
  ];

  buildInputs = [
    gtk4
    gtk4-layer-shell
    guile
    wayland
    wayland-protocols
    wlr-protocols
  ];

  doCheck = true;

  nativeCheckInputs = [
    xvfb-run
    dbus
    fontconfig
    hicolor-icon-theme
    adwaita-icon-theme
  ];

  checkPhase = ''
    runHook preCheck

    # GTK / X11 test environment configuration
    export HOME=$TMPDIR
    export XDG_RUNTIME_DIR=$TMPDIR
    export FONTCONFIG_FILE=${fontconfig.out}/etc/fonts/fonts.conf
    # The dock-item tests resolve `utilities-terminal'; give them the themes.
    export XDG_DATA_DIRS=${hicolor-icon-theme}/share:${adwaita-icon-theme}/share

    # Use GTK's test accessibility backend -- there is no a11y bus here, and the
    # accessible-name assertions need an AT context -- and force X11 under Xvfb
    export GTK_A11Y=test
    export GDK_BACKEND=x11
    # No EGL under Xvfb: use the software renderer for the GTK window tests.
    export GSK_RENDERER=cairo

    # Start DBus session bus
    dbus-daemon --config-file=${dbus}/share/dbus-1/session.conf --fork --print-address 5 5>dbus_address
    export DBUS_SESSION_BUS_ADDRESS=$(cat dbus_address)

    # Run tests on virtual display
    xvfb-run -s "-screen 0 1024x768x24" meson test --print-errorlogs

    runHook postCheck
  '';

  meta = with lib; {
    description = "Wayland desktop dock customizable with GNU Guile Scheme";
    homepage = "https://codeberg.org/jjba23/lambdock";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
    mainProgram = "lambdock";
  };
})
