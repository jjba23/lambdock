# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>

{
  description = "lambdock - Wayland desktop dock customizable with GNU Guile Scheme";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          lambdock = pkgs.callPackage ./package.nix { };
          default = self.packages.${system}.lambdock;
        });

      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.lambdock ];
            packages = with pkgs; [
              ccls
              watchexec
              xvfb-run
              # What the Guix shell gets from the `tools` and `rust-src` outputs
              # of its pinned rust: the lints, the formatter, and the editor's
              # language server with a sysroot it can read.
              clippy
              rustfmt
              rust-analyzer
              # The standard library's sources, which is what makes the editor
              # show std's signatures and docs instead of guesses. Guix hands
              # them to its `rust-analyzer' wrapper; here they are a package of
              # their own.
              rustPlatform.rustLibSrc
              gdb
            ];
            # Guix wraps its `rust-analyzer' to export this; the Nix one is
            # stock, so the shell says where the sources are itself. The package
            # is the library directory: `core', `alloc', `std' and their
            # Cargo.toml.
            shellHook = ''
              export RUST_SRC_PATH=${pkgs.rustPlatform.rustLibSrc}
            '';
          };
        });
    };
}
