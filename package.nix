# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>

{
  lib,
  stdenv,
  ninja,
  pkg-config,
  wayland-scanner,
  cargo,
  rustc,
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
  libx11,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "lambdock";
  version = "2.0.0";

  strictDeps = true;
  __structuredAttrs = true;

  src = ./.;

  buildPhase = ''
    runHook preBuild
    make build
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    make install PREFIX=$out
    runHook postInstall
  '';

  nativeBuildInputs = [
    pkg-config
    wayland-scanner
    # The crate, its suite, and the two generators `build.rs' runs.
    rustc
    cargo
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
    # The optional X11 backend: `gtk4-x11.pc' resolves it, and the build turns
    # the backend on when it finds it.
    libx11
  ];

  doCheck = true;

  nativeCheckInputs = [
    hicolor-icon-theme
    adwaita-icon-theme
  ];

  checkPhase = ''
    runHook preCheck

    # The suite is display-free: every test is a decision, so there is no Xvfb
    # and no accessibility bus to arrange.
    export HOME=$TMPDIR
    export XDG_RUNTIME_DIR=$TMPDIR
    export FONTCONFIG_FILE=${fontconfig.out}/etc/fonts/fonts.conf
    # The dock-item tests resolve `utilities-terminal'; give them the themes.
    export XDG_DATA_DIRS=${hicolor-icon-theme}/share:${adwaita-icon-theme}/share

    make test

    runHook postCheck
  '';

  meta = with lib; {
    description = "Wayland desktop dock customizable with GNU Guile Scheme";
    homepage = "https://github.com/jjba23/lambdock";
    license = licenses.mit;
    platforms = platforms.linux;
    mainProgram = "lambdock";
  };
})
