;; lambdock -- hyper-hackable desktop dock for Wayland
;;
;; SPDX-License-Identifier: MIT
;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

(define-module (lambdock state)
  #:declarative? #f
  #:use-module (srfi srfi-9)
  #:use-module (ice-9 format)
  #:use-module (lambdock logger)
  #:use-module (lambdock layout)
  #:use-module (lambdock looks)
  #:export ( ;Configuration Entry Point
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
            set-dock-reduce-motion!))

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
                    reduce-motion?)
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
                  set-dock-config-reduce-motion?!))

;; Global hashtable storing isolated <dock-config> records keyed by config path / symbol
(define *dock-configs*
  (make-hash-table))

;; Fallback default record when key is missing or unregistered
(define %default-fallback-config%
  (make-dock-config 'bottom
                    48
                    4       ; item spacing (px between dock items)
                    12      ; capsule padding (px inside the capsule's ends)
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
                    0       ; shelf-tilt (degrees)
                    400     ; shelf-perspective (px)
                    24      ; shelf-margin (px, C's CAPSULE_PADDING_PX)
                    0       ; shelf-front (px)
                    0.0     ; reflection (band height / icon size)
                    0.55    ; reflection-intensity
                    0.05    ; reflection-blur (blur radius / icon size)
                    0       ; shelf-flare (keystone px per side)
                    80      ; launchpad-icon-size (px)
                    0       ; launchpad-columns (0 = fit the screen)
                    #f      ; launchpad-blur? (frosted backdrop)
                    'left   ; launchpad-close-side (the launchpad's X)
                    #f))    ; reduce-motion? (disable animations)

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
                          (reduce-motion? #f))
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
                               reduce-motion?)))
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

(define (get-dock-items . args)
  (let* ((key (if (null? args)
                  'default
                  (car args)))
         (cfg (get-dock-config key)))
    ;; Drag and drop edits (order, additions, removals, stack view modes) are
    ;; replayed on top of the configured items, so the settings file stays the
    ;; source of truth while the running dock remains freely rearrangeable.
    (apply-dock-overrides (dock-config-items cfg) key)))

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
;; Whole-dock recipes: (apply-look! 'osx-mavericks)
;; =============================================================================

(define (apply-look-setting! cfg key value)
  "Writes one (KEY . VALUE) of a look recipe into the config record CFG."
  (case key
    ((theme dock-theme) (set-dock-config-theme! cfg value))
    ((icon-size dock-icon-size) (set-dock-config-icon-size! cfg value))
    ((item-spacing dock-item-spacing)
     (set-dock-config-item-spacing! cfg value))
    ((capsule-padding dock-capsule-padding)
     (set-dock-config-capsule-padding! cfg value))
    ((mode dock-mode) (set-dock-config-mode! cfg value))
    ((show-window-titles? dock-show-window-titles?)
     (set-dock-config-show-window-titles?! cfg value))
    ((hover-animate? dock-hover-animate?)
     (set-dock-config-hover-animate?! cfg value))
    ((launchpad-blur? dock-launchpad-blur?)
     (set-dock-config-launchpad-blur?! cfg value))
    ((shelf-tilt dock-shelf-tilt) (set-dock-config-shelf-tilt! cfg value))
    ((shelf-perspective dock-shelf-perspective)
     (set-dock-config-shelf-perspective! cfg value))
    ((shelf-margin dock-shelf-margin) (set-dock-config-shelf-margin! cfg value))
    ((shelf-front dock-shelf-front) (set-dock-config-shelf-front! cfg value))
    ((shelf-flare dock-shelf-flare) (set-dock-config-shelf-flare! cfg value))
    ((reflection dock-reflection) (set-dock-config-reflection! cfg value))
    ((reflection-intensity dock-reflection-intensity)
     (set-dock-config-reflection-intensity! cfg value))
    ((reflection-blur dock-reflection-blur)
     (set-dock-config-reflection-blur! cfg value))
    (else
     (log-warn "State" "Look setting '~a' has no runtime setter; ignored" key))))

(define (apply-look! look . args)
  "Applies the whole-dock LOOK (a recipe name, alias, or string) to the running
dock and redraws it once.

With no further argument every dock is restyled; pass a settings path or a
config key to restyle just that one:

  (apply-look! 'osx-mavericks)
  (apply-look! 'osx-tahoe-dark \"/home/me/.config/lambdock/settings-2.scm\")

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
