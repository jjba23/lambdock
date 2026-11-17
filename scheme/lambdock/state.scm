;; lambdock -- hyper-hackable desktop dock for Wayland
;;
;; SPDX-License-Identifier: MIT
;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

(define-module (lambdock state)
  #:declarative? #f
  #:use-module (srfi srfi-9)
  #:use-module (ice-9 format)
  #:use-module (lambdock logger)
  #:use-module (lambdock loader)
  #:use-module (lambdock layout)
  #:use-module (lambdock looks)
  #:use-module (lambdock items)
  #:export (;Configuration Entry Point
            configure-dock!
            <dock-config>
            dock-config?
            get-dock-config

            ;; Whole-dock recipes (see (lambdock looks))
            apply-look!

            ;; Getters
            get-dock-items
            get-dock-position
            get-dock-theme
            get-dock-icon-size
            get-dock-item-spacing
            get-dock-capsule-padding
            get-dock-auto-hide?
            get-dock-monitor
            get-dock-exclude-apps
            get-dock-enable-repl?
            get-dock-repl-socket-path
            get-dock-show-when-clear?
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
            get-dock-mirror-of
            get-dock-mirror-exclude
            get-dock-mirror-kinds

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

            ;; Capturing the resolved configuration as settings.scm
            dock-settings-scm
            export-dock-settings!))

;; =============================================================================
;; Dock Configuration Record
;; =============================================================================

(define-record-type <dock-config>
  (make-dock-config position
                    icon-size
                    item-spacing
                    capsule-padding
                    items
                    auto-hide?
                    show-when-clear?
                    theme
                    monitor
                    exclude-apps
                    enable-repl?
                    repl-socket-path
                    hover-animate?
                    track-windows?
                    mode
                    show-window-titles?
                    shelf-tilt
                    shelf-perspective
                    shelf-margin
                    shelf-front
                    reflection
                    reflection-intensity
                    reflection-blur
                    shelf-flare
                    launchpad-icon-size
                    launchpad-columns
                    launchpad-blur?
                    launchpad-close-side
                    reduce-motion?
                    mirror-of
                    mirror-exclude
                    mirror-kinds)
  dock-config?
  (position dock-config-position set-dock-config-position!)
  (icon-size dock-config-icon-size set-dock-config-icon-size!)
  (item-spacing dock-config-item-spacing set-dock-config-item-spacing!)
  (capsule-padding dock-config-capsule-padding
                   set-dock-config-capsule-padding!)
  (items dock-config-items set-dock-config-items!)
  (auto-hide? dock-config-auto-hide? set-dock-config-auto-hide?!)
  (show-when-clear? dock-config-show-when-clear?
                    set-dock-config-show-when-clear?!)
  (theme dock-config-theme set-dock-config-theme!)
  (monitor dock-config-monitor set-dock-config-monitor!)
  (exclude-apps dock-config-exclude-apps set-dock-config-exclude-apps!)
  (enable-repl? dock-config-enable-repl? set-dock-config-enable-repl?!)
  (repl-socket-path dock-config-repl-socket-path
                    set-dock-config-repl-socket-path!)
  (hover-animate? dock-config-hover-animate? set-dock-config-hover-animate?!)
  (track-windows? dock-config-track-windows? set-dock-config-track-windows?!)
  (mode dock-config-mode set-dock-config-mode!)
  (show-window-titles? dock-config-show-window-titles?
                       set-dock-config-show-window-titles?!)
  (shelf-tilt dock-config-shelf-tilt set-dock-config-shelf-tilt!)
  (shelf-perspective dock-config-shelf-perspective
                     set-dock-config-shelf-perspective!)
  (shelf-margin dock-config-shelf-margin set-dock-config-shelf-margin!)
  (shelf-front dock-config-shelf-front set-dock-config-shelf-front!)
  (reflection dock-config-reflection set-dock-config-reflection!)
  (reflection-intensity dock-config-reflection-intensity
                        set-dock-config-reflection-intensity!)
  (reflection-blur dock-config-reflection-blur
                   set-dock-config-reflection-blur!)
  (shelf-flare dock-config-shelf-flare set-dock-config-shelf-flare!)
  (launchpad-icon-size dock-config-launchpad-icon-size
                       set-dock-config-launchpad-icon-size!)
  (launchpad-columns dock-config-launchpad-columns
                     set-dock-config-launchpad-columns!)
  (launchpad-blur? dock-config-launchpad-blur?
                   set-dock-config-launchpad-blur?!)
  (launchpad-close-side dock-config-launchpad-close-side
                        set-dock-config-launchpad-close-side!)
  (reduce-motion? dock-config-reduce-motion?
                  set-dock-config-reduce-motion?!)
  ;; A mirror dock shows another dock's items, filtered: the same value read
  ;; twice, so one settings file stays the single source of the shelf.
  (mirror-of dock-config-mirror-of set-dock-config-mirror-of!)
  (mirror-exclude dock-config-mirror-exclude set-dock-config-mirror-exclude!)
  (mirror-kinds dock-config-mirror-kinds set-dock-config-mirror-kinds!))

;; Global hashtable storing isolated <dock-config> records keyed by config path / symbol
(define *dock-configs*
  (make-hash-table))

;; Fallback default record when key is missing or unregistered
(define %default-fallback-config%
  (make-dock-config 'bottom
                    48
                    4 ; item spacing (px between dock items)
                    12 ; capsule padding (px inside the capsule's ends)
                    '()
                    #f
                    #t
                    'vanilla
                    'default
                    '()
                    #f
                    "/tmp/lambdock-repl.sock"
                    #t
                    #t
                    'dock
                    #f
                    0 ; shelf-tilt (degrees)
                    400 ; shelf-perspective (px)
                    24 ; shelf-margin (px, C's CAPSULE_PADDING_PX)
                    0 ; shelf-front (px)
                    0.0 ; reflection (band height / icon size)
                    0.55 ; reflection-intensity
                    0.05 ; reflection-blur (blur radius / icon size)
                    0 ; shelf-flare (keystone px per side)
                    80 ; launchpad-icon-size (px)
                    0 ; launchpad-columns (0 = fit the screen)
                    #f ; launchpad-blur? (frosted backdrop)
                    'left ; launchpad-close-side (the launchpad's X)
                    #f ; reduce-motion? (disable animations)
                    #f ; mirror-of (no source: an ordinary dock)
                    '() ; mirror-exclude (item keys to drop)
                    '())) ; mirror-kinds (item types to keep)

;; =============================================================================
;; Configuration Interface
;; =============================================================================

(define (normalize-launchpad-close-side value)
  "Returns 'right when VALUE asks for the launchpad close button on the right,
and 'left otherwise. The launchpad has exactly two sides to put it on, so
anything else (an old symbol, a typo, a string) is the documented default."
  (if (eq? value 'right) 'right 'left))

;; === FILE: state.scm (Excerpt: configure-dock!) ===
(define* (configure-dock! #:key (config-path 'default)
                          (position 'bottom)
                          (icon-size 48)
                          (item-spacing 4)
                          (capsule-padding 12)
                          (items '())
                          (auto-hide? #f)
                          (show-when-clear? #t)
                          (theme 'vanilla)
                          (monitor 'default)
                          (exclude-apps '())
                          (enable-repl? #f)
                          (repl-socket-path "/tmp/lambdock-repl.sock")
                          (hover-animate? #t)
                          (track-windows? #t)
                          (mode 'dock)
                          (show-window-titles? #f)
                          (shelf-tilt 0)
                          (shelf-perspective 400)
                          (shelf-margin 24)
                          (shelf-front 0)
                          (reflection 0.0)
                          (reflection-intensity 0.55)
                          (reflection-blur 0.05)
                          (shelf-flare 0)
                          (launchpad-icon-size 80)
                          (launchpad-columns 0)
                          (launchpad-blur? #f)
                          (launchpad-close-side 'left)
                          (reduce-motion? #f)
                          (mirror-of #f)
                          (mirror-exclude '())
                          (mirror-kinds '()))
  "Sets and registers configuration parameters for a specific dock instance key or file path."
  (let ((cfg (make-dock-config position
                               icon-size
                               item-spacing
                               capsule-padding
                               items
                               auto-hide?
                               show-when-clear?
                               theme
                               monitor
                               exclude-apps
                               enable-repl?
                               repl-socket-path
                               hover-animate?
                               track-windows?
                               mode
                               show-window-titles?
                               shelf-tilt
                               shelf-perspective
                               shelf-margin
                               shelf-front
                               reflection
                               reflection-intensity
                               reflection-blur
                               shelf-flare
                               launchpad-icon-size
                               launchpad-columns
                               launchpad-blur?
                               (normalize-launchpad-close-side
                                launchpad-close-side)
                               reduce-motion?
                               mirror-of
                               mirror-exclude
                               mirror-kinds)))
    ;; Store under exact file path / symbol key
    (hash-set! *dock-configs* config-path cfg)

    ;; ALWAYS update 'default key if this is the primary settings file or initial load
    (when (or (eq? config-path
                   'default)
            (string-suffix? "settings.scm"
                            (format #f "~a" config-path)))
      (hash-set! *dock-configs*
                 'default cfg))

    (log-info "State"
              "Registered config for [~a]: ~a items, pos: ~a, theme: ~a, auto-hide?: ~a, repl?: ~a (~a), track-windows?: ~a, mode: ~a, icon ~a/spacing ~a/padding ~a (shelf: tilt ~a, margin ~a, front ~a, reflection ~a, flare ~a)"
              config-path
              (if (list? items)
                (length items) 0)
              position
              theme
              auto-hide?
              enable-repl?
              repl-socket-path
              track-windows?
              mode
              icon-size
              item-spacing
              capsule-padding
              shelf-tilt
              shelf-margin
              shelf-front
              reflection
              shelf-flare)))

(define (get-dock-config key)
  (or (hash-ref *dock-configs* key #f)
    (hash-ref *dock-configs*
              'default #f) %default-fallback-config%))

;; =============================================================================
;; Getters
;; =============================================================================

;; --- Mirrors ----------------------------------------------------------------
;; A mirror dock is a second dock that shows another dock's item list, filtered.
;; It is deliberately not a copy: the source is read whenever the items are
;; asked for, so a change in one settings file shows on both docks.

(define (dock-name-stem value)
  "The settings-file stem a mirror source names: a path, a file name or a symbol."
  (let* ((raw (cond [(string? value) value]
                    [(symbol? value) (symbol->string value)]
                    [else #f]))
         (base (and raw (basename raw))))
    (and base
      (if (string-suffix? ".scm" base)
        (substring base 0 (- (string-length base) 4))
        base))))

(define (dock-key-for-name name)
  "The *dock-configs* key NAME refers to, or #f.

A mirror can name its source with the full path the source was loaded from, or
with just its file name (\"settings-games.scm\") -- the latter is what a
settings file that something else generates can know without hardcoding a home
directory."
  (let ((stem (dock-name-stem name)))
    (and stem
      (or (and (hash-ref *dock-configs* name #f) name)
        (hash-fold (lambda (key _ found)
                     (or found
                       (and (equal? stem (dock-name-stem key)) key)))
                   #f *dock-configs*)))))

(define (%mirror-keeps? item exclude kinds)
  "Whether a mirror keeps ITEM: not excluded by key and of a wanted kind."
  (and (not (member (dock-item-key item) exclude))
    (or (null? kinds) (memq (dock-item-type item) kinds))))

(define (%mirror-items source own-key exclude kinds)
  (let ((source-key (dock-key-for-name source)))
    (cond
      [(not source-key)
       (log-warn "State"
                 "Mirror dock [~a] names unknown source ~s: it shows no items"
                 own-key source)
       '()]
      [(equal? source-key own-key)
       (log-warn "State"
                 "Mirror dock [~a] names itself as its source: it shows no items"
                 own-key)
       '()]
      [else
        (filter (lambda (item)
                  (%mirror-keeps? item exclude kinds))
                (get-dock-items source-key))])))

(define (get-dock-items . args)
  (let* ((key (if (null? args)
                'default
                (car args)))
         (cfg (get-dock-config key))
         (mirror-of (dock-config-mirror-of cfg)))
    (if mirror-of
      (%mirror-items mirror-of key
                     (dock-config-mirror-exclude cfg)
                     (dock-config-mirror-kinds cfg))
      ;; Drag and drop edits (order, additions, removals, stack view modes) are
      ;; replayed on top of the configured items, so the settings file stays
      ;; the source of truth while the running dock remains freely
      ;; rearrangeable.
      (apply-dock-overrides (dock-config-items cfg) key))))

(define (get-dock-position . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-position cfg)))

(define (get-dock-theme . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-theme cfg)))

(define (get-dock-icon-size . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-icon-size cfg)))

(define (get-dock-item-spacing . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-item-spacing cfg)))

(define (get-dock-capsule-padding . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-capsule-padding cfg)))

(define (get-dock-auto-hide? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-auto-hide? cfg)))

(define (get-dock-show-when-clear? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-show-when-clear? cfg)))

(define (get-dock-monitor . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-monitor cfg)))

(define (get-dock-exclude-apps . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-exclude-apps cfg)))

(define (get-dock-enable-repl? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-enable-repl? cfg)))

(define (get-dock-repl-socket-path . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-repl-socket-path cfg)))

(define (get-dock-hover-animate? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-hover-animate? cfg)))

(define (get-dock-track-windows? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-track-windows? cfg)))

(define (get-dock-mode . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-mode cfg)))

(define (get-dock-show-window-titles? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-show-window-titles? cfg)))

;; --- Shelf geometry / reflection parameters ---------------------------------
;; The dock has no modes: these numbers are its whole shape. The defaults
;; describe the classic flat capsule.

(define (get-dock-shelf-tilt . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-shelf-tilt cfg)))

(define (get-dock-shelf-perspective . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-shelf-perspective cfg)))

(define (get-dock-shelf-margin . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-shelf-margin cfg)))

(define (get-dock-shelf-front . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-shelf-front cfg)))

(define (get-dock-reflection . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-reflection cfg)))

(define (get-dock-reflection-intensity . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-reflection-intensity cfg)))

(define (get-dock-reflection-blur . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-reflection-blur cfg)))

(define (get-dock-shelf-flare . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-shelf-flare cfg)))

;; --- Launchpad ---------------------------------------------------------------
;; The launchpad is one screen for the whole dock, but its appearance follows
;; the settings of the dock instance that opened it.

(define (get-dock-launchpad-icon-size . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-launchpad-icon-size cfg)))

(define (get-dock-launchpad-columns . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-launchpad-columns cfg)))

(define (get-dock-launchpad-blur? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-launchpad-blur? cfg)))

(define (get-dock-launchpad-close-side . args)
  "Returns 'left (the default) or 'right: the end of the launchpad's header the
close button sits on."
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-launchpad-close-side cfg)))

;; --- Accessibility -----------------------------------------------------------
;; Motion is reduced when this is on, and also when the desktop itself has
;; animations turned off (the C side checks both).

(define (get-dock-reduce-motion? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                'default
                                (car args)))))
    (dock-config-reduce-motion? cfg)))

;; --- Mirrors ---------------------------------------------------------------

(define (get-dock-mirror-of . args)
  (dock-config-mirror-of (get-dock-config (if (null? args)
                                            'default
                                            (car args)))))

(define (get-dock-mirror-exclude . args)
  (dock-config-mirror-exclude (get-dock-config (if (null? args)
                                                 'default
                                                 (car args)))))

(define (get-dock-mirror-kinds . args)
  (dock-config-mirror-kinds (get-dock-config (if (null? args)
                                               'default
                                               (car args)))))

;; =============================================================================
;; Setters
;; =============================================================================

(define (trigger-ui-reload!)
  "Helper to invoke C reload-dock! procedure if available."
  (catch #t
         (lambda ()
           (let ((mod (resolve-module '(lambdock core)
                                      #:ensure #f)))
             (when mod
               (let ((proc (module-ref mod
                                       'reload-dock! #f)))
                 (when (and proc
                         (procedure? proc))
                   (proc))))))
         (lambda _
           #f)))

(define (apply-to-targets proc args)
  (if (null? args)
    (hash-for-each (lambda (k cfg)
                     (proc cfg)) *dock-configs*)
    (let ((cfg (get-dock-config (car args))))
      (when cfg
        (proc cfg))))
  (trigger-ui-reload!))

(define (set-dock-position! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-position! cfg val)) args))

(define (set-dock-theme! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-theme! cfg val)) args))

(define (set-dock-icon-size! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-icon-size! cfg val)) args))

(define (set-dock-item-spacing! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-item-spacing! cfg val)) args))

(define (set-dock-capsule-padding! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-capsule-padding! cfg val)) args))

(define (set-dock-auto-hide! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-auto-hide?! cfg val)) args))

(define (set-dock-show-when-clear! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-show-when-clear?! cfg val)) args))

(define (set-dock-monitor! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-monitor! cfg val)) args))

(define (set-dock-hover-animate! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-hover-animate?! cfg val)) args))

(define (set-dock-track-windows! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-track-windows?! cfg val)) args))

(define (set-dock-mode! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-mode! cfg val)) args))

(define (set-dock-show-window-titles! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-show-window-titles?! cfg val)) args))

;; --- Shelf geometry / reflection setters ------------------------------------

(define (set-dock-shelf-tilt! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-shelf-tilt! cfg val)) args))

(define (set-dock-shelf-perspective! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-shelf-perspective! cfg val)) args))

(define (set-dock-shelf-margin! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-shelf-margin! cfg val)) args))

(define (set-dock-shelf-front! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-shelf-front! cfg val)) args))

(define (set-dock-reflection! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-reflection! cfg val)) args))

(define (set-dock-reflection-intensity! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-reflection-intensity! cfg val)) args))

(define (set-dock-reflection-blur! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-reflection-blur! cfg val)) args))

(define (set-dock-shelf-flare! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-shelf-flare! cfg val)) args))

;; --- Launchpad ---------------------------------------------------------------

(define (set-dock-launchpad-icon-size! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-launchpad-icon-size! cfg val)) args))

(define (set-dock-launchpad-columns! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-launchpad-columns! cfg val)) args))

(define (set-dock-launchpad-blur! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-launchpad-blur?! cfg val)) args))

(define (set-dock-launchpad-close-side! val . args)
  (let ((side (normalize-launchpad-close-side val)))
    (apply-to-targets (lambda (cfg)
                        (set-dock-config-launchpad-close-side! cfg side))
                      args)))

;; --- Accessibility -----------------------------------------------------------

(define (set-dock-reduce-motion! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-reduce-motion?! cfg val)) args))

;; =============================================================================
;; Whole-dock recipes: (apply-look! 'platinum)
;; =============================================================================

(define (apply-look-setting! cfg key value)
  "Writes one (KEY . VALUE) of a look recipe into the config record CFG."
  (case key
    [(theme dock-theme) (set-dock-config-theme! cfg value)]
    [(icon-size dock-icon-size) (set-dock-config-icon-size! cfg value)]
    [(item-spacing dock-item-spacing)
     (set-dock-config-item-spacing! cfg value)]
    [(capsule-padding dock-capsule-padding)
     (set-dock-config-capsule-padding! cfg value)]
    [(mode dock-mode) (set-dock-config-mode! cfg value)]
    [(show-window-titles? dock-show-window-titles?)
     (set-dock-config-show-window-titles?! cfg value)]
    [(hover-animate? dock-hover-animate?)
     (set-dock-config-hover-animate?! cfg value)]
    [(launchpad-blur? dock-launchpad-blur?)
     (set-dock-config-launchpad-blur?! cfg value)]
    [(shelf-tilt dock-shelf-tilt) (set-dock-config-shelf-tilt! cfg value)]
    [(shelf-perspective dock-shelf-perspective)
     (set-dock-config-shelf-perspective! cfg value)]
    [(shelf-margin dock-shelf-margin) (set-dock-config-shelf-margin! cfg value)]
    [(shelf-front dock-shelf-front) (set-dock-config-shelf-front! cfg value)]
    [(shelf-flare dock-shelf-flare) (set-dock-config-shelf-flare! cfg value)]
    [(reflection dock-reflection) (set-dock-config-reflection! cfg value)]
    [(reflection-intensity dock-reflection-intensity)
     (set-dock-config-reflection-intensity! cfg value)]
    [(reflection-blur dock-reflection-blur)
     (set-dock-config-reflection-blur! cfg value)]
    [else
      (log-warn "State" "Look setting '~a' has no runtime setter; ignored" key)]))

(define (apply-look! look . args)
  "Applies the whole-dock LOOK (a recipe name, alias, or string) to the running
dock and redraws it once.

With no further argument every dock is restyled; pass a settings path or a
config key to restyle just that one:

  (apply-look! 'platinum)
  (apply-look! 'glass-dark \"/home/me/.config/lambdock/settings-2.scm\")

The recipe writes the theme, the icon size and spacing, the capsule padding plus
every shelf and reflection number -- including the ones the look leaves at their
defaults -- so no geometry is inherited from the look that was there before.
Only the running dock changes; write the look into settings.scm to keep it."
  (let ((settings (preset-look look)))
    (if (not (pair? settings))
      (begin
        (log-warn "State" "Unknown look '~a': nothing applied" look)
        #f)
      (begin
        (apply-to-targets
         (lambda (cfg)
           (for-each (lambda (pair)
                       (apply-look-setting! cfg (car pair) (cdr pair)))
                     settings))
         args)
        (log-info "State" "Applied the '~a' look to ~a (~a settings)"
                  (look-name look)
                  (if (null? args) "every dock" (car args))
                  (length settings))
        #t))))

;; =============================================================================
;; Capturing the resolved dock as settings.scm
;; =============================================================================
;; The dock on screen is the fixed point of three inputs: the settings file, the
;; GUI preferences and the drag-and-drop layout. This is the inverse of that
;; loader: a settings file that would produce the dock running now. It makes a
;; bug report reproducible and answers "what is my dock actually doing" without
;; a REPL or a log hunt.

(define (settings-value value)
  "VALUE written as a settings-file expression."
  (cond [(string? value) (format #f "~s" value)]
        [(symbol? value) (format #f "'~a" value)]
        [(boolean? value) (if value "#t" "#f")]
        [(null? value) "'()"]
        [(or (number? value) (pair? value)) (format #f "'~s" value)]
        [else (format #f "~s" value)]))

(define (settings-hover item)
  "The #:hover-animate? argument of ITEM, or \"\" when it is the default #t."
  (if (dock-item-hover-animate? item)
    ""
    " #:hover-animate? #f"))

;; A preset is recognised by rebuilding it from the item's own fields and
;; comparing the two values: that way a changed field falls back to the generic
;; dynamic item instead of silently exporting the preset's default.
(define (settings-same-item? a b)
  (and (eq? (dock-item-type a) (dock-item-type b))
    (equal? (dock-item-name a) (dock-item-name b))
    (equal? (dock-item-icon a) (dock-item-icon b))
    (equal? (dock-item-exec a) (dock-item-exec b))
    (eq? (dock-item-poll-fn a) (dock-item-poll-fn b))
    (= (dock-item-poll-interval a) (dock-item-poll-interval b))
    (eq? (dock-item-hover-animate? a) (dock-item-hover-animate? b))
    (eq? (dock-item-meter? a) (dock-item-meter? b))
    (eq? (dock-item-clock? a) (dock-item-clock? b))
    (equal? (dock-item-unit a) (dock-item-unit b))
    (= (dock-item-max a) (dock-item-max b))))

(define (settings-preset-text item)
  "The preset constructor that rebuilds ITEM, or #f for a custom poller."
  (let ((hover (dock-item-hover-animate? item))
        (meter? (dock-item-meter? item))
        (interval (dock-item-poll-interval item)))
    (cond
      [(settings-same-item? item (clock-item #:hover-animate? hover))
       (format #f "(clock-item~a)" (settings-hover item))]
      [(settings-same-item? item (ram-item #:poll-interval interval
                                           #:hover-animate? hover
                                           #:meter? meter?))
       (format #f "(ram-item #:poll-interval ~a~a~a)"
               interval (settings-hover item)
               (if meter? "" " #:meter? #f"))]
      [(settings-same-item? item (cpu-item #:hover-animate? hover
                                           #:meter? meter?))
       (format #f "(cpu-item~a~a)"
               (settings-hover item)
               (if meter? "" " #:meter? #f"))]
      [(settings-same-item? item (cpu-temp-item #:hover-animate? hover
                                                #:meter? meter?))
       (format #f "(cpu-temp-item~a~a)"
               (settings-hover item)
               (if meter? "" " #:meter? #f"))]
      [(settings-same-item? item (battery-item #:hover-animate? hover))
       (format #f "(battery-item~a)" (settings-hover item))]
      [else #f])))

(define (settings-dynamic-text item)
  "A dynamic item as an expression, with its poller named in a comment."
  (or (settings-preset-text item)
    (format #f
            ";; poll-fn: ~a -- a Scheme procedure cannot be written out; redefine it here.
  (dynamic-item #:name ~s #:icon ~s #:exec ~s #:poll-interval ~a~a~a~a~a)"
            (dock-item-poll-name item)
            (dock-item-name item) (dock-item-icon item) (dock-item-exec item)
            (dock-item-poll-interval item)
            (settings-hover item)
            (if (dock-item-clock? item) " #:clock? #t" "")
            (if (dock-item-meter? item) " #:meter? #t" "")
            (if (dock-item-meter? item)
              (format #f " #:unit ~s #:max ~a"
                      (dock-item-unit item) (dock-item-max item))
              ""))))

(define (settings-item-text item)
  "ITEM as the settings-file expression that builds it."
  (case (dock-item-type item)
    [(app)
     (format #f "(app-item #:name ~s #:icon ~s #:exec ~s~a)"
             (dock-item-name item) (dock-item-icon item) (dock-item-exec item)
             (settings-hover item))]
    [(separator)
     ;; The style is written back when it is not the default, so an exported
     ;; settings file draws the same separators the dock is drawing now.
     (let ((style (dock-item-style item)))
       (if (eq? style 'line)
         "(separator-item)"
         (format #f "(separator-item #:style '~a)" style)))]
    [(stack)
     (format #f "(stack-item #:name ~s #:icon ~s #:path ~s #:view-mode '~a~a)"
             (dock-item-name item) (dock-item-icon item) (dock-item-path item)
             (dock-item-view-mode item) (settings-hover item))]
    [(launchpad)
     (format #f "(launchpad-item #:name ~s #:icon ~s~a)"
             (dock-item-name item) (dock-item-icon item)
             (settings-hover item))]
    [(trash)
     (format #f "(trash-item #:name ~s #:icon ~s~a)"
             (dock-item-name item) (dock-item-icon item)
             (settings-hover item))]
    [(dynamic) (settings-dynamic-text item)]
    [else (format #f ";; unknown item ~s" (dock-item-key item))]))

(define (dock-settings-scm config-path)
  "The resolved configuration of CONFIG-PATH, as settings.scm text.

The text is what the loader would read back into the dock that is running now:
the settings file, the GUI preferences layered on top of it and the layout
overrides are already baked in, and a mirror dock exports its mirror setting
rather than the resolved copies of another dock's items."
  (let* ((cfg (get-dock-config config-path))
         (mirror (dock-config-mirror-of cfg)))
    (with-output-to-string
     (lambda ()
       (format #t
               ";; lambdock settings, exported from the running dock~%")
       (format #t ";; Source dock: ~a~%" config-path)
       (format #t
               ";; Resolved configuration: the settings file, the GUI preferences~%")
       (format #t
               ";; and the drag-and-drop layout, as values.  Save it as settings.scm~%")
       (format #t ";; to make the dock on screen the dock it starts from.~%~%")
       (format #t "(define dock-position ~a)~%"
               (settings-value (dock-config-position cfg)))
       (format #t "(define dock-theme ~a)~%"
               (settings-value (dock-config-theme cfg)))
       (format #t "(define dock-icon-size ~a)~%"
               (dock-config-icon-size cfg))
       (format #t "(define dock-item-spacing ~a)~%"
               (dock-config-item-spacing cfg))
       (format #t "(define dock-capsule-padding ~a)~%"
               (dock-config-capsule-padding cfg))
       (format #t "(define dock-monitor ~a)~%"
               (settings-value (dock-config-monitor cfg)))
       (format #t "(define dock-mode ~a)~%"
               (settings-value (dock-config-mode cfg)))
       (format #t "(define dock-auto-hide? ~a)~%"
               (if (dock-config-auto-hide? cfg) "#t" "#f"))
       (format #t "(define dock-show-when-clear? ~a)~%"
               (if (dock-config-show-when-clear? cfg) "#t" "#f"))
       (format #t "(define dock-hover-animate? ~a)~%"
               (if (dock-config-hover-animate? cfg) "#t" "#f"))
       (format #t "(define dock-track-windows? ~a)~%"
               (if (dock-config-track-windows? cfg) "#t" "#f"))
       (format #t "(define dock-show-window-titles? ~a)~%"
               (if (dock-config-show-window-titles? cfg) "#t" "#f"))
       (format #t "(define dock-exclude-apps ~a)~%"
               (settings-value (dock-config-exclude-apps cfg)))
       (format #t "(define dock-enable-repl? ~a)~%"
               (if (dock-config-enable-repl? cfg) "#t" "#f"))
       (format #t "(define dock-repl-socket-path ~a)~%"
               (settings-value (dock-config-repl-socket-path cfg)))
       (format #t "(define dock-reduce-motion? ~a)~%"
               (if (dock-config-reduce-motion? cfg) "#t" "#f"))
       (format #t "(define dock-shelf-tilt ~a)~%"
               (dock-config-shelf-tilt cfg))
       (format #t "(define dock-shelf-perspective ~a)~%"
               (dock-config-shelf-perspective cfg))
       (format #t "(define dock-shelf-margin ~a)~%"
               (dock-config-shelf-margin cfg))
       (format #t "(define dock-shelf-front ~a)~%"
               (dock-config-shelf-front cfg))
       (format #t "(define dock-shelf-flare ~a)~%"
               (dock-config-shelf-flare cfg))
       (format #t "(define dock-reflection ~a)~%"
               (dock-config-reflection cfg))
       (format #t "(define dock-reflection-intensity ~a)~%"
               (dock-config-reflection-intensity cfg))
       (format #t "(define dock-reflection-blur ~a)~%"
               (dock-config-reflection-blur cfg))
       (format #t "(define dock-launchpad-icon-size ~a)~%"
               (dock-config-launchpad-icon-size cfg))
       (format #t "(define dock-launchpad-columns ~a)~%"
               (dock-config-launchpad-columns cfg))
       (format #t "(define dock-launchpad-blur? ~a)~%"
               (if (dock-config-launchpad-blur? cfg) "#t" "#f"))
       (format #t "(define dock-launchpad-close-side ~a)~%"
               (settings-value (dock-config-launchpad-close-side cfg)))

       (if mirror
         (begin
           (format #t "~%;; A mirror: the item list is read from this dock.~%")
           (format #t "(define dock-mirror-of ~a)~%"
                   (settings-value mirror))
           (format #t "(define dock-mirror-exclude ~a)~%"
                   (settings-value (dock-config-mirror-exclude cfg)))
           (format #t "(define dock-mirror-kinds ~a)~%"
                   (settings-value (dock-config-mirror-kinds cfg))))
         (begin
           (format #t "~%(define dock-items (list~%")
           (for-each (lambda (item)
                       (format #t "  ~a~%" (settings-item-text item)))
                     (get-dock-items config-path))
           (format #t "))~%")))))))

(define* (export-dock-settings! config-path #:key target)
  "Writes the resolved dock of CONFIG-PATH as settings.scm text.

The file goes to TARGET, or to
${XDG_STATE_HOME:-~/.local/state}/lambdock/exported-<settings-stem>.scm -- a name
the dock's own settings*.scm scan does not pick up, so exporting cannot spawn a
second dock on the next start.  Returns the path written, or #f.

The exported file is meant to be read, diffed and copied into place: it is what
the running dock resolves, not what its settings file happens to spell."
  (let ((path (or target (dock-state-file config-path "exported-"))))
    (catch #t
           (lambda ()
             (ensure-directory (dirname path))
             (call-with-output-file path
                                    (lambda (port)
                                      (display (dock-settings-scm config-path) port)))
             (log-info "State" "Exported the resolved dock to ~a" path)
             path)
           (lambda (key . args)
             (log-error "State" "Could not export the dock to ~a (~a ~s)" path
                        key args)
             #f))))
