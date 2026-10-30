;;; lambdock -- hyper-hackable desktop dock for Wayland
;;;
;;; SPDX-License-Identifier: MIT
;;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

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
                 #:reflection 0.4
                 #:launchpad-icon-size 96
                 #:launchpad-columns 6
                 #:launchpad-blur? #t
                 #:reduce-motion? #t)

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
(check-equal "launchpad icon size getter"
             96 (get-dock-launchpad-icon-size %cfg-path))
(check-equal "launchpad columns getter"
             6 (get-dock-launchpad-columns %cfg-path))
(check-equal "launchpad blur getter"
             #t (get-dock-launchpad-blur? %cfg-path))
(check-equal "reduce-motion? getter"
             #t (get-dock-reduce-motion? %cfg-path))
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

(set-dock-launchpad-icon-size! 120 %cfg-path)
(check-equal "set-dock-launchpad-icon-size! updates the targeted config"
             120 (get-dock-launchpad-icon-size %cfg-path))

(set-dock-launchpad-columns! 9 %cfg-path)
(check-equal "set-dock-launchpad-columns! updates the targeted config"
             9 (get-dock-launchpad-columns %cfg-path))

(set-dock-launchpad-blur! #f %cfg-path)
(check-equal "set-dock-launchpad-blur! updates the targeted config"
             #f (get-dock-launchpad-blur? %cfg-path))

(set-dock-reduce-motion! #t %cfg-path)
(check-equal "set-dock-reduce-motion! updates the targeted config"
             #t (get-dock-reduce-motion? %cfg-path))
(set-dock-reduce-motion! #f %cfg-path)
(check-equal "set-dock-reduce-motion! can be turned off again"
             #f (get-dock-reduce-motion? %cfg-path))

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
(check-equal "the list view mode is accepted"
             'list (dock-item-view-mode (stack-item #:name "X"
                                                    #:view-mode 'list)))
(check-equal "the removed fan view mode falls back to grid"
             'grid (dock-item-view-mode (stack-item #:name "X"
                                                    #:view-mode 'fan)))

(define %launchpad (launchpad-item))
(check-equal "launchpad-item creates a launchpad"
             'launchpad (dock-item-type %launchpad))
(check-equal "launchpad items are keyed by kind and name"
             "launchpad:Launchpad" (dock-item-key %launchpad))
(check-equal "the launchpad defaults to the app grid icon"
             "view-app-grid-symbolic" (dock-item-icon %launchpad))

(define %trash (trash-item))
(check-equal "trash-item creates a trash item"
             'trash (dock-item-type %trash))
(check-equal "trash items are keyed by kind and name"
             "trash:Trash" (dock-item-key %trash))
(check-equal "the trash defaults to the empty trash icon"
             "user-trash" (dock-item-icon %trash))
(check-equal "trash-item takes a custom icon"
             "user-trash-symbolic"
             (dock-item-icon (trash-item #:icon "user-trash-symbolic")))
(check-equal "trash items do not animate at rest on request"
             #f (dock-item-hover-animate? (trash-item #:hover-animate? #f)))

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
(dock-layout-set-view! %layout-cfg "stack:Shots" 'list)
(check-equal "set-view switches a stack's view mode"
             'list
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
             'list
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

;; The dock's ready-made widgets can be added to the layout too, by name -- that
;; is what the Preferences window's "Add an item" row writes.
(define %widget-cfg (tmp-path "layout/widget-settings.scm"))
(configure-dock! #:config-path %widget-cfg
                 #:items (list (app-item #:name "A" #:icon "a" #:exec "a")))
(dock-layout-add! %widget-cfg "preset" "trash" "" "" "")
(dock-layout-add! %widget-cfg "preset" "clock" "" "" "")
(dock-layout-add! %widget-cfg "preset" "ram" "" "" "")
(check-equal "preset descriptors are replayed as the dock's widgets"
             '("app:A" "trash:Trash" "dynamic:Clock" "dynamic:RAM Usage")
             (map dock-item-key (get-dock-items %widget-cfg)))
(check-equal "the replayed RAM widget is a live gauge"
             #t
             (let ((item (car (reverse (get-dock-items %widget-cfg)))))
               (and (dock-item-meter? item) (procedure? (dock-item-poll-fn item)))))

;; Adding the same widget twice keeps one of it.
(dock-layout-add! %widget-cfg "preset" "ram" "" "" "")
(check-equal "adding the same preset twice is idempotent"
             4 (length (get-dock-items %widget-cfg)))

;; ...and it can be taken back out with the key it appeared under, which is what
;; "Remove from Dock" hands over.
(dock-layout-remove! %widget-cfg "dynamic:RAM Usage")
(check-equal "a preset widget can be removed by its item key"
             '("app:A" "trash:Trash" "dynamic:Clock")
             (map dock-item-key (get-dock-items %widget-cfg)))
(check-equal "removing a preset widget also drops its stored descriptor"
             '()
             (filter (lambda (desc) (equal? desc '("preset" "ram" "" "" "")))
                     (assq-ref (dock-layout-ref %widget-cfg) 'added)))

;; An unknown preset in the file is a warning, not a broken dock.
(define %unknown-widget-cfg (tmp-path "layout/unknown-widget.scm"))
(write-file (dock-layout-path %unknown-widget-cfg)
            "((order) (removed) (added (\"preset\" \"no-such-widget\" \"\" \"\" \"\")) (views))
")
(configure-dock! #:config-path %unknown-widget-cfg
                 #:items (list (app-item #:name "Solo" #:icon "s" #:exec "s")))
(check-equal "an unknown stored preset is skipped"
             '("app:Solo")
             (map dock-item-key (get-dock-items %unknown-widget-cfg)))

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

;; The override file is re-read every time it is needed, so a layout written by
;; another dock instance (or by hand) is visible on the next load instead of
;; being masked by a copy parsed at startup.
(define %reread-cfg (tmp-path "layout/reread-settings.scm"))
(configure-dock! #:config-path %reread-cfg
                 #:items (list (app-item #:name "A" #:icon "a" #:exec "a")
                               (app-item #:name "B" #:icon "b" #:exec "b")
                               (app-item #:name "C" #:icon "c" #:exec "c")))
(dock-layout-set-order! %reread-cfg '("app:C" "app:B" "app:A"))
(check-equal "the first read applies the stored order"
             '("app:C" "app:B" "app:A")
             (map dock-item-key (get-dock-items %reread-cfg)))

;; A second writer replaces the file while the dock is running.
(write-file (dock-layout-path %reread-cfg)
            "((order \"app:B\" \"app:C\" \"app:A\") (removed) (added) (views))\n")
(check-equal "a layout changed on disk is picked up without a restart"
             '("app:B" "app:C" "app:A")
             (map dock-item-key (get-dock-items %reread-cfg)))

;; A dock with no settings path of its own -- the fallback dock, or a REPL call
;; with no arguments -- uses the layout written for the default settings file
;; instead of dropping its overrides because the key is not a string.
(define %fallback-root (tmp-path "fallback"))
(define %fallback-cfg (string-append %fallback-root "/lambdock/settings.scm"))

(with-env `(("XDG_CONFIG_HOME" . ,%fallback-root))
  (lambda ()
    (configure-dock! #:config-path %fallback-cfg
                     #:items (list (app-item #:name "A" #:icon "a" #:exec "a")
                                   (app-item #:name "B" #:icon "b" #:exec "b")))
    (dock-layout-set-order! %fallback-cfg '("app:B" "app:A"))
    (check-equal "a PATH-less dock applies the default settings layout"
                 '("app:B" "app:A")
                 (map dock-item-key (get-dock-items)))
    (dock-layout-reset! %fallback-cfg)))

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

;; --- the launchpad -----------------------------------------------------------

(check-equal "launchpad icon size defaults to 80"
             80 (get-dock-launchpad-icon-size (tmp-path "no-launchpad.scm")))
(check-equal "launchpad columns default to automatic"
             0 (get-dock-launchpad-columns (tmp-path "no-launchpad.scm")))
(check-equal "the launchpad is solid unless asked otherwise"
             #f (get-dock-launchpad-blur? (tmp-path "no-launchpad.scm")))

(define %launchpad-cfg (tmp-path "launchpad.scm"))
(write-file %launchpad-cfg
            "(define dock-launchpad-icon-size 80)
(define dock-launchpad-columns 5)
(define dock-launchpad-blur? #t)
")
(load-dock-config! %launchpad-cfg)
(check-equal "the launchpad icon size is read from the config"
             80 (get-dock-launchpad-icon-size %launchpad-cfg))
(check-equal "the launchpad column count is read from the config"
             5 (get-dock-launchpad-columns %launchpad-cfg))
(check-equal "the frosted background is read from the config"
             #t (get-dock-launchpad-blur? %launchpad-cfg))

;; The launchpad's close button sits on the left unless asked otherwise, and
;; only "right" is a side: anything else is the documented default.
(check-equal "the launchpad close button is on the left by default"
             'left (get-dock-launchpad-close-side %launchpad-cfg))

(define %launchpad-side-cfg (tmp-path "launchpad-side.scm"))
(write-file %launchpad-side-cfg "(define dock-launchpad-close-side 'right)\n")
(load-dock-config! %launchpad-side-cfg)
(check-equal "the close side is read from the config"
             'right (get-dock-launchpad-close-side %launchpad-side-cfg))

(set-dock-launchpad-close-side! 'left %launchpad-side-cfg)
(check-equal "set-dock-launchpad-close-side! moves it back"
             'left (get-dock-launchpad-close-side %launchpad-side-cfg))
(set-dock-launchpad-close-side! 'sideways %launchpad-side-cfg)
(check-equal "an unknown side falls back to the left"
             'left (get-dock-launchpad-close-side %launchpad-side-cfg))

;; --- accessibility -----------------------------------------------------------

(check-equal "motion is not reduced by default"
             #f (get-dock-reduce-motion? (tmp-path "no-motion.scm")))

(define %motion-cfg (tmp-path "motion.scm"))
(write-file %motion-cfg "(define dock-reduce-motion? #t)\n")
(load-dock-config! %motion-cfg)
(check-equal "the reduced-motion flag is read from the config"
             #t (get-dock-reduce-motion? %motion-cfg))

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

(set-dock-preference! 'dock-launchpad-icon-size 112 %prefs-cfg)
(set-dock-preference! 'dock-launchpad-columns 4 %prefs-cfg)
(set-dock-preference! 'dock-launchpad-blur? #t %prefs-cfg)
(check-equal "launchpad numbers are remembered"
             112 (get-dock-launchpad-icon-size %prefs-cfg))
(check-equal "launchpad columns are remembered"
             4 (get-dock-launchpad-columns %prefs-cfg))
(check-equal "the launchpad blur preference is remembered"
             #t (get-dock-launchpad-blur? %prefs-cfg))

(set-dock-preference! 'dock-reduce-motion? #t %prefs-cfg)
(check-equal "the reduced-motion preference is remembered"
             #t (get-dock-reduce-motion? %prefs-cfg))

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
;; (lambdock looks) — whole-dock recipes
;; =============================================================================

(check "preset-look resolves a shipped look"
       (pair? (preset-look 'osx-tiger)))
(check-equal "look-settings is the same accessor"
             (preset-look 'osx-tiger) (look-settings 'osx-tiger))
(check "look? knows a shipped look" (look? 'osx-mavericks-dark))
(check-not "look? rejects a made-up look" (look? 'osx-dock-of-theseus))
(check-not "preset-look returns #f for an unknown look"
           (preset-look 'no-such-look))
(check-not "preset-look returns #f for a non-symbol"
           (preset-look 42))

(check-equal "look-name resolves a short alias"
             'osx-mavericks (look-name 'mavericks))
(check-equal "look-name resolves a theme name to its look"
             'osx-snow-leopard (look-name 'snow-leopard))
(check-equal "look-name accepts a string"
             'osx-tahoe-dark (look-name "tahoe-dark"))
(check-not "look-name returns #f for an unknown name"
           (look-name 'nonsense))

(check-equal "look-setting reads a recipe value"
             52 (look-setting 'osx-tahoe 'icon-size))
(check-equal "look-setting accepts the prefixed spelling"
             52 (look-setting 'osx-tahoe 'dock-icon-size))
(check-equal "look-setting reads the theme"
             'macos-mavericks (look-setting 'osx-mavericks 'theme))
(check-not "look-setting returns #f for a missing key"
           (look-setting 'osx-mavericks 'no-such-setting))

(check-equal "the tiger recipe wears the tiger theme"
             'macos-tiger (look-setting 'osx-tiger 'theme))
(check-equal "the tiger strip is glass above the icons"
             14 (look-setting 'osx-tiger 'shelf-margin))
(check-equal "the tiger strip has a front lip"
             6 (look-setting 'osx-tiger 'shelf-front))

;; Every recipe has to be a complete dock description: switching looks live
;; must never inherit a number from the look that was there before.
(define %look-required-keys
  '(theme icon-size item-spacing capsule-padding mode show-window-titles?
    hover-animate?
    shelf-tilt shelf-perspective shelf-margin shelf-front shelf-flare
    reflection reflection-intensity reflection-blur))

(define (look-integer-in? value lo hi)
  (and (integer? value) (>= value lo) (<= value hi)))

(define (look-real-in? value lo hi)
  (and (real? value) (>= value lo) (<= value hi)))

(define (look-problem look-entry)
  "Why LOOK-ENTRY is not a usable recipe, or #f when it is."
  (let* ((name (car look-entry))
         (settings (cdr look-entry))
         (ref (lambda (key) (assq-ref settings key))))
    (cond
     ((not (symbol? name)) "name is not a symbol")
     ((any (lambda (key) (not (assq key settings))) %look-required-keys)
      "missing a required setting")
     ((not (symbol? (ref 'theme))) "theme is not a symbol")
     ((not (look-integer-in? (ref 'icon-size) 8 256)) "icon-size out of range")
     ((not (look-integer-in? (ref 'item-spacing) 0 64)) "item-spacing out of range")
     ((not (look-integer-in? (ref 'capsule-padding) 0 96))
      "capsule-padding out of range")
     ((not (memq (ref 'mode) '(dock taskbar))) "mode is not dock or taskbar")
     ((not (boolean? (ref 'show-window-titles?)))
      "show-window-titles? is not a boolean")
     ((not (boolean? (ref 'hover-animate?)))
      "hover-animate? is not a boolean")
     ((not (look-real-in? (ref 'shelf-tilt) -45 45)) "shelf-tilt out of range")
     ((not (look-integer-in? (ref 'shelf-perspective) 50 5000))
      "shelf-perspective out of range")
     ((not (look-integer-in? (ref 'shelf-margin) -200 200))
      "shelf-margin out of range")
     ((not (look-integer-in? (ref 'shelf-front) 0 200))
      "shelf-front out of range")
     ((not (look-integer-in? (ref 'shelf-flare) -80 80))
      "shelf-flare out of range")
     ((not (look-real-in? (ref 'reflection) 0 2)) "reflection out of range")
     ((not (look-real-in? (ref 'reflection-intensity) 0 1))
      "reflection-intensity out of range")
     ((not (look-real-in? (ref 'reflection-blur) 0 0.25))
      "reflection-blur out of range")
     (else #f))))

(define %look-problems
  (filter-map (lambda (entry)
                (let ((problem (look-problem entry)))
                  (and problem (cons (car entry) problem))))
              dock-looks))

(check-equal "every look recipe is complete and in range"
             '() %look-problems)

(check-equal "every look alias points at a real recipe"
             '()
             (filter (lambda (alias) (not (assq (cdr alias) dock-looks)))
                     look-aliases))

;; Every shipped theme should have a look that was designed for it, so a user
;; can name a theme and get the dock it belongs to.
(define %shipped-themes
  '(vanilla vanilla-light snow-leopard snow-leopard-dark solarized-light
    unity nature macos-tiger macos-big-sur macos-ventura macos-tahoe
    macos-tahoe-dark macos-mavericks macos-mavericks-dark windows-xp
    windows-vista windows-7 windows-10 windows-11))

(check-equal "every shipped theme has a look that wears it"
             '()
             (filter (lambda (theme)
                       (not (any (lambda (entry)
                                   (eq? theme (assq-ref (cdr entry) 'theme)))
                                 dock-looks)))
                     %shipped-themes))

;; The five Windows looks are one family: they must all be bars (taskbar mode,
;; 32px icons, no shelf geometry) and only differ in their chrome -- the labels
;; are on where that Windows showed them (XP, Vista) and off where it did not
;; (7, 10, 11).
(define %windows-looks
  '((windows-xp . #t)
    (windows-vista . #t)
    (windows-7 . #f)
    (windows-10 . #f)
    (windows-11 . #f)))

(check-equal "every Windows look is a taskbar of 32px icons with no shelf"
             '()
             (filter-map
              (lambda (entry)
                (let* ((name (car entry))
                       (look (preset-look name))
                       (theme (assq-ref look 'theme)))
                  (and (or (not (eq? (assq-ref look 'mode) 'taskbar))
                           (not (equal? (assq-ref look 'icon-size) 32))
                           (not (equal? (assq-ref look 'shelf-margin) 24))
                           (not (equal? (assq-ref look 'shelf-front) 0))
                           (not (equal? (assq-ref look 'shelf-flare) 0))
                           (not (equal? (assq-ref look 'shelf-tilt) 0))
                           (not (equal? (assq-ref look 'reflection) 0.0))
                           (not (string-prefix? "windows-" (symbol->string theme))))
                       (cons name theme))))
              %windows-looks))

(check-equal "window titles follow what each Windows actually showed"
             '()
             (filter-map (lambda (entry)
                           (let ((shown (assq-ref (preset-look (car entry))
                                                  'show-window-titles?)))
                             (and (not (eq? shown (cdr entry))) (car entry))))
                         %windows-looks))

(check-equal "the Windows looks get airier from XP to 11"
             '(0 4 6 4 8)
             (map (lambda (name) (look-setting name 'item-spacing))
                  (map car %windows-looks)))

(check-equal "the vista alias is spelled aero too"
             'windows-vista (look-name 'aero))
(check-equal "the seven alias resolves to windows-7"
             'windows-7 (look-name 'seven))
(check-equal "the aero look wears the vista theme"
             'windows-vista (look-setting 'windows-vista 'theme))
(check-equal "the seven look wears the windows 7 theme"
             'windows-7 (look-setting 'windows-7 'theme))

;; --- the loader resolves a look ---------------------------------------------

(define %look-cfg (tmp-path "look-settings.scm"))
(write-file %look-cfg
            "(define dock-look 'osx-mavericks)
(define dock-items (list (app-item #:name \"Look\"
                                   #:icon \"look\"
                                   #:exec \"look\")))
")
(load-dock-config! %look-cfg)
(check-equal "dock-look brings the recipe's theme"
             'macos-mavericks (get-dock-theme %look-cfg))
(check-equal "dock-look brings the recipe's icon size"
             48 (get-dock-icon-size %look-cfg))
(check-equal "dock-look brings the recipe's spacing"
             4 (get-dock-item-spacing %look-cfg))
(check-equal "dock-look brings the recipe's end padding"
             8 (get-dock-capsule-padding %look-cfg))
(check-equal "dock-look brings the recipe's overhang"
             -18 (get-dock-shelf-margin %look-cfg))
(check-equal "dock-look brings the recipe's front glass"
             24 (get-dock-shelf-front %look-cfg))
(check-equal "dock-look brings the recipe's keystone"
             24 (get-dock-shelf-flare %look-cfg))
(check-equal "dock-look brings the recipe's reflection band"
             0.30 (get-dock-reflection %look-cfg))

;; The dark sibling: only the theme moves.
(define %look-dark-cfg (tmp-path "look-dark.scm"))
(write-file %look-dark-cfg "(define dock-look 'mavericks-dark)\n")
(load-dock-config! %look-dark-cfg)
(check-equal "an alias brings the dark theme"
             'macos-mavericks-dark (get-dock-theme %look-dark-cfg))
(check-equal "an alias keeps the geometry"
             -18 (get-dock-shelf-margin %look-dark-cfg))

;; A string works too, and a taskbar look switches the mode.
(define %look-taskbar-cfg (tmp-path "look-taskbar.scm"))
(write-file %look-taskbar-cfg "(define dock-look \"xp\")\n")
(load-dock-config! %look-taskbar-cfg)
(check-equal "a look given as a string is resolved"
             'windows-xp (get-dock-theme %look-taskbar-cfg))
(check-equal "the xp look is a taskbar"
             'taskbar (get-dock-mode %look-taskbar-cfg))
(check-equal "the xp look shows window titles"
             #t (get-dock-show-window-titles? %look-taskbar-cfg))

;; The next era along: the same kind of bar, with labels off.
(define %look-win7-cfg (tmp-path "look-win7.scm"))
(write-file %look-win7-cfg "(define dock-look 'seven)\n")
(load-dock-config! %look-win7-cfg)
(check-equal "the seven alias brings the windows 7 theme"
             'windows-7 (get-dock-theme %look-win7-cfg))
(check-equal "the windows 7 look is a taskbar"
             'taskbar (get-dock-mode %look-win7-cfg))
(check-equal "the windows 7 look hides window titles"
             #f (get-dock-show-window-titles? %look-win7-cfg))
(check-equal "the windows 7 look spaces its icons"
             6 (get-dock-item-spacing %look-win7-cfg))
(check-equal "the windows 7 look pads the bar's ends"
             8 (get-dock-capsule-padding %look-win7-cfg))

;; A look carries the dock's manners too: the OS X docks magnified the icons
;; under the pointer, no Windows taskbar ever did.
(check-equal "the macOS looks magnify"
             #t (look-setting 'osx-mavericks 'hover-animate?))
(check-equal "the Windows looks do not magnify"
             '()
             (filter (lambda (entry)
                       (assq-ref (preset-look (car entry)) 'hover-animate?))
                     %windows-looks))
(check-not "the plain taskbar does not magnify either"
           (look-setting 'plain-taskbar 'hover-animate?))

;; ...and apply-look! sets and clears it, so switching families switches manners.
(define %look-magnify-cfg (tmp-path "look-magnify.scm"))
(write-file %look-magnify-cfg "(define dock-hover-animate? #t)\n")
(load-dock-config! %look-magnify-cfg)
(apply-look! 'windows-11 %look-magnify-cfg)
(check-equal "a Windows look turns the fisheye off"
             #f (get-dock-hover-animate? %look-magnify-cfg))
(apply-look! 'osx-tahoe %look-magnify-cfg)
(check-equal "a macOS look turns the fisheye back on"
             #t (get-dock-hover-animate? %look-magnify-cfg))

;; What the file says itself beats the recipe.
(define %look-override-cfg (tmp-path "look-override.scm"))
(write-file %look-override-cfg
            "(define dock-look 'osx-tahoe)
(define dock-icon-size 40)
(define dock-theme 'vanilla)
(define dock-shelf-margin 30)
")
(load-dock-config! %look-override-cfg)
(check-equal "an explicit icon size wins over the recipe"
             40 (get-dock-icon-size %look-override-cfg))
(check-equal "an explicit theme wins over the recipe"
             'vanilla (get-dock-theme %look-override-cfg))
(check-equal "an explicit shelf number wins over the recipe"
             30 (get-dock-shelf-margin %look-override-cfg))
(check-equal "the rest of the recipe still applies"
             16 (get-dock-capsule-padding %look-override-cfg))

;; An unknown look is a warning, not a crash: the file's own settings load and
;; the dock stays flat.
(define %look-bogus-cfg (tmp-path "look-bogus.scm"))
(write-file %look-bogus-cfg
            "(define dock-look 'no-such-look)
(define dock-icon-size 36)
")
(load-dock-config! %look-bogus-cfg)
(check-equal "an unknown look keeps the file's own settings"
             36 (get-dock-icon-size %look-bogus-cfg))
(check-equal "an unknown look leaves the theme alone"
             'vanilla (get-dock-theme %look-bogus-cfg))
(check-equal "an unknown look leaves the shelf flat"
             24 (get-dock-shelf-margin %look-bogus-cfg))

;; --- apply-look! restyles a running dock ------------------------------------

(define %apply-look-cfg (tmp-path "look-apply.scm"))
(write-file %apply-look-cfg "(define dock-icon-size 48)\n")
(load-dock-config! %apply-look-cfg)

(check "apply-look! reports success"
       (apply-look! 'osx-tahoe-dark %apply-look-cfg))
(check-equal "apply-look! switches the theme"
             'macos-tahoe-dark (get-dock-theme %apply-look-cfg))
(check-equal "apply-look! switches the icon size"
             52 (get-dock-icon-size %apply-look-cfg))
(check-equal "apply-look! switches the end padding"
             16 (get-dock-capsule-padding %apply-look-cfg))

;; A shelf look writes the whole geometry...
(apply-look! 'snow-leopard %apply-look-cfg)
(check-equal "apply-look! sets the overhang"
             -20 (get-dock-shelf-margin %apply-look-cfg))
(check-equal "apply-look! sets the front glass"
             28 (get-dock-shelf-front %apply-look-cfg))
(check-equal "apply-look! sets the keystone"
             26 (get-dock-shelf-flare %apply-look-cfg))
(check-equal "apply-look! sets the reflection band"
             0.42 (get-dock-reflection %apply-look-cfg))

;; ...and a flat look takes it all back again, instead of inheriting it.
(apply-look! 'osx-tahoe %apply-look-cfg)
(check-equal "a flat look resets the overhang"
             24 (get-dock-shelf-margin %apply-look-cfg))
(check-equal "a flat look resets the front glass"
             0 (get-dock-shelf-front %apply-look-cfg))
(check-equal "a flat look resets the keystone"
             0 (get-dock-shelf-flare %apply-look-cfg))
(check-equal "a flat look resets the reflection band"
             0.0 (get-dock-reflection %apply-look-cfg))
(check-equal "a flat look returns to dock mode"
             'dock (get-dock-mode %apply-look-cfg))

;; An unknown look changes nothing and says so.
(define %theme-before-apply (get-dock-theme %apply-look-cfg))
(check-not "apply-look! refuses an unknown look"
           (apply-look! 'no-such-look %apply-look-cfg))
(check-equal "a refused look leaves the dock untouched"
             %theme-before-apply (get-dock-theme %apply-look-cfg))

;; apply-look! also takes a config key, like the setters do.
(configure-dock! #:config-path 'look-repl-dock #:theme 'vanilla)
(apply-look! 'unity 'look-repl-dock)
(check-equal "apply-look! targets one dock when given its key"
             'unity (get-dock-theme 'look-repl-dock))

;; =============================================================================
;; Summary
;; =============================================================================

(format (current-error-port) "1..~a\n" %total)
(if (zero? %failed)
    (begin (format #t "# all ~a Scheme assertions passed\n" %total) (exit 0))
    (begin (format (current-error-port) "# ~a of ~a Scheme assertions failed\n"
                   %failed %total)
           (exit 1)))
