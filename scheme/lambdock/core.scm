;; lambdock

;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

;; lambdock is free software : you can redistribute it and / or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; lambdock is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with lambdock.If not, see <https://www.gnu.org/licenses/>.

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
;; Config File Resolution
;; =============================================================================

(define (get-config-path)
  "Resolves the user configuration path based on XDG guidelines."
  (let ((xdg-config (getenv "XDG_CONFIG_HOME"))
        (home (getenv "HOME")))
    (cond
     ((and xdg-config (not (string-null? xdg-config)))
      (string-append xdg-config "/lambdock/settings.scm"))
     (home
      (string-append home "/.config/lambdock/settings.scm"))
     (else #f))))

;; =============================================================================
;; Core Hooks (Called by main.c)
;; =============================================================================

(define (init-dock!)
  "Entry point invoked after C runtime initialization."
  (format #t "[Scheme] Core dock module initialized.~%")

  ;; 1. Attempt to load the user's configuration file
  (let ((config-file (get-config-path)))
    (if (and config-file (file-exists? config-file))
        (begin
          (format #t "[Scheme] Loading user settings from: ~a~%" config-file)
          (catch #t
            (lambda ()
              (load config-file))
            (lambda (key . args)
              (format #t "[Scheme] ERROR loading ~a: ~a ~a~%" config-file key args))))
        (format #t "[Scheme] No settings file found at: ~a~%" config-file)))

  ;; 2. Load fallback default dock items if no items were registered
  (when (null? *dock-items*)
    (format #t "[Scheme] No configured items found, loading default configuration...~%")
    (configure-dock! #:position 'bottom
                     #:icon-size 48
                     #:items (list (terminal-item))))
  *dock-items*)
