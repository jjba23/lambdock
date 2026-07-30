(use-modules (guix packages)
             (gnu)
             (guix utils)
             (guix git-download)
             (guix download)
             (gnu packages guile)
             (gnu packages nss)
             (gnu packages admin)
             (gnu packages web)
             (gnu packages gnupg)
             (gnu packages guile-xyz)
             (gnu packages pkg-config)
             (guix build-system guile)
             (gnu packages linux)
             (gnu packages compression)
             (gnu packages texinfo)
             (gnu packages autotools)
             (gnu packages build-tools)
             (gnu packages cpp)
             (gnu packages gtk)
             (gnu packages commencement)
             (gnu packages rust-apps)
             ((guix licenses)
              #:prefix license:))

(packages->manifest (list guile-3.0
                          gcc-toolchain
                          pkg-config
                          meson
                          ccls
                          gtk gtk4-layer-shell
                          texinfo
                          watchexec
                          guile-documenta
                          ninja
                          guile-ares-rs))
