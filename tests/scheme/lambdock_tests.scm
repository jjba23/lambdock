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
             (lambdock state)
             (lambdock items)
             (lambdock presets)
             (lambdock loader)
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
(check-equal "fallback config is not auto-hiding"
             #f (get-dock-auto-hide?))
(check-equal "fallback config mode is 'dock"
             'dock (get-dock-mode))
(check-equal "fallback config does not show window titles"
             #f (get-dock-show-window-titles?))
(check-equal "fallback config special mode is 'normal"
             'normal (get-dock-special-mode))

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
(define dock-special-mode 'big-cat)
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
(check-equal "extracted dock-special-mode is 'big-cat"
             'big-cat (assq-ref %extracted 'dock-special-mode))
(check-equal "extracted one dock item"
             1 (length (assq-ref %extracted 'dock-items)))

;; =============================================================================
;; (lambdock state) — configure-dock! getters and setters
;; =============================================================================

(define %cfg-path (tmp-path "dock/settings.scm"))
(configure-dock! #:config-path %cfg-path
                 #:position 'left
                 #:icon-size 64
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
                 #:special-mode 'big-cat)

(check-equal "config record is retrievable by path key"
             'left (get-dock-position %cfg-path))
(check-equal "getters accept the config path argument"
             64 (get-dock-icon-size %cfg-path))
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
(check-equal "special-mode getter"
             'big-cat (get-dock-special-mode %cfg-path))
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

(set-dock-special-mode! 'normal %cfg-path)
(check-equal "set-dock-special-mode! updates the targeted config"
             'normal (get-dock-special-mode %cfg-path))

(set-dock-special-mode! 'big-cat %cfg-path)
(check-equal "set-dock-special-mode! accepts 'big-cat"
             'big-cat (get-dock-special-mode %cfg-path))

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
;; Shelf parameters: one implementation, presets on top
;; =============================================================================

;; Defaults describe the plain flat dock.
(configure-dock! #:config-path 'shelf-defaults)
(check-equal "shelf defaults: tilt is 0 (flat)"
             0 (get-dock-shelf-tilt 'shelf-defaults))
(check-equal "shelf defaults: perspective"
             400 (get-dock-shelf-perspective 'shelf-defaults))
(check-equal "shelf defaults: the glass covers the whole icon"
             1.0 (get-dock-shelf-cover 'shelf-defaults))
(check-equal "shelf defaults: no glass floor"
             0 (get-dock-shelf-floor 'shelf-defaults))
(check-equal "shelf defaults: capsule padding"
             24 (get-dock-shelf-padding 'shelf-defaults))
(check-equal "shelf defaults: reflections are off"
             0.0 (get-dock-reflection 'shelf-defaults))
(check-equal "shelf defaults: reflection intensity"
             0.85 (get-dock-reflection-intensity 'shelf-defaults))
(check-equal "shelf defaults: reflection blur"
             0.035 (get-dock-reflection-blur 'shelf-defaults))

;; preset-special-mode: normal is empty, big-cat carries the Mavericks values,
;; unknown symbols fall back to the flat dock.
(check-equal "preset-special-mode 'normal is empty"
             '() (preset-special-mode 'normal))
(check-equal "preset-special-mode on an unknown mode is empty"
             '() (preset-special-mode 'not-a-mode))
(check-equal "preset-special-mode 'big-cat has 8 parameters"
             8 (length (preset-special-mode 'big-cat)))
(check-equal "preset-special-mode 'big-cat tilt"
             14 (assq-ref (preset-special-mode 'big-cat) 'shelf-tilt))
(check-equal "special-mode-presets lists the normal preset"
             '() (assq-ref special-mode-presets 'normal))

;; Loading a settings file that only selects the preset expands it.
(define %preset-cfg (tmp-path "bigcat.scm"))
(write-file %preset-cfg
            "(define dock-special-mode 'big-cat)\n")
(load-dock-config! %preset-cfg)
(check-equal "preset expands: tilt"
             14 (get-dock-shelf-tilt %preset-cfg))
(check-equal "preset expands: cover"
             0.58 (get-dock-shelf-cover %preset-cfg))
(check-equal "preset expands: floor"
             8 (get-dock-shelf-floor %preset-cfg))
(check-equal "preset expands: padding"
             0 (get-dock-shelf-padding %preset-cfg))
(check-equal "preset expands: reflection"
             0.52 (get-dock-reflection %preset-cfg))
(check-equal "preset expands: blur"
             0.035 (get-dock-reflection-blur %preset-cfg))

;; Explicit parameters win over the preset, prefixed or not, and the remaining
;; parameters still come from the preset.
(define %override-cfg (tmp-path "bigcat-tweak.scm"))
(write-file %override-cfg
            "(define dock-special-mode 'big-cat)
(define dock-shelf-tilt 4)
(define shelf-perspective 900)
(define reflection 0.2)
")
(load-dock-config! %override-cfg)
(check-equal "explicit dock-shelf-tilt wins over the preset"
             4 (get-dock-shelf-tilt %override-cfg))
(check-equal "explicit unprefixed shelf-perspective wins over the preset"
             900 (get-dock-shelf-perspective %override-cfg))
(check-equal "explicit reflection wins over the preset"
             0.2 (get-dock-reflection %override-cfg))
(check-equal "untouched parameters still come from the preset"
             8 (get-dock-shelf-floor %override-cfg))

;; A file without the special mode ignores any preset values.
(define %plain-cfg (tmp-path "plain.scm"))
(write-file %plain-cfg "(define dock-icon-size 40)\n")
(load-dock-config! %plain-cfg)
(check-equal "no special mode keeps the flat defaults"
             0 (get-dock-shelf-tilt %plain-cfg))
(check-equal "no special mode keeps reflections off"
             0.0 (get-dock-reflection %plain-cfg))

;; Setters mutate only the targeted config.
(set-dock-shelf-tilt! 21 %override-cfg)
(check-equal "set-dock-shelf-tilt! updates the targeted config"
             21 (get-dock-shelf-tilt %override-cfg))
(set-dock-reflection! 0.75 %override-cfg)
(check-equal "set-dock-reflection! updates the targeted config"
             0.75 (get-dock-reflection %override-cfg))

;; =============================================================================
;; Summary
;; =============================================================================

(format (current-error-port) "1..~a\n" %total)
(if (zero? %failed)
    (begin (format #t "# all ~a Scheme assertions passed\n" %total) (exit 0))
    (begin (format (current-error-port) "# ~a of ~a Scheme assertions failed\n"
                   %failed %total)
           (exit 1)))
