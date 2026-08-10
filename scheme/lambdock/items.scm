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

(define-module (lambdock items)
  #:declarative? #t
  #:use-module (srfi srfi-9)
  #:use-module (ice-9 rdelim)
  #:use-module (ice-9 format)
  #:export (<dock-item> dock-item?
                        dock-item-type
                        dock-item-name
                        dock-item-icon
                        dock-item-exec
                        dock-item-poll-fn
                        dock-item-poll-interval

                        ;; Generic Constructors
                        app-item
                        separator-item
                        dynamic-item

                        ;; Dynamic Item Helpers & Presets
                        get-free-ram-mb
                        ram-item))

;; =============================================================================
;; Data Structures
;; =============================================================================

;; Record type definition with accessors for polling logic
(define-record-type <dock-item>
  (make-dock-item type
                  name
                  icon
                  exec
                  poll-fn
                  poll-interval)
  dock-item?
  (type %dock-item-type)
  (name %dock-item-name)
  (icon %dock-item-icon)
  (exec %dock-item-exec)
  (poll-fn %dock-item-poll-fn)
  (poll-interval %dock-item-poll-interval))

;; Public procedure wrappers
(define (dock-item-type item)
  (%dock-item-type item))

(define (dock-item-name item)
  (%dock-item-name item))

(define (dock-item-icon item)
  (%dock-item-icon item))

(define (dock-item-exec item)
  (%dock-item-exec item))

(define (dock-item-poll-fn item)
  (%dock-item-poll-fn item))

(define (dock-item-poll-interval item)
  (%dock-item-poll-interval item))

;; Generic constructor for custom app launchers
(define* (app-item #:key (name "Unknown")
                   (icon "application-x-executable")
                   (exec ""))
  (make-dock-item 'app
                  name
                  icon
                  exec
                  #f
                  0))

;; Generic constructor for separators
(define (separator-item)
  (make-dock-item 'separator
                  ""
                  ""
                  ""
                  #f
                  0))

;; Generic constructor for dynamic polling items
(define* (dynamic-item #:key (name "Dynamic")
                       (icon "")
                       (exec "")
                       (poll-fn #f)
                       (poll-interval 2000))
  (make-dock-item 'dynamic
                  name
                  icon
                  exec
                  poll-fn
                  poll-interval))

;; =============================================================================
;; Dynamic Helper Procedures & Presets
;; =============================================================================

(define (get-free-ram-mb)
  "Parses /proc/meminfo and returns available system RAM in MB."
  (catch #t
         (lambda ()
           (call-with-input-file "/proc/meminfo"
             (lambda (port)
               (let loop
                 ((line (read-line port)))
                 (cond
                   ((eof-object? line)
                    "RAM N/A")
                   ((string-prefix? "MemAvailable:" line)
                    (let* ((parts (string-split line #\space))
                           (nums (filter (lambda (s)
                                           (not (string-null? s))) parts)))
                      (if (>= (length nums) 2)
                          (let ((kb (string->number (list-ref nums 1))))
                            (if kb
                                (format #f "~a MB free"
                                        (round (/ kb 1024))) "RAM N/A"))
                          "RAM N/A")))
                   (else (loop (read-line port))))))))
         (lambda _
           "RAM N/A")))

(define* (ram-item #:key (icon "btop")
                   (exec "alacritty -e 'btop'")
                   (poll-interval 2000))
  "Preset builder for dynamic RAM monitoring item."
  (dynamic-item #:name "Free RAM"
                #:icon icon
                #:exec exec
                #:poll-fn get-free-ram-mb
                #:poll-interval poll-interval))
