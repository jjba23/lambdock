;; lambdock -- hyper-hackable desktop dock for Wayland
;;
;; SPDX-License-Identifier: MIT
;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

(define-module (lambdock core)
  #:declarative? #f
  #:use-module (ice-9 format)
  #:use-module (ice-9 threads)
  #:use-module (lambdock items)
  #:use-module (lambdock state)
  #:use-module (lambdock loader)
  #:use-module (lambdock logger)
  #:use-module (lambdock presets)
  #:use-module (lambdock looks)
  #:use-module (lambdock layout)
  #:use-module (lambdock prefs)
  #:use-module (system repl server)
  #:re-export ( ;Item structures
                <dock-item>
               dock-item?
               dock-item-type
               dock-item-name
               dock-item-icon
               dock-item-exec
               dock-item-poll-fn
               dock-item-poll-interval
               dock-item-hover-animate?
               dock-item-path
               dock-item-view-mode
               dock-item-meter?
               dock-item-clock?
               dock-item-unit
               dock-item-max
               dock-item-key
               app-item
               separator-item
               dynamic-item
               stack-item
               launchpad-item
               trash-item
               ;; Dynamic items
               get-used-ram-percent
               get-used-ram-value
               ram-item
               get-clock-text
               clock-item
               get-cpu-percent
               get-cpu-value
               cpu-item
               get-battery-status
               battery-item
               get-cpu-temp
               get-cpu-temp-value
               cpu-temp-item

               ;; Primary Configurator
               configure-dock!

               ;; Getters
               get-dock-config
               get-dock-items
               get-dock-position
               get-dock-theme
               get-dock-icon-size
               get-dock-item-spacing
               get-dock-capsule-padding
               get-dock-auto-hide?
               get-dock-show-when-clear?
               get-dock-monitor
               get-dock-exclude-apps
               get-dock-enable-repl?
               get-dock-repl-socket-path
               get-dock-hover-animate?
               get-dock-track-windows?
               get-dock-mode
               get-dock-show-window-titles?
               get-dock-shelf-tilt
               get-dock-shelf-perspective
               get-dock-shelf-margin
               get-dock-shelf-front
               get-dock-reflection
               get-dock-reflection-intensity
               get-dock-reflection-blur
               get-dock-shelf-flare
               get-dock-launchpad-icon-size
               get-dock-launchpad-columns
               get-dock-launchpad-blur?
               get-dock-launchpad-close-side
               get-dock-reduce-motion?

               ;; Setters
               set-dock-position!
               set-dock-theme!
               set-dock-icon-size!
               set-dock-item-spacing!
               set-dock-capsule-padding!
               set-dock-auto-hide!
               set-dock-show-when-clear!
               set-dock-monitor!
               set-dock-hover-animate!
               set-dock-track-windows!
               set-dock-mode!
               set-dock-show-window-titles!
               set-dock-shelf-tilt!
               set-dock-shelf-perspective!
               set-dock-shelf-margin!
               set-dock-shelf-front!
               set-dock-reflection!
               set-dock-reflection-intensity!
               set-dock-reflection-blur!
               set-dock-shelf-flare!
               set-dock-launchpad-icon-size!
               set-dock-launchpad-columns!
               set-dock-launchpad-blur!
               set-dock-launchpad-close-side!
               set-dock-reduce-motion!

               ;; Config Loader
               resolve-dock-configuration
               ;; Alists
               preset-icons
               preset-launchers

               ;; Lookup & Convenience Functions
               preset-icon
               preset-launcher
               preset-launchers-for

               ;; Whole-dock look recipes
               dock-looks
               look-aliases
               look-name
               preset-look
               look-settings
               look-setting
               look?
               apply-look!

               ;; Dock layout overrides (written by drag and drop)
               dock-layout-path
               dock-layout-ref
               dock-layout-reset!
               apply-dock-overrides
               dock-layout-add!
               dock-layout-remove!
               dock-layout-set-order!
               dock-layout-set-view!

               ;; GUI preference overrides (written by the Preferences window)
               dock-preferences-path
               dock-preferences-read
               set-dock-preference!
               reset-dock-preferences!)
  #:export (init-dock! load-dock-config! start-repl!))

;; =============================================================================
;; Core Hooks
;; =============================================================================

(define* (load-dock-config! file-path)
  "Loads a specific settings file in isolation and registers its configuration record.

A settings file can name a whole-dock recipe with `(define dock-look 'NAME)`
(see (lambdock looks)); everything the recipe does not mention falls back to the
dock's defaults, and anything the file defines itself wins over the recipe.

Every shelf/reflection knob is an independent setting; anything you leave out
keeps its default (the plain flat dock)."
  (if (and file-path
           (file-exists? file-path))
      (begin
        (log-info "Core" "Loading user settings from: ~a" file-path)
        (catch #t
               (lambda ()
                 (let* ((settings
                         ;; GUI preference overrides are layered on top of the
                         ;; settings file, so a regenerated settings file keeps
                         ;; the choices made in the Preferences window.
                         (apply-dock-preferences
                          (resolve-dock-configuration file-path)
                          file-path))
                        ;; A whole-dock recipe: (define dock-look 'osx-mavericks)
                        ;; brings a theme, an icon size and a whole shelf
                        ;; geometry in one line (see (lambdock looks)).
                        (look-request (or (assq-ref settings 'dock-look)
                                          (assq-ref settings 'look)))
                        (look (look-name look-request))
                        (look-settings (or (preset-look look-request) '()))
                        ;; Helper to lookup both 'dock-foo and 'foo: the file
                        ;; (with the GUI overrides on top) is asked first and
                        ;; the look's recipe answers for whatever it leaves
                        ;; out, so an explicit definition always wins.
                        (cfg (lambda (key alt-key default-val)
                               (let ((pair (or (assq key settings)
                                               (assq alt-key settings)
                                               (assq key look-settings)
                                               (assq alt-key look-settings))))
                                 (if pair
                                     (cdr pair) default-val)))))

                   (when look-request
                     (if look
                         (log-info "Core"
                          "Look '~a' in [~a]: ~a settings from the recipe (the file's own definitions win over it)"
                          look file-path (length look-settings))
                         (log-warn "Core"
                          "Unknown dock-look '~a' in [~a]: ignoring it, the dock keeps the flat defaults"
                          look-request file-path)))

                   (configure-dock! #:config-path file-path
                                    #:position (cfg 'dock-position
                                                    'position
                                                    'bottom)
                                    #:icon-size (cfg 'dock-icon-size
                                                     'icon-size 48)
                                    #:item-spacing (cfg 'dock-item-spacing
                                                        'item-spacing 4)
                                    #:capsule-padding (cfg 'dock-capsule-padding
                                                           'capsule-padding 12)
                                    #:items (cfg 'dock-items
                                                 'items
                                                 '())
                                    #:theme (cfg 'dock-theme
                                                 'theme
                                                 'vanilla)
                                    #:monitor (cfg 'dock-monitor
                                                   'monitor
                                                   'default)
                                    #:auto-hide? (cfg 'dock-auto-hide?
                                                      'auto-hide? #f)
                                    #:show-when-clear? (cfg 'dock-show-when-clear?
                                                            'show-when-clear?
                                                            #t)
                                    #:exclude-apps (cfg 'dock-exclude-apps
                                                        'exclude-apps
                                                        '())
                                    #:enable-repl? (cfg 'dock-enable-repl?
                                                        'enable-repl? #f)
                                    #:repl-socket-path (cfg 'dock-repl-socket-path
                                                        'repl-socket-path
                                                        "/tmp/lambdock-repl.sock")
                                    #:hover-animate? (cfg 'dock-hover-animate?
                                                          'hover-animate? #t)
                                    #:track-windows? (cfg 'dock-track-windows?
                                                          'track-windows? #t)
                                    #:mode (cfg 'dock-mode
                                                'mode 'dock)
                                    #:show-window-titles?
                                    (cfg 'dock-show-window-titles?
                                         'show-window-titles? #f)
                                    #:shelf-tilt (cfg 'dock-shelf-tilt
                                                      'shelf-tilt 0)
                                    #:shelf-perspective
                                    (cfg 'dock-shelf-perspective
                                         'shelf-perspective 400)
                                    #:shelf-margin (cfg 'dock-shelf-margin
                                                        'shelf-margin 24)
                                    #:shelf-front (cfg 'dock-shelf-front
                                                       'shelf-front 0)
                                    #:reflection (cfg 'dock-reflection
                                                      'reflection 0.0)
                                    #:reflection-intensity
                                    (cfg 'dock-reflection-intensity
                                         'reflection-intensity 0.55)
                                    #:reflection-blur
                                    (cfg 'dock-reflection-blur
                                         'reflection-blur 0.05)
                                    #:shelf-flare
                                    (cfg 'dock-shelf-flare
                                         'shelf-flare 0)
                                    #:launchpad-icon-size
                                    (cfg 'dock-launchpad-icon-size
                                         'launchpad-icon-size 80)
                                    #:launchpad-columns
                                    (cfg 'dock-launchpad-columns
                                         'launchpad-columns 0)
                                    #:launchpad-blur?
                                    (cfg 'dock-launchpad-blur?
                                         'launchpad-blur? #f)
                                    #:launchpad-close-side
                                    (cfg 'dock-launchpad-close-side
                                         'launchpad-close-side 'left)
                                    #:reduce-motion?
                                    (cfg 'dock-reduce-motion?
                                         'reduce-motion? #f))))
               (lambda (key . args)
                 (log-error "Core"
                  "EXCEPTION while loading config [~a]: ~a | Details: ~s"
                  file-path key args))))
      (log-warn "Core" "No settings file found at path: ~a" file-path)))

(define (init-dock!)
  "Entry point invoked after C runtime initialization."
  (let ((config-file (get-config-path)))
    (when config-file
      (load-dock-config! config-file)))

  ;; Load fallback default dock items if no items were registered
  (when (null? (get-dock-items))
    (log-warn "Core"
              "No configured items found, loading default configuration...")
    (configure-dock! #:position 'bottom
                     #:icon-size 48
                     #:items (list (app-item #:name "Terminal"
                                             #:icon "Alacritty"
                                             #:exec "alacritty"))
                     #:auto-hide? #f)))

;; =============================================================================
;; REPL Management
;; =============================================================================

(define *active-repl-sockets*
  '())

(define* (start-repl! . args)
  (define (mkdir-p path)
    (unless (or (string-null? path)
                (file-exists? path))
      (let ((parent (dirname path)))
        (when (and (not (string=? parent path))
                   (not (string=? parent ".")))
          (mkdir-p parent))
        (catch 'system-error
               (lambda ()
                 (mkdir path))
               (lambda (key . args)
                 (unless (and (file-exists? path)
                              (eq? 'directory
                                   (stat:type (stat path))))
                   (apply throw key args)))))))

  (let* ((config-key (if (pair? args)
                         (car args)
                         'default))
         (socket-path (get-dock-repl-socket-path config-key))
         (enabled? (get-dock-enable-repl? config-key)))
    (when (and enabled?
               (not (member socket-path *active-repl-sockets*)))
      (set! *active-repl-sockets*
            (cons socket-path *active-repl-sockets*))
      (log-info "Core" "Starting interactive Guile REPL Unix socket at ~a..."
                socket-path)
      (call-with-new-thread (lambda ()
                              (catch #t
                                     (lambda ()
                                       (let ((dir (dirname socket-path)))
                                         (when (and (not (string-null? dir))
                                                    (not (file-exists? dir)))
                                           (mkdir-p dir)))
                                       (when (file-exists? socket-path)
                                         (delete-file socket-path))
                                       (let ((server-socket (make-unix-domain-server-socket
                                                             #:path
                                                             socket-path)))
                                         (spawn-server server-socket)))
                                     (lambda (key . args)
                                       (log-error "Core"
                                        "Failed to start REPL server on ~a (~a): ~a"
                                        socket-path key args))))))))
