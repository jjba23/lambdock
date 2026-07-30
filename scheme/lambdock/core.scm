(define-module (lambddock core)
  #:use-module (srfi srfi-9)
  #:use-module (ice-9 format)
  #:export (
             <dock-item>
            dock-item?
            dock-item-type
            dock-item-name
            dock-item-icon
            dock-item-exec

            app-item
            separator-item

            ;; State & Configuration
            configure-dock!
            get-dock-items
            get-dock-position
            get-icon-size

            ;; Core Initialization
            init-dock!))

;; =============================================================================
;; Data Structures
;; =============================================================================

;; Defines a record type for dock elements
(define-record-type <dock-item>
  (make-dock-item type name icon exec)
  dock-item?
  (type dock-item-type) ;Symbol: 'app or 'separator
  (name dock-item-name) ;String: Display name (e.g., "Terminal")
  (icon dock-item-icon) ;String: Icon theme name (e.g., "utilities-terminal")
  (exec dock-item-exec))
 ; String: Shell command to spawn (e.g., "foot")

;; Helper constructor for app launchers
(define* (app-item #:key (name "Unknown")
                   (icon "application-x-executable")
                   (exec ""))
  (make-dock-item 'app name icon exec))

;; Helper constructor for separators
(define (separator-item)
  (make-dock-item 'separator "" "" ""))

;; =============================================================================
;; Application State
;; =============================================================================

(define *dock-items*
  '())
(define *dock-position*
  'bottom)
(define *icon-size*
  48)

;; =============================================================================
;; Configuration Interface (Called by user's config.scm)
;; =============================================================================

(define* (configure-dock! #:key (position 'bottom)
                          (icon-size 48)
                          (items '()))
  "Sets the primary configuration parameters for the dock."
  (set! *dock-position* position)
  (set! *icon-size* icon-size)
  (set! *dock-items* items)
  (format #t "[Scheme] Configuration loaded: ~a items registered at size ~apx.
"
          (length *dock-items*) *icon-size*))

(define (get-dock-items)
  *dock-items*)
(define (get-dock-position)
  *dock-position*)
(define (get-icon-size)
  *icon-size*)

;; =============================================================================
;; Core Hooks (Called by main.c)
;; =============================================================================

(define (init-dock!)
  "Entry point invoked after C runtime initialization."
  (format #t "[Scheme] Core dock module initialized.\n")

  ;; Load default fallback items if none are configured
  (when (null? *dock-items*)
    (format #t "[Scheme] No config found, loading default items...
")
    (configure-dock! #:position 'bottom
                     #:icon-size 48
                     #:items (list (app-item #:name "Terminal"
                                             #:icon "utilities-terminal"
                                             #:exec "foot")
                                   (separator-item)
                                   (app-item #:name "Files"
                                             #:icon "system-file-manager"
                                             #:exec "thunar"))))
  *dock-items*)
