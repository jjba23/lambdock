;; lambdock -- hyper-hackable desktop dock for Wayland
;;
;; SPDX-License-Identifier: MIT
;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

(define-module (maak)
  #:declarative? #t
  #:use-module (maak maak)
  ;; The container helpers are imported under a prefix because the
  ;; tasks below are named the way maak exposes them on the command
  ;; line -- `maak container-build' -- and a task may not shadow the
  ;; helper it calls.
  #:use-module ((maak container) #:prefix ct:)
  ;; The Nix helpers are imported the same way: two of the tasks below are
  ;; named `nix-build' and `nix-run'.
  #:use-module ((maak nix) #:prefix nx:)
  ;; The task named `run' shadows the shell builtin of that name for the rest
  ;; of this file, so every command here is started through `dsl:run' -- the
  ;; builtin under the name of the module it comes from -- instead.
  #:use-module ((maak dsl) #:prefix dsl:))

;; ===== Maak tasks =======

(define project-name
  'lambdock)

(define (run)
  "Run lambdock with Guix"
  ;; Nothing of the right name running is not a failure, so the command is
  ;; allowed to say so instead of the task dying on `pkill''s non-zero exit.
  (dsl:run (list "pkill" "lambdock") #:allow-failure? #t)
  (time-machine-program-shell (list "lambdock")
                              #:verbose? #t))

(define (test)
  "Build and run the whole test suite headlessly.

The shell wrapper only applies to the first command, so the pipeline is run by
one inner `sh -c' instead of a bare `&&': otherwise `ninja' would execute
outside the Guix environment.

The GTK tests need a display: with none, `gtk_init_check' fails and GLib turns
the resulting GTK warnings into aborts (6 of 7 tests die with SIGTRAP), so the
suite runs on a virtual X server, as in package.nix's checkPhase.`GDK_BACKEND', `GTK_A11Y' and `GSK_RENDERER' are pinned because there is no compositor, no a11y
bus and no EGL under Xvfb; `GTK_A11Y=test' is GTK's own backend for the
accessible-name assertions in the suite.

The build directory is thrown away first: the Byggsteg job clone survives
between jobs, and a tree configured by a different Meson version makes both
`meson setup' and `ninja' fail with 'build data file ... references functions
or classes that don't exist'. Rebuilding from scratch also means the passing
tests are the ones that match the checked out sources."
  (time-machine-manifest-shell
   (list "/bin/sh" "-c"
         ;; One token, because it is one shell script: the quoting that keeps
         ;; `-screen 0 1024x768x24' one argument to xvfb-run is part of the
         ;; text the inner shell reads.
         (~ "'~a'"
            (string-join
             (list "set -eu"
                   "export GTK_A11Y=test GDK_BACKEND=x11 GSK_RENDERER=cairo"
                   "rm -rf build"
                   "meson setup build"
                   "ninja -C build"
                   "xvfb-run -a -s \"-screen 0 1024x768x24\" meson test -C build --print-errorlogs")
             "; ")))
   #:verbose? #t))

(define (repl)
  "Development REPL."
  (time-machine-manifest-shell
   (list "guile" "-L" "./src"
         "-c" "'((@ (ares server) run-nrepl-server))'")))

(define c-packages
  (make-parameter (string-join (map symbol->string
                                    '(gtk4 guile-3.0 gtk4-layer-shell-0
                                           wlr-protocols)) " ")))

(define (add-libs)
  (delete-file-recursively ".ccls")
  ;; A pipe and an append-redirection, so this is one of the lines that needs a
  ;; shell.  Every part is a word of it, and `tr''s second set is the `\n'
  ;; escape the shell hands over as a newline character.
  (time-machine-manifest-shell
   (append (list "pkg-config" "--cflags")
           (string-split (c-packages) #\space)
           (list "|" "tr" "' '" "'\n'" ">>" ".ccls"))
   #:verbose? #t)
  (cat ".ccls"))

(define deb-packages
  (make-parameter (map symbol->string
                       '(build-essential debhelper
                                         devscripts
                                         cme
                                         lintian
                                         meson
                                         ninja-build
                                         pkg-config
                                         gettext
                                         libgtk-4-dev
                                         libgtk4-layer-shell-dev
                                         guile-3.0-dev
                                         libwayland-dev
                                         libglib2.0-dev-bin
                                         wayland-protocols))))

(define (deb-build)
  "Build .deb package (unsigned)"
  (dsl:run (list "apt" "update")
           #:sudo? #t
           #:verbose? #t)
  (dsl:run (append (list "apt" "install" "-y")
                   (deb-packages))
           #:sudo? #t
           #:verbose? #t)

  (dynamic-wind
   ;; Setup: create temporary symlink at project root
   (lambda ()
     (ln-s "packaging/debian" "debian"
           #:force? #f
           #:verbose? #t))

   ;; Execution: run debian build commands against ./debian
   (lambda ()
     (dsl:run (list "mk-build-deps" "-i" "-r" "debian/control")
              #:sudo? #t
              #:verbose? #t)
     (dsl:run (list "debuild" "-b" "-uc" "-us")
              #:verbose? #t))

   ;; Cleanup: remove symlink safely (leaves packaging/debian intact)
   (lambda ()
     (rm-rf "debian"
            #:verbose? #t))))

(define (arch-build)
  "Build Arch Linux package (.pkg.tar.zst)"
  (dsl:run (list "makepkg" "-si")
           #:verbose? #t)
  (dsl:run (list "namcap" "packaging/arch/PKGBUILD")
           #:verbose? #t))

(define rpm-packages
  (make-parameter (map symbol->string
                       '(rpm-build rpmdevtools
                                   gcc
                                   meson
                                   ninja-build
                                   pkgconfig
                                   gettext
                                   gtk4-devel
                                   gtk4-layer-shell-devel
                                   guile30-devel
                                   libwayland-client-devel))))

(define (rpm-build)
  "Build RPM package for openSUSE and Fedora/RHEL"
  (let* ((latest-tag (git-describe-tag))
         ;; `git describe' prints nothing in a dry run, and a version is not
         ;; derived from nothing: the tag is kept as it came when it carries no
         ;; leading `v'.
         (latest-tag-num (cond ((string=? latest-tag "") "")
                               ((string-prefix? "v" latest-tag)
                                (string-drop latest-tag 1))
                               (else latest-tag)))
         (sources (~ "~a/rpmbuild/SOURCES" (home-dir)))
         (specs (~ "~a/rpmbuild/SPECS" (home-dir)))
         (rpms (~ "~a/rpmbuild/RPMS/x86_64" (home-dir)))
         (srpms (~ "~a/rpmbuild/SRPMS" (home-dir))))
    (dsl:run (list "rpmdev-setuptree")
             #:verbose? #t)
    (git-archive-tar (~ "~a/lambdock-~a.tar.gz" sources latest-tag-num)
                     #:prefix (~ "lambdock-~a/" latest-tag-num)
                     #:revision latest-tag)
    (cp "packaging/opensuse/lambdock.spec" specs
        #:verbose? #t)
    (dsl:run (list "rpmbuild" "-ba" (~ "~a/lambdock.spec" specs))
             #:verbose? #t)
    (dsl:run (list "ls" (~ "~a/lambdock-[0-9]*.rpm" rpms))
             #:expand? #t)
    (let* ((latest-rpm
            (string-trim-both
             (capture (~ "ls -t ~a/lambdock-[0-9]*.rpm | head -n 1" rpms))))
           (latest-src-rpm
            (string-trim-both
             (capture (~ "ls -t ~a/lambdock-[0-9]*.rpm | head -n 1" srpms)))))
      (cp "packaging/opensuse/lambdock-rpmlintrc" rpms
          #:verbose? #t)
      (dsl:run (list "rpmlint" latest-rpm)
               #:verbose? #t)
      (cp "packaging/opensuse/lambdock-rpmlintrc" srpms
          #:verbose? #t)
      (dsl:run (list "rpmlint" latest-src-rpm)
               #:verbose? #t))))

(define (nix-test)
  "Run Nix flake checks and test phase"
  (nx:nix-flake-check #:flake "."
                      #:verbose? #t))

(define (nix-build)
  "Build Nix package"
  (nx:nix-flake-build #:flake "."))

(define (nix-run)
  "Run Nix package"
  (nx:nix-run '()))

(define (default)
  "List the tasks this project defines."
  (list-tasks))

(define (container-build)
  "Build Container image"
  (ct:container-build "." #:tag "lambdock:latest" #:verbose? #t))

(define (container-run)
  "Run Container image"
  ;; `$(id -u)' and `$WAYLAND_DISPLAY' were the shell's way of naming the
  ;; session's runtime directory and socket; they are asked for in Scheme here,
  ;; so the whole command line is arguments rather than words a shell splits.
  ;; The flags without a `container-run' keyword -- `--userns', `--ipc' and
  ;; `--device' -- are what the escape hatch is for.
  (let* ((user-id (number->string (getuid)))
         (runtime-dir (or (getenv "XDG_RUNTIME_DIR")
                          (~ "/run/user/~a" user-id)))
         (wayland-display (or (getenv "WAYLAND_DISPLAY") ""))
         (home (home-dir))
         (socket (~ "~a/~a" runtime-dir wayland-display)))
    (ct:container
     (list "run" "--detach" "--rm"
           "--name" "lambdock"
           "--network" "host"
           "--userns=keep-id"
           "--ipc=host"
           "--device" "/dev/dri"
           "--env" (~ "XDG_RUNTIME_DIR=~a" runtime-dir)
           "--env" (~ "WAYLAND_DISPLAY=~a" wayland-display)
           "--env" (~ "HOME=~a" home)
           "--volume" (~ "/run/user/~a:/run/user/~a:z" user-id user-id)
           "--volume" (~ "~a:/run/user/~a/~a:ro" socket user-id
                         wayland-display)
           "--volume" (~ "~a:~a" home home)
           "--volume" "/usr/share/icons:/usr/share/icons:ro"
           "--volume" "/usr/share/applications:/usr/share/applications:ro"
           "lambdock:latest")
     #:verbose? #t)))

(define (push-docker-latest)
  "Push the latest locally generated Docker image to Dockerhub."
  (ct:container (list "push"
                      (~ "localhost/~a:latest" project-name)
                      (~ "docker.io/jjba23/~a:latest" project-name))
                #:verbose? #t))

(define (push-docker-tag)
  "Push the locally generated Docker image to Dockerhub."
  (git-fetch "--all")
  (let ((latest-tag (git-describe-tag)))
    (ct:container (list "push"
                        (~ "localhost/~a:latest" project-name)
                        (~ "docker.io/jjba23/~a:~a" project-name latest-tag))
                  #:verbose? #t)))

(define (i18n-update)
  "Update .pot and .po files based on the source code."
  ;; Three steps of the same build, each run where it happens rather than as
  ;; one `&&' chain inside a single shell: the one that fails is the one that
  ;; says so.
  (time-machine-manifest-shell (list "meson" "setup" "build"))
  (time-machine-manifest-shell (list "ninja" "-C" "build" "lambdock-pot"))
  (time-machine-manifest-shell (list "ninja" "-C" "build"
                                     "lambdock-update-po")))

(define (meson-build)
  "Meson build."
  ;;(delete-file-recursively "./build")
  (time-machine-program-shell (list "meson" "setup" "build"))
  (time-machine-program-shell (list "ninja" "-C" "build")))

(define (debug-run)
  "Debug run lambdock."
  (meson-build)
  (dsl:run (list "gdb" "--args" "./build/lambdock")))

(define (flatpak-build)
  "Build Flatpak package."
  (dsl:run (list "flatpak" "install" "--user" "flathub"
                 "org.gnome.Platform//50" "org.gnome.Sdk//50"))
  (dsl:run (list "flatpak-builder" "build-dir"
                 "packaging/flatpak/org.jointhefreeworld.lambdock.yaml"
                 "--force-clean")))
