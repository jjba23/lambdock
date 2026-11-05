;; lambdock -- hyper-hackable desktop dock for Wayland
;;
;; SPDX-License-Identifier: MIT
;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

(define-module (maak)
  #:declarative? #t
  #:use-module (maak maak)
  #:use-module ((ice-9 rdelim) #:select (read-line))
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

;; ====== Agent skills ======

;; `run' names a task in this file, so the shell helper is reached through the
;; module it comes from.

;; AGENTS.md is generated, never edited by hand.  The section has two halves:
;; the data below, which is this project's, and the task itself, which is not.
;; Another repository copies the whole section and replaces the data - the
;; header, the pinned skills (repository, commit, path, licence) and the
;; practices - and `maak agents' writes its own AGENTS.md.  Regeneration is
;; deterministic: the same pins and the same text produce the same file, byte
;; for byte, and it needs nothing but curl.  The skill URLs assume GitHub, so a
;; skill hosted elsewhere needs a URL of its own.

(define ponytail-commit
  "96608e078b2d5ea607c5c3b8b5a54d1c1fba6846")

(define tdd-commit
  "22f0b463e7a5e579904dd6211a520321035526f7")

;; Name, repository, commit, the path the repository keeps the skill at and the
;; licence.  A skill arrives whole: trimming it means scanning for headings, and
;; such a scan cannot tell a heading from a `##' line inside a code fence.
(define agent-skills
  (list (list "ponytail"
              "DietrichGebert/ponytail"
              ponytail-commit
              "skills/ponytail/SKILL.md"
              "MIT")
        (list "test-driven-development"
              "obra/superpowers"
              tdd-commit
              "skills/test-driven-development/SKILL.md"
              "MIT")))

(define agent-skills-dir
  "reports/agent-skills")

(define agents-header
  "# AGENTS.md

Generated by `maak agents` from the pinned skills and the practices in
`maak.scm`; edit that task, never this file.  The pins and the text are the whole
input, so regenerating gives the same document.  The repository's own rules win
over general advice, and the narrowest rule here wins over the broadest.
")

;; Practices this project keeps.  They are short on purpose: a rule a reader
;; checks in a second is a rule that gets followed.  The pinned skills are the
;; long form of the same habits.
(define agent-practices
  (list
   "## Documentation

- Every user-visible change reaches the documentation: the page that owns the
  feature, the manual page for a flag or an environment variable, and the
  release notes under the unreleased heading.
- Keep the table of contents in step with the headings.  A section with no entry
  is a section nobody finds.
- The guide is Org: one `*` heading per page, `**` for sections, `#+TITLE` and
  `#+EMAIL` at the top, a `CUSTOM_ID` on anything linked from elsewhere, and
  `file:` links between pages.
- Explain why and when, never what: the code already says what.  A docstring's
  first line works as a summary on its own.
- Spell an identifier the way the code spells it - the exact name, not a
  description of it.
- An example must run as written, copied from a real invocation.
- No promises about the future and no changelog in the guide; history lives in
  the release notes.
"
   "## Security

- No secrets in the tree.  Tokens, keys and passwords arrive through the
  environment, and no recipe, workflow or example embeds one.
- Validate at the edge: anything from a file, a report, a path or a subprocess
  is untrusted until it has been checked.
- Never build a shell string out of data.  Tasks pass argument lists, and Guile
  calls `(system \"program\" \"arg\")` or `open-pipe*`, never a command
  assembled with `string-append`.
- Do not `eval` or `read` input you did not write; parse with `(json)` or a
  reader built for the format.
- Escape what leaves the process: a name that came from input is escaped when
  it is written to XML, HTML or a data file.
- Fail closed.  A file that cannot be read is an error, not an empty result.
- Input never chooses the path of a written file; writes stay inside the
  directory reserved for them.
- A dependency is a security decision: it must be needed, MIT compatible and
  pinned.
- Report a vulnerability privately, not in a public issue.
"
   "## Performance

- Measure first.  A before and after belongs in the benchmark, and a speedup
  claimed without a number is not a speedup.
- Prefer not doing the work: cache it, skip it, batch it, or delete the call,
  before making the call faster.
- Then the algorithm, then the data structure (hash table, vector), then the
  micro-optimisation.  In that order, for that reason.
- Guile specifics: keep module resolution and long `assoc` chains out of loops,
  use `#:declarative?` modules and compiled `.go` files at startup, and reach
  for `statprof` instead of guessing where the time goes.
- A shortcut that buys speed costs clarity; it gets a measurement and a
  `shortcut:` comment saying what it costs and when to revisit it.
- Coverage instrumentation is slow by design: iterate without it, and measure
  before finishing.
"
   "## Architecture decisions

- A decision that is expensive to reverse gets a short record: the context, the
  decision, the consequences, and the alternatives that lost.  A small decision
  belongs in the commit body instead.
- Two paragraphs beat a design document; record the choice while it is still a
  choice, not after it has become a fact.
- An accepted record is not rewritten.  Supersede it with a new one and link the
  two directions.
- The record lives next to its subject: the documentation for anything a user
  would notice, the release notes for the release-level result.
- Changes to the public API, a file format, the pipeline or the licence get this
  treatment at a minimum in the commit body.
"
   "## Semantic versioning

- The project follows SemVer 2.0.0 for its public API: the exported functions,
  the CLI flags and environment variables, and the shape of its file formats.
- A breaking change bumps MAJOR, a new feature MINOR, a fix PATCH.  A new export
  is MINOR; removing or changing one is MAJOR.
- One version string, named everywhere it appears: the source, the build
  recipes, the manual page and the packaging.
- A release is a dated release-notes section, an annotated `vX.Y.Z` tag on the
  release commit, and a packaging checksum updated afterwards from the published
  archive.
"
   "## Conventional commits

- `type(scope): summary`, imperative, lower case, no full stop, about seventy
  characters.
- Types in use: `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `build`, `ci`,
  `chore`.  A `!` after the type or a `BREAKING CHANGE:` footer marks a MAJOR
  change, and says what breaks.
- The subject says what changed.  The body says why, and what was rejected.
- One logical change per commit: a refactor and a fix travel apart, so either
  can be reverted alone.
- Run the full validation task before committing, and never commit a red suite
  without saying so in the message.
- Reference the issue in the footer - `Closes #12` - not in the subject.
"
   "## Functional programming and immutability

- Pure functions in the core, effects at the edges.  Whatever touches the file
  system or the clock sits at the edge; the core does not.
- Prefer `let` and new values to `set!`.  Build results with `map`, `filter`,
  `fold` and named `let`, not with index loops that push into a list.
- A record is a value: construct a new one instead of mutating a shared one, and
  pass configuration as arguments or parameters rather than as globals.
- Handle an exception at the boundary: catch it there, turn it into a result
  record, and let the caller decide what it means.
- A function that both computes and prints is two functions.
- Temporary effects - a changed directory, a bound environment variable - go
  through `dynamic-wind`.
- Prefer the expression style: `if` and `cond` that return a value, not a branch
  that assigns one.
"
   "## Do not reinvent the wheel

- Guile's standard library first: `(ice-9 ...)` for the runtime, `(srfi srfi-1)`
  for lists, `(srfi srfi-9)` for records, `(srfi srfi-13)` for strings, `(json)`
  for JSON.  The project already depends on all of them.
- Then this codebase: read the existing modules before writing a helper, and use
  the one that is nearly right instead of adding a second one.
- A new dependency needs a reason the standard library cannot meet, a licence
  the project can carry, and a paragraph in the commit body.
- Prefer deleting a special case to writing code that handles it.
- The best diff removes lines and keeps the behaviour; the second best is a
  small diff that is obvious a year later.
"))

(define (slurp path)
  "Return the whole of @var{path} as a string."
  (call-with-input-file path
    (lambda (port)
      (let loop ((lines '())
                 (line (read-line port)))
        (if (eof-object? line)
            (string-concatenate-reverse lines)
            (loop (cons (string-append line "\n") lines)
                  (read-line port)))))))

(define (fetch-skill name repository commit path)
  "Download one skill pinned at @var{commit}, into the reports tree.

  Returns the skill's text."
  (let ((target (~ "~a/~a.md" agent-skills-dir name))
        (url (~ "https://raw.githubusercontent.com/~a/~a/~a"
                repository commit path)))
    ((@ (maak maak) run) (list "curl" "--fail" "--silent" "--show-error" "--location"
               "--output" target url)
         #:verbose? #t)
    (slurp target)))

(define (agents)
  "Regenerate AGENTS.md from the pinned skills and the practices."
  ((@ (maak maak) run) (list "mkdir" "-p" agent-skills-dir))
  ;; Every download happens before the file is opened, so a fetch that fails
  ;; leaves the previous AGENTS.md in place rather than a half-written one.
  (let ((skills (map (lambda (skill)
                       (append skill
                               (list (fetch-skill (list-ref skill 0)
                                                  (list-ref skill 1)
                                                  (list-ref skill 2)
                                                  (list-ref skill 3)))))
                     agent-skills)))
    (call-with-output-file "AGENTS.md"
      (lambda (port)
        (display agents-header port)
        (for-each (lambda (skill)
                    (format port
                            "~%## ~a~%~%`~a` at `~a`, `~a`, ~a licensed.~%~%"
                            (list-ref skill 0)
                            (list-ref skill 1)
                            (list-ref skill 2)
                            (list-ref skill 3)
                            (list-ref skill 4))
                    (display (list-ref skill 5) port))
                  skills)
        (for-each (lambda (practice)
                    (newline port)
                    (display practice port))
                  agent-practices)))))
