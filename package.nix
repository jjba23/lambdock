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
  glibcLocales,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "lambdock";
  version = "2.3.1";

  strictDeps = true;
  __structuredAttrs = true;

  src = ./.;

  buildPhase = ''
    runHook preBuild

    # Compiled in, because the dock finds its Scheme modules and its catalogues
    # by path, not by a wrapper: without these an installed binary only works
    # when it is run from the source tree, where "./scheme" happens to resolve.
    export LAMBDOCK_SCHEME_DIR=$out/share/guile/site/3.0
    export LAMBDOCK_LOCALEDIR=$out/share/locale

    cargo build --offline --locked --release
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    cargo run --offline --locked -p xtask -- install --prefix $out
    runHook postInstall
  '';

  # The wrapper points GTK and GIO at this closure; the locales are a file of
  # their own. Without it the process locale is unsupported -- GTK says so once
  # -- and gettext falls back to the C locale, so the dock is English no matter
  # which catalogue the install put in $out/share/locale.
  #
  # It also has to point the loader at an EGL vendor. The closure ships none, and
  # a GTK that cannot create an EGL display gives up on hardware rendering: it
  # falls back to cairo, which has no projective transform, so the shelf's
  # keystone and tilt are drawn as a plain rectangle and every frame is composited
  # on the CPU (a fisheye hover on a 27-item dock then crawls). NixOS keeps its
  # driver in /run/opengl-driver and the other distributions keep theirs under
  # /usr/lib*, so the wrapper appends all of them: the loader ignores a directory
  # that is not there, and because these are appended the closure still wins for
  # every library it does ship.
  preFixup = ''
    gappsWrapperArgs+=(
      --set LOCALE_ARCHIVE ${glibcLocales}/lib/locale/locale-archive
      --suffix LD_LIBRARY_PATH : "/run/opengl-driver/lib:/usr/lib64:/usr/lib/x86_64-linux-gnu:/usr/lib"
    )
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
    glibcLocales
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

    cargo test --offline --locked

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
