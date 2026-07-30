(define-module (lambdock core)
  #:use-module (srfi srfi-9)
  #:use-module (ice-9 format)
  #:export (<dock-item> dock-item?
                        dock-item-type
                        dock-item-name
                        dock-item-icon
                        dock-item-exec

                        ;; Generic Constructors
                        app-item
                        separator-item

                        ;; Specialized App Helper Constructors
                        terminal-item
                        browser-item
                        files-item
                        editor-item
                        settings-item
                        audio-item
                        sysmon-item

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

;; Internal record type definition with private accessor names
(define-record-type <dock-item>
  (make-dock-item type name icon exec)
  dock-item?
  (type %dock-item-type)
  (name %dock-item-name)
  (icon %dock-item-icon)
  (exec %dock-item-exec))

;; Public procedure wrappers (guarantees C-API compatibility via scm_call_1)
(define (dock-item-type item)
  (%dock-item-type item))
(define (dock-item-name item)
  (%dock-item-name item))
(define (dock-item-icon item)
  (%dock-item-icon item))
(define (dock-item-exec item)
  (%dock-item-exec item))

;; Generic constructor for custom app launchers
(define* (app-item #:key (name "Unknown")
                   (icon "application-x-executable")
                   (exec ""))
  (make-dock-item 'app name icon exec))

;; Generic constructor for separators
(define (separator-item)
  (make-dock-item 'separator "" "" ""))

;; =============================================================================
;; Specialized Helper Constructors (Freedesktop Standard Icons)
;; =============================================================================

(define* (terminal-item #:key (name "Terminal")
                        (icon "utilities-terminal")
                        (exec "alacritty"))
  "Constructs a terminal launcher."
  (app-item #:name name
            #:icon icon
            #:exec exec))

(define* (browser-item #:key (name "Web Browser")
                       (icon "web-browser")
                       (exec "google-chrome"))
  "Constructs a web browser launcher."
  (app-item #:name name
            #:icon icon
            #:exec exec))

(define* (files-item #:key (name "Files")
                     (icon "system-file-manager")
                     (exec "nautilus -w"))
  "Constructs a file manager launcher."
  (app-item #:name name
            #:icon icon
            #:exec exec))

(define* (editor-item #:key (name "Text Editor")
                      (icon "accessories-text-editor")
                      (exec "emacsclient -c"))
  "Constructs a text editor launcher."
  (app-item #:name name
            #:icon icon
            #:exec exec))

(define* (settings-item #:key (name "Settings")
                        (icon "preferences-system")
                        (exec "gnome-control-center"))
  "Constructs a system settings launcher."
  (app-item #:name name
            #:icon icon
            #:exec exec))

(define* (audio-item #:key (name "Volume Control")
                     (icon "multimedia-volume-control")
                     (exec "pavucontrol"))
  "Constructs an audio volume mixer launcher."
  (app-item #:name name
            #:icon icon
            #:exec exec))

(define* (sysmon-item #:key (name "System Monitor")
                      (icon "utilities-system-monitor")
                      (exec "gnome-system-monitor"))
  "Constructs a system monitor launcher."
  (app-item #:name name
            #:icon icon
            #:exec exec))

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
;; Configuration Interface
;; =============================================================================

(define* (configure-dock! #:key (position 'bottom)
                          (icon-size 48)
                          (items '()))
  "Sets the primary configuration parameters for the dock."
  (set! *dock-position* position)
  (set! *icon-size* icon-size)
  (set! *dock-items* items)
  (format #t
   "[Scheme] Configuration loaded: ~a items registered at size ~apx.~%"
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
  (format #t "[Scheme] Core dock module initialized.~%")

  ;; Load rich default dock items if none are configured
  (when (null? *dock-items*)
    (format #t "[Scheme] No config found, loading default items...~%")
    (configure-dock! #:position 'bottom
                     #:icon-size 48
                     #:items (list (terminal-item)
                                   (browser-item)
                                   (files-item)
                                   (editor-item)
                                   (separator-item)
                                   (audio-item)
                                   (settings-item)
                                   (sysmon-item))))
  *dock-items*)
