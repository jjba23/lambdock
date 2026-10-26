;;; lambdock
;;;
;;; Copyright © Josep Bigorra <jjbigorra@gmail.com>
;;;
;;; lambdock is free software : you can redistribute it and / or modify
;;; it under the terms of the GNU General Public License as published by
;;; the Free Software Foundation, either version 3 of the License, or
;;; (at your option) any later version.
;;;
;;; lambdock is distributed in the hope that it will be useful,
;;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
;;; GNU General Public License for more details.
;;;
;;; You should have received a copy of the GNU General Public License
;;; along with lambdock. If not, see <https://www.gnu.org/licenses/>.

;;; Pure Guile unit tests for the embedded Scheme engine. These run without a
;;; display server and without the C runtime: they load the real modules from
;;; scheme/lambdock and assert their behaviour directly. Exit code is 0 on
;;; success and 1 when any assertion fails (Meson reports 77 as "skipped").

(use-modules (ice-9 format)
             (ice-9 regex)
             (srfi srfi-1)
             (lambdock state)
             (lambdock items)
             (lambdock presets)
             (lambdock loader)
             (lambdock prefs)
             (lambdock core))

;; =============================================================================
;; Tiny TAP-ish assertion framework
;; =============================================================================

(define %total 0)
(define %failed 0)

(define (record name ok detail)
  (set! %total (+ %total 1))
  (if ok
      (format #t "ok ~a - ~a\n" %total name)
      (begin
        (set! %failed (+ %failed 1))
        (format (current-error-port)
                "not ok ~a - ~a :: ~a\n" %total name detail))))

(define (check name ok)
  (record name (and ok #t) (format #f "expected a truthy value, got ~s" ok)))

(define (check-not name ok)
  (record name (not ok) (format #f "expected #f, got ~s" ok)))

(define (check-equal name expected actual)
  (record name (equal? expected actual)
          (format #f "expected ~s but got ~s" expected actual)))

(define (check-string name text)
  (record name (and (string? text) (> (string-length text) 0))
          (format #f "expected a non-empty string, got ~s" text)))

(define (check-matches name pattern text)
  (record name (and (string? text) (string-match pattern text) #t)
          (format #f "expected a string matching ~s, got ~s" pattern text)))

(define (check-throws name thunk)
  (record name
          (catch #t (lambda () (thunk) #f) (lambda _ #t))
          "expected an exception that was not raised"))

;; =============================================================================
;; Helpers
;; =============================================================================

(define %tmp-root
  (let ((dir (format #f "/tmp/lambdock-scheme-tests-~a" (getpid))))
    (catch #t (lambda () (mkdir dir)) (lambda _ #f))
    dir))

(define (tmp-path name)
  (format #f "~a/~a" %tmp-root name))

(define (write-file path contents)
  (call-with-output-file path
    (lambda (port) (display contents port))))

(define (set-env name value)
  "Sets NAME to VALUE, or unsets it entirely when VALUE is #f."
  (if value (setenv name value) (unsetenv name)))

;; Dock layout overrides live under XDG_STATE_HOME. Point it at the throwaway
;; test root so the developer's real overrides can never leak into the tests.
(setenv "XDG_STATE_HOME" (tmp-path "state"))

(define (with-env bindings thunk)
  "Runs THUNK with an environment overlay, restoring the previous values."
  (let ((saved (map (lambda (b) (cons (car b) (getenv (car b)))) bindings)))
    (for-each (lambda (b) (set-env (car b) (cdr b))) bindings)
    (dynamic-wind
      (lambda () #t)
      thunk
      (lambda ()
        (for-each (lambda (s) (set-env (car s) (cdr s))) saved)))))

;; =============================================================================
;; (lambdock core) — init-dock! fallback configuration
;; =============================================================================
;; This MUST run first: it asserts the "no items registered yet" fallback,
;; which only holds while the global config table is still empty.

(setenv "HOME" %tmp-root)
(setenv "XDG_CONFIG_HOME" %tmp-root) ; no lambdock/settings.scm inside

(init-dock!)
(check "init-dock! registers a fallback config when items are empty"
       (not (null? (get-dock-items))))
(check-equal "fallback config uses bottom position"
             'bottom (get-dock-position))
(check-equal "fallback config icon size is 48"
             48 (get-dock-icon-size))
(check-equal "fallback config item spacing is the Mavericks-like 4px"
             4 (get-dock-item-spacing))
(check-equal "fallback config is not auto-hiding"
             #f (get-dock-auto-hide?))
(check-equal "fallback config mode is 'dock"
             'dock (get-dock-mode))
(check-equal "fallback config does not show window titles"
             #f (get-dock-show-window-titles?))

;; =============================================================================
;; (lambdock loader) — config path resolution
;; =============================================================================

(with-env `(("XDG_CONFIG_HOME" . "/xdg") ("HOME" . "/home/tester"))
  (lambda ()
    (check-equal "get-config-path honours XDG_CONFIG_HOME"
                 "/xdg/lambdock/settings.scm" (get-config-path))))

(with-env `(("XDG_CONFIG_HOME" . "") ("HOME" . "/home/tester"))
  (lambda ()
    (check-equal "get-config-path falls back to HOME"
                 "/home/tester/.config/lambdock/settings.scm"
                 (get-config-path))))

(with-env `(("XDG_CONFIG_HOME" . "") ("HOME" . #f))
  (lambda ()
    (check-not "get-config-path returns #f without HOME or XDG_CONFIG_HOME"
               (get-config-path))))

;; resolve-dock-configuration: missing files yield an empty alist
(check-equal "resolve-dock-configuration on a missing file yields '()"
             '() (resolve-dock-configuration (tmp-path "does-not-exist.scm")))
(check-equal "resolve-dock-configuration on #f yields '()"
             '() (resolve-dock-configuration #f))

;; resolve-dock-configuration: a real settings file is evaluated in isolation
(define %settings-file (tmp-path "settings.scm"))
(write-file %settings-file
            "(define dock-position 'top)
(define dock-icon-size 72)
(define dock-theme 'unity)
(define dock-auto-hide? #t)
(define dock-monitor \"DP-1\")
(define dock-mode 'taskbar)
(define dock-show-window-titles? #t)
(define dock-item-spacing 9)
(define dock-items (list (app-item #:name \"Terminal\"
                                   #:icon \"Alacritty\"
                                   #:exec \"alacritty\")))
")

(define %extracted (resolve-dock-configuration %settings-file))
(check "resolve-dock-configuration extracts keys from a settings file"
       (pair? %extracted))
(check-equal "extracted dock-position is 'top"
             'top (assq-ref %extracted 'dock-position))
(check-equal "extracted dock-icon-size is 72"
             72 (assq-ref %extracted 'dock-icon-size))
(check-equal "extracted dock-theme is 'unity"
             'unity (assq-ref %extracted 'dock-theme))
(check-equal "extracted dock-auto-hide? is #t"
             #t (assq-ref %extracted 'dock-auto-hide?))
(check-equal "extracted dock-monitor is DP-1"
             "DP-1" (assq-ref %extracted 'dock-monitor))
(check-equal "extracted dock-mode is 'taskbar"
             'taskbar (assq-ref %extracted 'dock-mode))
(check-equal "extracted dock-show-window-titles? is #t"
             #t (assq-ref %extracted 'dock-show-window-titles?))
(check-equal "extracted dock-item-spacing is 9"
             9 (assq-ref %extracted 'dock-item-spacing))
(check-equal "extracted one dock item"
             1 (length (assq-ref %extracted 'dock-items)))

;; =============================================================================
;; (lambdock state) — configure-dock! getters and setters
;; =============================================================================

(define %cfg-path (tmp-path "dock/settings.scm"))
(configure-dock! #:config-path %cfg-path
                 #:position 'left
                 #:icon-size 64
                 #:item-spacing 12
                 #:theme 'snow-leopard
                 #:monitor 'HDMI-1
                 #:items (list (app-item #:name "A" #:icon "a" #:exec "a"))
                 #:auto-hide? #t
                 #:show-when-clear? #f
                 #:exclude-apps '("daemon" "tracker")
                 #:enable-repl? #t
                 #:repl-socket-path "/tmp/x.sock"
                 #:hover-animate? #f
                 #:track-windows? #f
                 #:mode 'taskbar
                 #:show-window-titles? #t
                 #:shelf-tilt 12
                 #:shelf-margin -18
                 #:reflection 0.4)

(check-equal "config record is retrievable by path key"
             'left (get-dock-position %cfg-path))
(check-equal "getters accept the config path argument"
             64 (get-dock-icon-size %cfg-path))
(check-equal "item-spacing getter"
             12 (get-dock-item-spacing %cfg-path))
(check-equal "theme getter returns the configured symbol"
             'snow-leopard (get-dock-theme %cfg-path))
(check-equal "monitor getter returns the configured value"
             'HDMI-1 (get-dock-monitor %cfg-path))
(check-equal "auto-hide? getter"
             #t (get-dock-auto-hide? %cfg-path))
(check-equal "show-when-clear? getter"
             #f (get-dock-show-when-clear? %cfg-path))
(check-equal "enable-repl? getter"
             #t (get-dock-enable-repl? %cfg-path))
(check-equal "repl-socket-path getter"
             "/tmp/x.sock" (get-dock-repl-socket-path %cfg-path))
(check-equal "hover-animate? getter"
             #f (get-dock-hover-animate? %cfg-path))
(check-equal "track-windows? getter"
             #f (get-dock-track-windows? %cfg-path))
(check-equal "mode getter"
             'taskbar (get-dock-mode %cfg-path))
(check-equal "show-window-titles? getter"
             #t (get-dock-show-window-titles? %cfg-path))
(check-equal "shelf-tilt getter"
             12 (get-dock-shelf-tilt %cfg-path))
(check-equal "negative shelf-margin is kept as-is"
             -18 (get-dock-shelf-margin %cfg-path))
(check-equal "reflection getter"
             0.4 (get-dock-reflection %cfg-path))
(check-equal "exclude-apps getter"
             '("daemon" "tracker") (get-dock-exclude-apps %cfg-path))

;; A path ending in settings.scm also refreshes the 'default key
(check-equal "default key mirrors a settings.scm config"
             'left (get-dock-position))
(check-equal "unknown keys fall back to the default record"
             'left (get-dock-position 'totally-unknown-key))

;; Setters mutate only the targeted config when given a key argument
(set-dock-position! 'right %cfg-path)
(check-equal "set-dock-position! updates the targeted config"
             'right (get-dock-position %cfg-path))

(set-dock-theme! 'nature %cfg-path)
(check-equal "set-dock-theme! updates the targeted config"
             'nature (get-dock-theme %cfg-path))

(set-dock-icon-size! 96 %cfg-path)
(check-equal "set-dock-icon-size! updates the targeted config"
             96 (get-dock-icon-size %cfg-path))

(set-dock-item-spacing! 2 %cfg-path)
(check-equal "set-dock-item-spacing! updates the targeted config"
             2 (get-dock-item-spacing %cfg-path))

(set-dock-auto-hide! #f %cfg-path)
(check-equal "set-dock-auto-hide! updates the targeted config"
             #f (get-dock-auto-hide? %cfg-path))

(set-dock-show-when-clear! #t %cfg-path)
(check-equal "set-dock-show-when-clear! updates the targeted config"
             #t (get-dock-show-when-clear? %cfg-path))

(set-dock-monitor! 'DP-3 %cfg-path)
(check-equal "set-dock-monitor! updates the targeted config"
             'DP-3 (get-dock-monitor %cfg-path))

(set-dock-hover-animate! #t %cfg-path)
(check-equal "set-dock-hover-animate! updates the targeted config"
             #t (get-dock-hover-animate? %cfg-path))

(set-dock-track-windows! #f %cfg-path)
(check-equal "set-dock-track-windows! updates the targeted config"
             #f (get-dock-track-windows? %cfg-path))

(set-dock-mode! 'dock %cfg-path)
(check-equal "set-dock-mode! updates the targeted config"
             'dock (get-dock-mode %cfg-path))

(set-dock-show-window-titles! #f %cfg-path)
(check-equal "set-dock-show-window-titles! updates the targeted config"
             #f (get-dock-show-window-titles? %cfg-path))

;; =============================================================================
;; (lambdock items) — record constructors and accessors
;; =============================================================================

(define %app (app-item #:name "Firefox" #:icon "firefox" #:exec "firefox %u"))
(check "app-item produces a dock item" (dock-item? %app))
(check-equal "app-item type is 'app" 'app (dock-item-type %app))
(check-equal "app-item name accessor" "Firefox" (dock-item-name %app))
(check-equal "app-item icon accessor" "firefox" (dock-item-icon %app))
(check-equal "app-item exec accessor" "firefox %u" (dock-item-exec %app))
(check-not "app-item has no poll function by default"
           (dock-item-poll-fn %app))
(check-equal "app-item poll interval defaults to 0"
             0 (dock-item-poll-interval %app))
(check-equal "app-item hover animation defaults to #t"
             #t (dock-item-hover-animate? %app))

(check-equal "app-item defaults name to Unknown when omitted"
             "Unknown" (dock-item-name (app-item)))
(check-equal "app-item defaults icon to a generic executable"
             "application-x-executable" (dock-item-icon (app-item)))
(check-equal "app-item honours hover-animate? #f"
             #f (dock-item-hover-animate? (app-item #:hover-animate? #f)))

(define %sep (separator-item))
(check-equal "separator-item type is 'separator"
             'separator (dock-item-type %sep))
(check-equal "separator-item name is empty" "" (dock-item-name %sep))
(check-not "separator-item never hover-animates"
           (dock-item-hover-animate? %sep))

(define %dyn (dynamic-item #:name "Dyn" #:poll-fn (lambda () "x")))
(check-equal "dynamic-item type is 'dynamic"
             'dynamic (dock-item-type %dyn))
(check-equal "dynamic-item name accessor" "Dyn" (dock-item-name %dyn))
(check "dynamic-item keeps its poll function"
       (procedure? (dock-item-poll-fn %dyn)))
(check-equal "dynamic-item poll interval defaults to 2000"
             2000 (dock-item-poll-interval %dyn))
(check "dynamic-item poller returns the expected text"
       (equal? "x" ((dock-item-poll-fn %dyn))))

(define %dyn-custom (dynamic-item #:name "C" #:poll-interval 250
                                  #:hover-animate? #f))
(check-equal "dynamic-item honours a custom interval"
             250 (dock-item-poll-interval %dyn-custom))
(check-equal "dynamic-item honours hover-animate? #f"
             #f (dock-item-hover-animate? %dyn-custom))

;; =============================================================================
;; (lambdock items) — dynamic preset items
;; =============================================================================

(define %ram (ram-item))
(check-equal "ram-item is a dynamic item" 'dynamic (dock-item-type %ram))
(check-equal "ram-item is named RAM Usage" "RAM Usage" (dock-item-name %ram))
(check-equal "ram-item polls every 2000ms"
             2000 (dock-item-poll-interval %ram))
(check-not "ram-item disables hover animation"
           (dock-item-hover-animate? %ram))
(check "ram-item poll function is a procedure"
       (procedure? (dock-item-poll-fn %ram)))

(define %clock (clock-item))
(check-equal "clock-item is named Clock" "Clock" (dock-item-name %clock))
(check-equal "clock-item polls every 1000ms"
             1000 (dock-item-poll-interval %clock))

(define %cpu (cpu-item))
(check-equal "cpu-item is named CPU Usage" "CPU Usage" (dock-item-name %cpu))
(check-equal "cpu-item polls every 2000ms"
             2000 (dock-item-poll-interval %cpu))

(define %bat (battery-item))
(check-equal "battery-item is named Battery"
             "Battery" (dock-item-name %bat))
(check-equal "battery-item polls every 5000ms"
             5000 (dock-item-poll-interval %bat))
(check-equal "battery-item icon is battery"
             "battery" (dock-item-icon %bat))

(define %temp (cpu-temp-item))
(check-equal "cpu-temp-item is named CPU Temp"
             "CPU Temp" (dock-item-name %temp))
(check-equal "cpu-temp-item polls every 3000ms"
             3000 (dock-item-poll-interval %temp))

;; =============================================================================
;; (lambdock items) — poller output
;; =============================================================================

(check-string "get-clock-text returns a non-empty timestamp"
              (get-clock-text))
(check-matches "get-clock-text timestamp contains a time"
               "[0-9]" (get-clock-text))
(check-matches "get-used-ram-percent returns a RAM reading"
               "^RAM " (get-used-ram-percent))
(check-matches "get-battery-status returns a battery reading"
               "^(Bat N/A|[0-9]+%)" (get-battery-status))
(check-matches "get-cpu-temp returns a temperature reading"
               "^(Temp N/A|[0-9]+)" (get-cpu-temp))

;; get-cpu-percent is stateful: the first call has no baseline, and after a
;; short real interval the next call must produce a reading.
(check-string "get-cpu-percent first call returns a string"
              (get-cpu-percent))
(usleep 150000)
(check-matches "get-cpu-percent reports a percentage after a delta"
               "^CPU ([0-9]+%|N/A)" (get-cpu-percent))

;; The poller must never block the main thread: it used to sleep 100ms.
(let* ((t0 (get-internal-real-time))
       (_ (get-cpu-percent))
       (elapsed-ms (/ (* 1000 (- (get-internal-real-time) t0))
                      internal-time-units-per-second)))
  (check "get-cpu-percent does not block the caller"
         (< elapsed-ms 80)))

;; =============================================================================
;; (lambdock presets) — lookups
;; =============================================================================

(check-equal "preset-icon resolves a known key"
             "Alacritty" (preset-icon 'alacritty))
(check-equal "preset-icon resolves a browser key"
             "firefox" (preset-icon 'firefox))
(check-not "preset-icon returns #f for an unknown key"
           (preset-icon 'no-such-preset))
(check "preset-launcher resolves a known key"
       (dock-item? (preset-launcher 'alacritty)))
(check-equal "preset-launcher item carries the preset display name"
             "Terminal (Alacritty)" (dock-item-name (preset-launcher 'alacritty)))
(check-not "preset-launcher returns #f for an unknown key"
           (preset-launcher 'nope))

(check-equal "preset-launchers-for accepts variadic symbols"
             2 (length (preset-launchers-for 'alacritty 'firefox)))
(check-equal "preset-launchers-for accepts a single list"
             2 (length (preset-launchers-for '(alacritty firefox))))
(check-equal "preset-launchers-for drops unknown keys"
             1 (length (preset-launchers-for 'alacritty 'nope)))
(check-equal "preset-launchers-for on all-unknown keys yields '()"
             '() (preset-launchers-for 'nope 'nada))
(check-equal "preset-launchers-for preserves requested order"
             '("Terminal (Alacritty)" "Firefox")
             (map dock-item-name (preset-launchers-for 'alacritty 'firefox)))

;; =============================================================================
;; (lambdock core) — load-dock-config!
;; =============================================================================

(define %core-cfg (tmp-path "core.scm"))
(write-file %core-cfg
            "(define dock-position 'left)
(define dock-icon-size 40)
(define dock-theme 'unity)
(define dock-items (list (app-item #:name \"Core\"
                                   #:icon \"core\"
                                   #:exec \"core\")))
")

(load-dock-config! %core-cfg)
(check-equal "load-dock-config! registers position"
             'left (get-dock-position %core-cfg))
(check-equal "load-dock-config! registers icon size"
             40 (get-dock-icon-size %core-cfg))
(check-equal "load-dock-config! registers items"
             1 (length (get-dock-items %core-cfg)))
(check-equal "load-dock-config! registered item name"
             "Core" (dock-item-name (car (get-dock-items %core-cfg))))

;; load-dock-config! on a missing file must be a no-op (not an exception)
(load-dock-config! (tmp-path "missing.scm"))
(check-equal "load-dock-config! on a missing file leaves state intact"
             40 (get-dock-icon-size %core-cfg))

;; =============================================================================
;; Shelf parameters: no modes, no presets, just numbers
;; =============================================================================

;; Defaults describe the classic flat capsule.
(configure-dock! #:config-path 'shelf-defaults)
(check-equal "shelf defaults: tilt is 0 (flat)"
             0 (get-dock-shelf-tilt 'shelf-defaults))
(check-equal "shelf defaults: perspective"
             400 (get-dock-shelf-perspective 'shelf-defaults))
(check-equal "shelf defaults: margin is the capsule padding"
             24 (get-dock-shelf-margin 'shelf-defaults))
(check-equal "shelf defaults: no front glass"
             0 (get-dock-shelf-front 'shelf-defaults))
(check-equal "shelf defaults: reflections are off"
             0.0 (get-dock-reflection 'shelf-defaults))
(check-equal "shelf defaults: reflection intensity"
             0.55 (get-dock-reflection-intensity 'shelf-defaults))
(check-equal "shelf defaults: reflection blur"
             0.05 (get-dock-reflection-blur 'shelf-defaults))

;; Every knob is independent: a settings file can set any subset.
(define %shelf-cfg (tmp-path "shelf.scm"))
(write-file %shelf-cfg
            "(define dock-shelf-tilt 14)
(define dock-shelf-perspective 900)
(define dock-shelf-margin -22)
(define dock-shelf-front 18)
(define dock-reflection 0.5)
(define dock-reflection-intensity 0.4)
(define dock-reflection-blur 0.08)
")
(load-dock-config! %shelf-cfg)
(check-equal "tilt is read"
             14 (get-dock-shelf-tilt %shelf-cfg))
(check-equal "perspective is read"
             900 (get-dock-shelf-perspective %shelf-cfg))
(check-equal "a negative margin is read (icons overhang the shelf)"
             -22 (get-dock-shelf-margin %shelf-cfg))
(check-equal "front glass is read"
             18 (get-dock-shelf-front %shelf-cfg))
(check-equal "reflection band is read"
             0.5 (get-dock-reflection %shelf-cfg))
(check-equal "reflection intensity is read"
             0.4 (get-dock-reflection-intensity %shelf-cfg))
(check-equal "reflection blur is read"
             0.08 (get-dock-reflection-blur %shelf-cfg))

;; Unprefixed spellings work too, and unset knobs keep their default.
(define %partial-cfg (tmp-path "shelf-partial.scm"))
(write-file %partial-cfg
            "(define shelf-tilt 5)
(define shelf-front 12)
")
(load-dock-config! %partial-cfg)
(check-equal "unprefixed tilt is read"
             5 (get-dock-shelf-tilt %partial-cfg))
(check-equal "unprefixed front is read"
             12 (get-dock-shelf-front %partial-cfg))
(check-equal "unset margin keeps the default"
             24 (get-dock-shelf-margin %partial-cfg))
(check-equal "unset reflections stay off"
             0.0 (get-dock-reflection %partial-cfg))

;; Setters mutate only the targeted config.
(set-dock-shelf-tilt! 21 %shelf-cfg)
(check-equal "set-dock-shelf-tilt! updates the targeted config"
             21 (get-dock-shelf-tilt %shelf-cfg))
(set-dock-shelf-margin! -8 %shelf-cfg)
(check-equal "set-dock-shelf-margin! accepts negatives"
             -8 (get-dock-shelf-margin %shelf-cfg))
(set-dock-reflection! 0.75 %shelf-cfg)
(check-equal "set-dock-reflection! updates the targeted config"
             0.75 (get-dock-reflection %shelf-cfg))
(set-dock-shelf-front! 30 %shelf-cfg)
(check-equal "set-dock-shelf-front! updates the targeted config"
             30 (get-dock-shelf-front %shelf-cfg))


;; =============================================================================
;; Stacks, the launchpad and the dock layout overrides
;; =============================================================================

(define %stack (stack-item #:name "Downloads"
                           #:path "~/Downloads"
                           #:view-mode 'list))
(check-equal "stack-item creates a stack item"
             'stack (dock-item-type %stack))
(check-equal "stack-item keeps its folder"
             "~/Downloads" (dock-item-path %stack))
(check-equal "stack-item keeps its view mode"
             'list (dock-item-view-mode %stack))
(check-equal "stack-item defaults to the folder icon"
             "folder" (dock-item-icon %stack))
(check-equal "stacks are keyed by kind and name"
             "stack:Downloads" (dock-item-key %stack))

(check-equal "an unknown stack view mode falls back to grid"
             'grid (dock-item-view-mode (stack-item #:name "X"
                                                    #:view-mode 'carousel)))
(check-equal "the fan view mode is accepted"
             'fan (dock-item-view-mode (stack-item #:name "X"
                                                   #:view-mode 'fan)))

(define %launchpad (launchpad-item))
(check-equal "launchpad-item creates a launchpad"
             'launchpad (dock-item-type %launchpad))
(check-equal "launchpad items are keyed by kind and name"
             "launchpad:Launchpad" (dock-item-key %launchpad))
(check-equal "the launchpad defaults to the app grid icon"
             "view-app-grid-symbolic" (dock-item-icon %launchpad))

;; Plain app items carry no folder and the default view mode.
(check-equal "app items have no folder"
             "" (dock-item-path (app-item #:name "A")))
(check-equal "app items use the grid view"
             'grid (dock-item-view-mode (app-item #:name "A")))
(check-equal "app item keys"
             "app:A" (dock-item-key (app-item #:name "A")))

;; --- layout overrides -------------------------------------------------------

(define %layout-cfg (tmp-path "layout/settings.scm"))
(define %layout-items
  (list (app-item #:name "A" #:icon "a-icon" #:exec "a")
        (app-item #:name "B" #:icon "b-icon" #:exec "b")
        (separator-item)
        (app-item #:name "C" #:icon "c-icon" #:exec "c")))

(configure-dock! #:config-path %layout-cfg #:items %layout-items)

(check-equal "the override file is named after the settings file"
             (string-append (tmp-path "state")
                            "/lambdock/layout-settings.scm")
             (dock-layout-path %layout-cfg))

(check-equal "without overrides the configured order is kept"
             '("app:A" "app:B" "separator:" "app:C")
             (map dock-item-key (get-dock-items %layout-cfg)))

;; Reordering: C first, then A. Items the order does not mention (including the
;; unnamed separator) keep their place after their predecessor.
(dock-layout-set-order! %layout-cfg (list "app:C" "app:A"))
(check-equal "set-order reorders and keeps separators in place"
             '("app:C" "app:A" "app:B" "separator:")
             (map dock-item-key (get-dock-items %layout-cfg)))

;; Removal.
(dock-layout-remove! %layout-cfg "app:B")
(check-equal "remove drops an item from the dock"
             '("app:C" "app:A" "separator:")
             (map dock-item-key (get-dock-items %layout-cfg)))

;; Addition.
(dock-layout-add! %layout-cfg "app" "D" "d-icon" "d --start" "")
(check-equal "add appends a droplet item"
             '("app:C" "app:A" "separator:" "app:D")
             (map dock-item-key (get-dock-items %layout-cfg)))
(check-equal "the added item keeps its icon and command"
             '("d-icon" "d --start")
             (let ((item (car (reverse (get-dock-items %layout-cfg)))))
               (list (dock-item-icon item) (dock-item-exec item))))

;; A dropped folder becomes a stack.
(dock-layout-add! %layout-cfg "stack" "Shots" "folder" "/tmp/shots" "list")
(check-equal "add remembers stack folders"
             "/tmp/shots"
             (let ((item (car (reverse (get-dock-items %layout-cfg)))))
               (dock-item-path item)))

;; Per-stack view modes are remembered too.
(dock-layout-set-view! %layout-cfg "stack:Shots" 'fan)
(check-equal "set-view switches a stack's view mode"
             'fan
             (let ((item (car (reverse (get-dock-items %layout-cfg)))))
               (dock-item-view-mode item)))

;; The override file is real Scheme data, so it can be read back with `read`.
(define %saved-layout
  (call-with-input-file (dock-layout-path %layout-cfg) read))
(check "the override file contains an alist"
       (and (list? %saved-layout) (every pair? %saved-layout)))
(check-equal "the saved order is stored"
             '("app:C" "app:A")
             (assq-ref %saved-layout 'order))
(check-equal "the saved removals are stored"
             '("app:B")
             (assq-ref %saved-layout 'removed))
(check-equal "the saved additions are stored"
             '(("app" "D" "d-icon" "d --start" "")
               ("stack" "Shots" "folder" "/tmp/shots" "list"))
             (assq-ref %saved-layout 'added))
(check-equal "the saved stack view mode is stored"
             'fan
             (assoc-ref (assq-ref %saved-layout 'views) "stack:Shots"))

;; A hand written override file is applied on load: this is what makes the
;; layout survive a restart.
(define %preset-cfg (tmp-path "layout/preset-settings.scm"))
(write-file (dock-layout-path %preset-cfg)
            "((order \"app:Z\" \"app:A\")
  (removed \"app:Skip\")
  (added (\"app\" \"New\" \"new-icon\" \"new\" \"\"))
  (views))
")
(configure-dock! #:config-path %preset-cfg
                 #:items (list (app-item #:name "A" #:icon "a" #:exec "a")
                               (app-item #:name "Skip" #:icon "s" #:exec "s")
                               (app-item #:name "Z" #:icon "z" #:exec "z")))
(check-equal "a stored layout is replayed on top of the settings file"
             '("app:Z" "app:A" "app:New")
             (map dock-item-key (get-dock-items %preset-cfg)))

;; A broken override file must never take the dock down.
(define %broken-cfg (tmp-path "layout/broken-settings.scm"))
(write-file (dock-layout-path %broken-cfg) "(this is not (a layout")
(configure-dock! #:config-path %broken-cfg
                 #:items (list (app-item #:name "Only" #:icon "o" #:exec "o")))
(check-equal "a malformed override file is ignored"
             '("app:Only")
             (map dock-item-key (get-dock-items %broken-cfg)))

;; Resetting forgets everything again.
(dock-layout-reset! %layout-cfg)
(check-equal "reset restores the configured layout"
             '("app:A" "app:B" "separator:" "app:C")
             (map dock-item-key (get-dock-items %layout-cfg)))

;; Paths that are not settings files keep the plain configured items.
(check-equal "a symbol config key never reads overrides"
             '("app:A")
             (begin (configure-dock! #:config-path 'default
                                     #:items (list (app-item #:name "A")))
                    (map dock-item-key (get-dock-items))))

;; =============================================================================
;; Meter items, the numeric poll helpers and the shelf flare
;; =============================================================================

(define %plain-dynamic (dynamic-item #:name "Plain"))
(check-equal "dynamic items are text by default"
             #f (dock-item-meter? %plain-dynamic))
(check-equal "dynamic items have no unit by default"
             "" (dock-item-unit %plain-dynamic))
(check-equal "dynamic items fill at 100 by default"
             100 (dock-item-max %plain-dynamic))

(define %gauge (dynamic-item #:name "Gauge"
                             #:poll-fn (lambda () 42)
                             #:meter? #t
                             #:unit "%"
                             #:max 50))
(check-equal "dynamic-item can be a meter"
             #t (dock-item-meter? %gauge))
(check-equal "a meter keeps its unit" "%" (dock-item-unit %gauge))
(check-equal "a meter keeps its maximum" 50 (dock-item-max %gauge))
(check-equal "a meter's poll function is the one it was given"
             42 ((dock-item-poll-fn %gauge)))

;; The presets are meters by default and read numbers, which is what the gauge
;; needs; #:meter? #f falls back to the old text items.
(define %ram (ram-item))
(check-equal "ram-item is a meter by default" #t (dock-item-meter? %ram))
(check-equal "ram-item uses percent" "%" (dock-item-unit %ram))
(check-equal "ram-item fills at 100%" 100 (dock-item-max %ram))
(check "ram-item's poller returns a number (or nothing)"
       (let ((value ((dock-item-poll-fn %ram))))
         (or (number? value) (not value))))

(define %ram-text (ram-item #:meter? #f))
(check-equal "ram-item can still be text"
             #f (dock-item-meter? %ram-text))
(check-matches "ram-item's text poller returns a RAM reading"
               "^RAM " ((dock-item-poll-fn %ram-text)))

(define %cpu (cpu-item))
(check-equal "cpu-item is a meter by default" #t (dock-item-meter? %cpu))

(define %temp (cpu-temp-item))
(check-equal "cpu-temp-item is a meter by default" #t (dock-item-meter? %temp))
(check-equal "cpu-temp-item shows degrees" "°C" (dock-item-unit %temp))

;; Mixing the two forms is allowed: the meter knobs are independent of the poll
;; function's return type.
(check-equal "the clock stays a text item"
             #f (dock-item-meter? (clock-item)))

;; --- the shelf flare --------------------------------------------------------

(define %flare-cfg (tmp-path "flare.scm"))
(write-file %flare-cfg
            "(define dock-shelf-flare 26)
(define dock-shelf-margin -23)
")
(load-dock-config! %flare-cfg)
(check-equal "the keystone lean is read from the config"
             26 (get-dock-shelf-flare %flare-cfg))
(check-equal "the flare is independent of the margin"
             -23 (get-dock-shelf-margin %flare-cfg))
(check-equal "an unset flare means a rectangular slab"
             0 (get-dock-shelf-flare (tmp-path "no-flare.scm")))

(set-dock-shelf-flare! 40 %flare-cfg)
(check-equal "set-dock-shelf-flare! updates the targeted config"
             40 (get-dock-shelf-flare %flare-cfg))

;; --- the capsule's end padding ---------------------------------------------

(check-equal "the capsule padding defaults to 12"
             12 (get-dock-capsule-padding (tmp-path "no-padding.scm")))

(define %padding-cfg (tmp-path "padding.scm"))
(write-file %padding-cfg
            "(define dock-capsule-padding 8)
")
(load-dock-config! %padding-cfg)
(check-equal "the capsule padding is read from the config"
             8 (get-dock-capsule-padding %padding-cfg))

(set-dock-capsule-padding! 0 %padding-cfg)
(check-equal "set-dock-capsule-padding! accepts zero"
             0 (get-dock-capsule-padding %padding-cfg))
(set-dock-capsule-padding! 20 %padding-cfg)
(check-equal "set-dock-capsule-padding! updates the targeted config"
             20 (get-dock-capsule-padding %padding-cfg))

;; It is a layout knob, not a shelf knob: it works without any shelf geometry.
(check-equal "the padding is independent of the shelf"
             0 (get-dock-shelf-flare %padding-cfg))

;; =============================================================================
;; (lambdock prefs) — GUI preference overrides
;; =============================================================================

(define %prefs-cfg (tmp-path "prefs.scm"))
(write-file %prefs-cfg
            "(define dock-icon-size 48)
(define dock-auto-hide? #f)
(define dock-theme 'vanilla)
")
(load-dock-config! %prefs-cfg)
(check-equal "a fresh config starts from the settings file"
             48 (get-dock-icon-size %prefs-cfg))
(check-equal "a fresh config has no GUI overrides"
             '() (dock-preferences-read %prefs-cfg))
(check-matches "the override file is named after the settings file"
               "preferences-prefs\\.scm$" (dock-preferences-path %prefs-cfg))

;; A GUI edit updates the running dock immediately...
(set-dock-preference! 'dock-icon-size 64 %prefs-cfg)
(check-equal "set-dock-preference! applies the edit live"
             64 (get-dock-icon-size %prefs-cfg))
(check "set-dock-preference! writes the override file"
       (file-exists? (dock-preferences-path %prefs-cfg)))
(check-equal "the stored override is an alist of settings"
             '((dock-icon-size . 64)) (dock-preferences-read %prefs-cfg))

;; ...and survives reloading the generated settings file.
(load-dock-config! %prefs-cfg)
(check-equal "a GUI edit wins over the settings file on reload"
             64 (get-dock-icon-size %prefs-cfg))

(set-dock-preference! 'dock-auto-hide? #t %prefs-cfg)
(set-dock-preference! 'dock-theme 'macos-mavericks %prefs-cfg)
(check-equal "booleans are remembered" #t (get-dock-auto-hide? %prefs-cfg))
(check-equal "symbols are remembered" 'macos-mavericks
             (get-dock-theme %prefs-cfg))

;; Editing the same key twice replaces its entry instead of piling up.
(set-dock-preference! 'dock-icon-size 56 %prefs-cfg)
(check-equal "one entry per edited key"
             1 (length (filter (lambda (pair)
                                 (eq? (car pair) 'dock-icon-size))
                               (dock-preferences-read %prefs-cfg))))
(check-equal "the newest value wins" 56 (get-dock-icon-size %prefs-cfg))

;; apply-dock-preferences is what the loader consumes: the overrides are
;; prepended to the settings alist, so `assq' finds them first.
(define %apply-cfg (tmp-path "apply.scm"))
(write-file %apply-cfg "(define dock-icon-size 48)\n")
(load-dock-config! %apply-cfg)
(reset-dock-preferences! %apply-cfg)
(save-dock-preference! %apply-cfg 'dock-icon-size 72)
(check-equal "apply-dock-preferences prepends the overrides"
             '((dock-icon-size . 72) (icon-size . 48))
             (apply-dock-preferences '((icon-size . 48)) %apply-cfg))

;; reset forgets everything and the settings file takes over again.
(reset-dock-preferences! %prefs-cfg)
(check-not "reset removes the override file"
           (file-exists? (dock-preferences-path %prefs-cfg)))
(load-dock-config! %prefs-cfg)
(check-equal "reset restores the settings file value"
             48 (get-dock-icon-size %prefs-cfg))
(check-equal "reset restores the settings file theme"
             'vanilla (get-dock-theme %prefs-cfg))

;; =============================================================================
;; Summary
;; =============================================================================

(format (current-error-port) "1..~a\n" %total)
(if (zero? %failed)
    (begin (format #t "# all ~a Scheme assertions passed\n" %total) (exit 0))
    (begin (format (current-error-port) "# ~a of ~a Scheme assertions failed\n"
                   %failed %total)
           (exit 1)))
