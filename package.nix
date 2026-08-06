{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  glib,
  wrapGAppsHook4,
  gtk4,
  gtk4-layer-shell,
  guile,
  wayland,
  wayland-protocols,
  wlr-protocols,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "lambdock";
  version = "0.3.6";

  src = ./.;

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
    glib # provides glib-compile-resources
    wrapGAppsHook4
  ];

  buildInputs = [
    gtk4
    gtk4-layer-shell
    guile
    wayland
    wayland-protocols
    wlr-protocols
  ];

  meta = with lib; {
    description = "Wayland desktop dock customizable with GNU Guile Scheme";
    homepage = "https://codeberg.org/jjba23/lambdock";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
    mainProgram = "lambdock";
  };
})
