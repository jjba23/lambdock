#+title: Configuring lambdock
#+author: Josep Jesus Bigorra Algaba
#+email: jjbigorra@gmail.com
#+language: en
#+options: toc:nil num:nil html-style:nil
#+HTML_HEAD: <link rel="stylesheet" type="text/css" href="https://jointhefreeworld.org/static/css/jointhefreeworld.css" />

[[file:../README.org][← lambdock]]

* Configuring lambdock
:PROPERTIES:
:CUSTOM_ID: configuring-lambdock
:END:

The configuration is a Scheme program.  The dock evaluates it at start and again
whenever the file changes, and adopts the values it defines: a setting is a
binding, and a reload is a re-evaluation.  Style overrides are GTK4 CSS.

~lambdock~ will scan your configuration directory -- the one the XDG base
directory specification
([[https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html][freedesktop.org]])
puts at ~$XDG_CONFIG_HOME/lambdock/~, usually =~/.config/lambdock/= -- for
~settings*.scm~ files, effectively supporting multi-dock multi-config.

Tip: It might be useful sometimes to clean the Guile cache, specially when
upgrading to newer ~lambdock~ versions. You can do this with
=rm -rf ~/.cache/guile=

- *Single Configuration File*: =~/.config/lambdock/settings.scm=
- *Multi-Instance Configuration Files*: =~/.config/lambdock/settings-1.scm=,
  =~/.config/lambdock/settings-2.scm=, etc.
- *Styles and overrides*: ~$XDG_CONFIG_HOME/lambdock/styles.css~ (usually
  =~/.config/lambdock/styles.css=).
- *Hot-Reloading*: Save your files, and ~lambdock~ automatically updates existing
  docks or spawns/destroys dock instances as files are created or deleted.
- *Sensible Defaults:* Every option comes pre-configured -- you only need to
  override and export the settings you wish to customize.
- *Presets* that can help you reduce boilerplate, see
  [[file:../scheme/lambdock/presets.scm]] (launchers) and
  [[file:../scheme/lambdock/looks.scm]] (whole-dock look recipes, see [[file:theming.org#looks][Looks]]).

** A settings file, top to bottom
:PROPERTIES:
:CUSTOM_ID: a-settings-file
:END:

#+begin_src scheme
;; Enable/disable smooth edge sliding auto-hide
(define dock-auto-hide? #t)

;; Show dock when last window was closed
(define dock-show-when-clear? #t)

;; Dock edge attachment: 'bottom | 'top | 'left | 'right
(define dock-position 'bottom)

;; Default icon pixel resolution
(define dock-icon-size 48)

;; Themes:
;; 'vanilla | 'vanilla-light | 'frost | 'frost-dark
;; | 'solarized-light | 'nature | 'ambiance
;; | 'brushed | 'dune | 'stage | 'glass
;; | 'glass-dark | 'platinum | 'platinum-dark
;; | 'luna | 'aurora | 'seven | 'tiles | 'island
;; | 'none
(define dock-theme 'vanilla)

;; Or ask for a whole look: a theme plus the icon size, spacing, end padding,
;; mode and every shelf number in one line ('brushed, 'frost,
;; 'platinum-dark, 'glass, 'aurora, ...). Anything you define
;; yourself still wins over the recipe. See the "Looks" section.
;; (define dock-look 'platinum)

;; 'all         -> Render dock on every connected monitor
;; "DP-1"        -> Target specific monitor output connector
;; 'default     -> Default compositor monitor assignment
(define dock-monitor 'all)

;; List of app ids to never show in the dock
(define dock-exclude-apps '())

;; Whether to enable the lambdock REPL
(define dock-enable-repl? #f)

;; Location for the REPL unix socket
(define dock-repl-socket-path
  "/tmp/lambdock-repl.sock")

;; Whether to animate dock items when hovering on them with the mouse
(define dock-hover-animate? #t)

;; Whether to keep track of open apps and windows and show indicators and popovers
(define dock-track-windows? #t)

;; Layout mode: 'dock (floating capsule) | 'taskbar (full-width bar)
(define dock-mode 'dock)

;; Show the focused window's title next to running app icons (taskbar-style)
(define dock-show-window-titles? #f)

;; The dock's glass shelf is described by plain numbers. Everything below has
;; a sensible default (the flat capsule), so write only what you want to move.
(define dock-shelf-tilt 0)       ; tilt of the shelf plane, degrees (0 = flat)
(define dock-shelf-margin 24)    ; glass on the inner side of the icons, px
(define dock-shelf-front 0)      ; glass on the outer side (front face), px
(define dock-reflection 0)       ; reflection band / icon size (0 = off)
;; (define dock-shelf-perspective 400)   ; projection distance for the tilt
;; (define dock-reflection-intensity 0.55)
;; (define dock-reflection-blur 0.05)

;; Application launchers list
(define dock-items
  (list (preset-launcher 'alacritty)
        (preset-launcher 'google-chrome-flatpak)
        (app-item #:name "Spotify"
                  #:exec "flatpak run com.spotify.Client"
                  #:icon "com.spotify.Client")
        (app-item #:name "Files"
                  #:exec "nautilus -w"
                  #:icon (preset-icon 'nautilus))
        (separator-item)
        (app-item #:name "Emacs"
                  #:exec "emacsclient -c"
                  #:icon "emacs")))
#+end_src

** Configuration options reference
:PROPERTIES:
:CUSTOM_ID: configuration-options
:END:

Every option below is a top-level definition in your =settings*.scm=.  Writing
~dock-foo~ or just ~foo~ both work; the defaults are what you get when you say
nothing at all.

| Option                        | Default                      | What it does                                                                   |
|-------------------------------+------------------------------+--------------------------------------------------------------------------------|
| ~dock-position~               | ~'bottom~                    | Screen edge to attach to: ~'bottom~, ~'top~, ~'left~, ~'right~.                 |
| ~dock-icon-size~              | ~48~                         | Icon pixel size.                                                               |
| ~dock-item-spacing~           | ~4~                          | Gap between dock items in px (0 … 64). The theme's padding adds to it.          |
| ~dock-capsule-padding~        | ~12~                         | Room between the capsule's ends and the icons, in px (0 … 96). The theme's own button padding adds to it. |
| ~dock-items~                  | ~'()~                        | The item list -- launchers, separators and dynamic widgets, in order.          |
| ~dock-theme~                  | ~'vanilla~                   | Built-in theme symbol, or ~'none~ for bare GTK.                                 |
| ~dock-look~                   | unset                        | Whole-dock recipe: theme, sizes, spacing, mode and shelf geometry in one name (see [[file:theming.org#looks][Looks]]). Anything you define wins over it. |
| ~dock-monitor~                | ~'default~                   | ~'all~, ~'default~, or an output connector name such as ~"DP-1"~.               |
| ~dock-mode~                   | ~'dock~                      | ~'dock~ (floating capsule) or ~'taskbar~ (full-width bar).                      |
| ~dock-shelf-tilt~             | ~0~                          | Tilt of the shelf plane in degrees. 0 keeps the classic flat capsule.           |
| ~dock-shelf-perspective~      | ~400~                        | Projection distance in px for the tilt (smaller = stronger 3D).                 |
| ~dock-shelf-margin~           | ~24~                         | Glass on the inner side of the icons, in px. Negative lets them overhang.       |
| ~dock-shelf-front~            | ~0~                          | Glass on the outer (screen edge) side: the front face, in px.                   |
| ~dock-reflection~             | ~0~                          | Reflection band height / icon size. 0 disables the reflections.                 |
| ~dock-reflection-intensity~   | ~0.55~                       | Alpha of the reflection where it touches the icon.                              |
| ~dock-reflection-blur~        | ~0.05~                       | Gaussian blur radius / icon size for the reflections.                           |
| ~dock-shelf-flare~            | ~0~                          | Keystone: px the slab is wider per side at the screen edge (-80 … 80).          |
| ~dock-launchpad-icon-size~    | ~80~                         | Icon pixel size inside the launchpad grid (32 … 160).                          |
| ~dock-launchpad-columns~      | ~0~                          | Launchpad tiles per row; 0 fits the screen, 3 … 14 is an upper bound.          |
| ~dock-launchpad-blur?~        | ~#f~                         | Frosted, translucent launchpad backdrop (see [[file:using-the-dock.org#launchpad][The launchpad]]).                  |
| ~dock-launchpad-close-side~   | ~'left~                      | Launchpad close button: ~'left~ or ~'right~ end of its header.                  |
| ~dock-reduce-motion?~         | ~#f~                         | Turn off the slide and fish-eye animations (see [[file:using-the-dock.org#accessibility][accessibility]]).             |
| ~dock-auto-hide?~             | ~#f~                         | Slide the dock off the edge when unused.                                       |
| ~dock-show-when-clear?~       | ~#t~                         | Keep the dock visible when no window is open.                                  |
| ~dock-show-window-titles?~    | ~#f~                         | Show the focused window title next to running items (taskbar style).            |
| ~dock-hover-animate?~         | ~#t~                         | Fish-eye magnification on hover.                                               |
| ~dock-track-windows?~         | ~#t~                         | Track open apps/windows for indicators, focus and popovers.                     |
| ~dock-exclude-apps~           | ~'()~                        | App IDs never to show in the dock (e.g. ~'("warp-taskbar")~).                   |
| ~dock-enable-repl?~           | ~#f~                         | Serve a Unix socket REPL for live inspection and mutation.                      |
| ~dock-repl-socket-path~       | ~"/tmp/lambdock-repl.sock"~  | Where that REPL listens.                                                        |

The same names are also the keyword arguments of the programmatic
~(configure-dock! ...)~ procedure, which is what the loader calls after reading
your file and what you can call yourself from the REPL:

#+begin_src scheme
(configure-dock!
 #:position 'right
 #:theme 'island
 #:icon-size 40
 #:mode 'taskbar
 #:items (preset-launchers-for 'alacritty 'emacs)
 #:auto-hide? #t
 #:enable-repl? #t
 #:repl-socket-path "/tmp/lambdock-repl.sock"
 ;; Shelf parameters (defaults shown): flat capsule
 #:shelf-tilt 0
 #:shelf-perspective 400
 #:shelf-margin 24
 #:shelf-front 0
 #:reflection 0
 #:reflection-intensity 0.55
 #:reflection-blur 0.05
 ;; Launchpad
 #:launchpad-icon-size 64
 #:launchpad-columns 0
 #:launchpad-blur? #f
 #:launchpad-close-side 'left
 ;; Accessibility
 #:reduce-motion? #f)
#+end_src

** The dock DSL: items and presets
:PROPERTIES:
:CUSTOM_ID: the-dock-dsl
:END:

~lambdock~ provides a small Lisp DSL for defining your dock, based on items and
presets.

| Scheme Constructor / Procedure  | Return Type | Description                                        | Keyword Arguments                                                                                     |
|---------------------------------+-------------+----------------------------------------------------+-------------------------------------------------------------------------------------------------------|
| ~(app-item ...)~                | Record      | Custom application launcher definition              | ~#:name~, ~#:icon~, ~#:exec~, ~#:hover-animate?~                                                       |
| ~(dynamic-item ...)~            | Record      | Dynamic polling widget: text, or a gauge with ~#:meter?~ | ~#:name~, ~#:icon~, ~#:exec~, ~#:poll-fn~, ~#:poll-interval~, ~#:hover-animate?~, ~#:meter?~, ~#:unit~, ~#:max~ |
| ~(separator-item)~              | Record      | Layout divider line                                 | None                                                                                                  |
| ~(stack-item ...)~              | Record      | Folder stack: opens a directory in a popover        | ~#:name~, ~#:path~, ~#:icon~, ~#:view-mode~ (~'grid~, ~'list~), ~#:hover-animate?~                    |
| ~(launchpad-item ...)~          | Record      | Opens the full-screen application grid              | ~#:name~, ~#:icon~, ~#:hover-animate?~                                                                 |
| ~(trash-item ...)~              | Record      | Desktop trash icon: shows, opens and empties it     | ~#:name~, ~#:icon~, ~#:hover-animate?~                                                                 |
| ~(preset-launcher 'symbol)~     | Record      | Standard launcher resolved from the preset list      | Symbol (e.g. ~'emacs~, ~'alacritty~)                                                                    |
| ~(preset-launchers-for 'a 'b)~  | List        | Batch helper returning a list of preset launchers    | Variadic list of symbols, or one list of symbols                                                       |
| ~(preset-icon 'symbol)~         | String      | Resolves the default icon name for a given preset    | Symbol                                                                                                |

*** Dynamic widgets
:PROPERTIES:
:CUSTOM_ID: dynamic-widgets
:END:

Dynamic items poll a Scheme thunk on the GTK main thread. They come in three
shapes, and the built-in presets pick the interesting one for you:

- *text items* print the string the thunk returns (the battery);
- *gauges* draw the number the thunk returns as a ring that fills up inside the
  icon slot -- the CPU, RAM and temperature presets -- with the colour going
  green → amber → red as the value climbs, and the reading written in the middle
  ("42%", "54°C"). At a glance across the room a ring size is far easier to read
  than a string, and the dock stays a row of icons instead of a row of text;
- *the clock* draws an analogue face: a squircle body with a brushed bezel, a
  paper dial, tick marks and three hands, all drawn from geometry so it stays
  crisp at any ~dock-icon-size~. The hands follow the system clock and are set on
  every poll, while the string the thunk returns stays the item's tooltip and the
  text a screen reader announces.

| Preset             | Polls                                    | Default interval | Shape    |
|--------------------+------------------------------------------+------------------+----------|
| ~'clock~           | ~get-clock-text~ (day, date, time)       | 1 s              | face     |
| ~'ram~             | ~get-used-ram-value~ (from /proc)        | 2 s              | gauge    |
| ~'cpu~             | ~get-cpu-value~ (delta between polls)    | 2 s              | gauge    |
| ~'cpu-temp~        | ~get-cpu-temp-value~ (thermal zone)      | 3 s              | gauge    |
| ~'battery~         | ~get-battery-status~ (capacity, charging) | 5 s             | text     |

Each preset also has the text form of the same reading (~get-used-ram-percent~,
~get-cpu-percent~, ~get-cpu-temp~) and takes ~#:meter? #f~ to use it:

#+begin_src scheme
(list (ram-item)              ; a ring that fills with the used RAM
      (cpu-item)              ; a ring for the CPU load
      (cpu-temp-item)         ; a ring filling from 0 to 100 °C
      (cpu-item #:meter? #f)) ; ... and the old "CPU 42%" text item
#+end_src

Writing a gauge by hand is the same thunk plus three keywords: ~#:meter?~ makes
the item a gauge, ~#:max~ is the value that fills the ring and ~#:unit~ is
appended to the number. The thunk must return a *number* (or ~#f~ for "no
reading"):

#+begin_src scheme
(define (get-swap-used)
  (catch #t
         (lambda ()
           (let* ((line (call-with-input-file "/proc/swaps" read-line))
                  (rest (call-with-input-file "/proc/swaps"
                          (lambda (port)
                            (read-line port)
                            (read-line port)))))
             (if rest (string->number (cadr (string-split rest #\space))) #f)))
         (lambda _ #f)))

(define dock-items
  (list (dynamic-item #:name "Swap"
                      #:exec "alacritty -e 'btop'"
                      #:poll-fn get-swap-used
                      #:poll-interval 5000
                      #:meter? #t
                      #:unit "%"
                      #:max 4096)))       ; 4 GiB of swap fills the ring
#+end_src

The ring takes its text colour from the theme, so it reads correctly on the
light platinum slab and on dark themes alike, and it is sized from
~dock-icon-size~ like every other icon.

Writing your own is one thunk.  Because the poller runs on the main thread, it
must not block -- keep it to a cheap read, never a sleep:

#+begin_src scheme
;; A widget that shows how many days remain in the year
(define (get-days-left)
  (let* ((now (localtime (current-time)))
         (yday (tm:yday now))
         (year (tm:year now))
         (leap? (and (= 0 (modulo year 4))
                     (or (not (= 0 (modulo year 100)))
                         (= 0 (modulo year 400)))))
         (total (if leap? 366 365)))
    (format #f "~a days left" (- total yday))))

(define dock-items
  (list (dynamic-item #:name "Year"
                      #:icon "x-office-calendar"
                      #:exec "gnome-calendar"
                      #:poll-fn get-days-left
                      #:poll-interval 60000
                      #:hover-animate? #f)))
#+end_src

*** The preset zoo
:PROPERTIES:
:CUSTOM_ID: the-preset-zoo
:END:

~preset-launchers-for~ understands symbols for a growing list of common apps,
both native and Flatpak.  The Flatpak variants are suffixed ~-flatpak~ and run
through ~flatpak run ...~.

- *Browsers*: ~google-chrome~, ~firefox~, ~chromium~, ~brave~, ~nyxt~,
  ~zen-browser~, ~librewolf~
- *Terminals*: ~alacritty~, ~kitty~, ~foot~, ~wezterm~, ~gnome-terminal~,
  ~ghostty~
- *Editors & IDEs*: ~emacs~, ~vscode~, ~intellij~, ~webstorm~, ~datagrip~,
  ~bruno~, ~zed~, ~neovim~
- *Utilities*: ~nautilus~, ~gnome-calendar~, ~gnome-calculator~, ~pavucontrol~,
  ~btop~, ~onepassword~, ~thunderbird~, ~bitwarden~
- *Media & messaging*: ~spotify~, ~zapzap~, ~discord~, ~slack~, ~telegram~,
  ~gimp~, ~inkscape~, ~obs~, ~vlc~, ~steam~, ~blender~
- *Widgets*: ~ram~, ~cpu~, ~cpu-temp~, ~battery~, ~clock~, ~trash~

Anything missing is still one ~app-item~ away -- or one more entry in
[[file:../scheme/lambdock/presets.scm][presets.scm]], if you feel like giving it
back to everyone.

*** Building your item list procedurally
:PROPERTIES:
:CUSTOM_ID: building-your-item-list
:END:

Here is an example of separators being used along with presets.

#+begin_src scheme
;; Alternatively, construct dock-items procedurally using list operations:
(define dock-items
  (append (preset-launchers-for 'alacritty
                                'google-chrome-flatpak
                                'spotify
                                'nautilus
                                'nyxt)
          (list (separator-item))
          (preset-launchers-for 'emacs
                                'intellij-flatpak
                                'bruno)
          (list (separator-item))
          (preset-launchers-for 'btop
                                'pavucontrol
                                'onepassword)))
#+end_src

Here a snapshot of my own (@jjba23) configuration:

#+begin_src scheme
(define dock-auto-hide? #t)

(define dock-show-when-clear? #t)

(define dock-position 'bottom)

(define dock-icon-size 48)

(define dock-theme 'vanilla-light)

(define dock-monitor 'all)

(define dock-exclude-apps '("warp-taskbar"))

(define dock-enable-repl? #t)

(define dock-hover-animate? #t)

(define dock-track-windows? #t)

(define dock-items
  (append
   (preset-launchers-for
    'alacritty
    'google-chrome
    'spotify-flatpak
    'nautilus
    'nyxt-flatpak
    'gnome-calendar
    'gnome-calculator
    'zapzap-flatpak)
   (list (separator-item))
   (preset-launchers-for 'emacs 'intellij 'bruno-flatpak)
   (list (separator-item))
   (preset-launchers-for 'btop 'pavucontrol 'onepassword)
   (list (separator-item))
   (preset-launchers-for 'ram 'cpu 'cpu-temp 'battery)))
#+end_src

** Your config is a real program
:PROPERTIES:
:CUSTOM_ID: your-config-is-a-real-program
:END:

Since lambdock configuration files are evaluated directly as standard GNU Guile
Scheme, you can run *any arbitrary Guile code* ‼️✨ right inside any
=~/.config/lambdock/settings*.scm= file.  You can import modules, define helper
functions, query system state (like time, hostnames, or installed packages), do
API calls, and dynamically build your configuration.

#+begin_src scheme
(use-modules (ice-9 popen)
             (ice-9 rdelim)
             (srfi srfi-19)
             (srfi srfi-1))

;; Helper to run a shell command and return its output as a clean string
(define (sh-output cmd)
  (let* ((port (open-input-pipe cmd))
         (output (read-line port)))
    (close-pipe port)
    (if (eof-object? output) "" output)))

;; Enable auto-hide only on a specific hostname (e.g., laptop setup)
(define dock-auto-hide?
  (string=? (string-trim-both (sh-output "hostname")) "thinkpad"))

;; Pick the dock edge from the current hour: a sunny-side-up desk by day...
(define dock-position
  (if (< (date-hour (current-date)) 12) 'top 'bottom))
#+end_src

That flexibility is the point: a dock that reads your machine, instead of a dock
that only reads a file.

** Multi-config & multi-dock instances
:PROPERTIES:
:CUSTOM_ID: multi-config-and-multi-dock
:END:

~lambdock~ features built-in multi-dock orchestration. Rather than being limited
to a single dock bar, you can run multiple independent docks simultaneously
across different screen edges or monitors.

- *Automatic Directory Monitoring*: ~lambdock~ watches =~/.config/lambdock/= for
  files matching ~settings.scm~ or ~settings-*.scm~ (e.g., ~settings-left.scm~,
  ~settings-bottom.scm~).
- *Isolated State*: Each configuration file defines its own isolated
  ~LambdockState~, position (~dock-position~), theme (~dock-theme~), monitor
  targets (~dock-monitor~), and item launcher layout (~dock-items~).
- *Dynamic Spawning*: Creating a new ~settings-2.scm~ file instantly spawns a new
  dock bar on screen.
- *Dynamic Hot-Reloading*: Editing any ~settings-*.scm~ file hot-reloads that
  specific dock instance without flickering or restarting other running
  instances.
- *Dynamic Destruction*: Deleting a ~settings-*.scm~ file safely tears down and
  destroys its corresponding dock window, removing Wayland handles and GTK
  widgets without crashing the application.
- *Monitor Hotplug*: Docks survive monitors coming and going; layer-shell is
  unbound from an output before it is disconnected rather than after.

Also, here is a small demo of hot-reload:

** Changing settings from the GUI (Preferences) 🎛️
:PROPERTIES:
:CUSTOM_ID: preferences-window
:END:

Right-click the dock and choose *Preferences…* to open a GTK4 settings window.
It covers the settings people reach for most often, grouped the way the settings
file groups them:

- *Behaviour*: auto-hide, show when no windows, hover magnification, window
  tracking, window titles, and dock vs taskbar mode.
- *Dock items*: *Add an item* -- a trash can, a clock, or a RAM, CPU, CPU
  temperature or battery meter -- appended to the dock in one click.
- *Accessibility*: reduce motion (see [[file:using-the-dock.org#accessibility][Keyboard and accessibility]]).
- *Layout & material*: icon size, item spacing, capsule padding, screen edge and
  theme.
- *Launchpad*: tile icon size, columns, frosted background, and which end of the
  launchpad's header the close button sits on (see [[file:using-the-dock.org#launchpad][The launchpad]]).
- *Glass shelf*: icon overhang, front glass, reflection height/intensity/blur,
  keystone flare, tilt and perspective.

*Add an item* is worth a note: it appends one of the dock's ready-made widgets to
the *running* dock and remembers it in the dock's layout override -- the same
file drag and drop writes -- so it survives a restart without touching
~settings.scm~.  It is a move, not a mode: the item is a normal dock item from
then on, so it can be dragged along the dock, and a right-click gives *Remove
from Dock*.  Adding the same widget twice keeps one of it.

Every control applies immediately through the same ~set-dock-*!~ setters the
[[file:repl.org#repl][REPL]] uses, so the dock redraws as you drag a slider (the apply is
debounced so a drag is one edit, not one per pixel).  Each slider also has a spin
button beside it for typing an exact value -- a 53 px icon, a 0.07 blur -- and the
window closes with *Escape*.

*Your settings file is never modified.* GUI changes are written to a separate
override file, next to the dock's drag-and-drop layout overrides:

#+begin_src sh
${XDG_STATE_HOME:-~/.local/state}/lambdock/preferences-<settings-stem>.scm
#+end_src

The loader layers that file on top of ~settings.scm~, which is what lets a
generated settings file (an OS image, a dotfiles generator) be regenerated
without losing the choices made in the window. Deleting the override file -- or
pressing *Reset* in the window -- restores the settings file exactly.

When several docks run (see [[#multi-config-and-multi-dock][multi-config]]), the
menu item carries the settings path of the dock it was opened on, so the window
edits that dock and not another one.
