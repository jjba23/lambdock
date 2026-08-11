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
  #:use-module (ice-9 time)
  #:use-module (ice-9 format)
  #:export (<dock-item> dock-item?
                        dock-item-type
                        dock-item-name
                        dock-item-icon
                        dock-item-exec
                        dock-item-poll-fn
                        dock-item-poll-interval
                        dock-item-hover-animate?

                        ;; Generic Constructors
                        app-item
                        separator-item
                        dynamic-item

                        ;; Dynamic Item Helpers & Presets
                        get-free-ram-mb
                        ram-item
                        get-clock-text
                        clock-item
                        get-cpu-loadavg
                        cpu-item
                        get-battery-status
                        battery-item
                        get-cpu-temp
                        cpu-temp-item))

;; =============================================================================
;; Data Structures
;; =============================================================================

;; Record type definition with accessors for polling logic and animation flags
(define-record-type <dock-item>
  (make-dock-item type
                  name
                  icon
                  exec
                  poll-fn
                  poll-interval
                  hover-animate?)
  dock-item?
  (type %dock-item-type)
  (name %dock-item-name)
  (icon %dock-item-icon)
  (exec %dock-item-exec)
  (poll-fn %dock-item-poll-fn)
  (poll-interval %dock-item-poll-interval)
  (hover-animate? %dock-item-hover-animate?))

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

(define (dock-item-hover-animate? item)
  (%dock-item-hover-animate? item))

;; Generic constructor for custom app launchers
(define* (app-item #:key (name "Unknown")
                   (icon "application-x-executable")
                   (exec "")
                   (hover-animate? #t))
  (make-dock-item 'app
                  name
                  icon
                  exec
                  #f
                  0
                  hover-animate?))

;; Generic constructor for separators
(define (separator-item)
  (make-dock-item 'separator
                  ""
                  ""
                  ""
                  #f
                  0
                  #f))

;; Generic constructor for dynamic polling items
(define* (dynamic-item #:key (name "Dynamic")
                       (icon "")
                       (exec "")
                       (poll-fn #f)
                       (poll-interval 2000)
                       (hover-animate? #t))
  (make-dock-item 'dynamic
                  name
                  icon
                  exec
                  poll-fn
                  poll-interval
                  hover-animate?))

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

(define* (ram-item #:key (exec "alacritty -e 'btop'")
                   (poll-interval 2000)
                   (hover-animate? #f))
  "Preset builder for dynamic RAM monitoring item."
  (dynamic-item #:name "Free RAM"
                #:icon #f
                #:exec exec
                #:poll-fn get-free-ram-mb
                #:poll-interval poll-interval
                #:hover-animate? hover-animate?))

(define (get-clock-text)
  (strftime "%a %b %d  %H:%M"
            (localtime (current-time))))

(define* (clock-item #:key (hover-animate? #f))
  (dynamic-item #:name "Clock"
                #:icon #f
                #:exec "gnome-calendar"
                #:poll-fn get-clock-text
                #:poll-interval 1000
                #:hover-animate? hover-animate?))

(define (get-cpu-loadavg)
  (catch #t
         (lambda ()
           (call-with-input-file "/proc/loadavg"
             (lambda (port)
               (let* ((line (read-line port))
                      (parts (string-split line #\space)))
                 (format #f "CPU ~a"
                         (car parts))))))
         (lambda _
           "CPU N/A")))

(define* (cpu-item #:key (hover-animate? #f))
  (dynamic-item #:name "CPU Load"
                #:icon #f
                #:exec "alacritty -e 'btop'"
                #:poll-fn get-cpu-loadavg
                #:poll-interval 2000
                #:hover-animate? hover-animate?))

(define (get-battery-status)
  (catch #t
         (lambda ()
           (let* ((cap-path "/sys/class/power_supply/BAT0/capacity")
                  (stat-path "/sys/class/power_supply/BAT0/status")
                  (cap (string-trim-both (call-with-input-file cap-path
                                           read-line)))
                  (stat (string-trim-both (call-with-input-file stat-path
                                            read-line)))
                  (charging? (string-ci=? stat "Charging")))
             (format #f "~a%~a" cap
                     (if charging? " ⚡" ""))))
         (lambda _
           "Bat N/A")))

(define* (battery-item #:key (hover-animate? #f))
  (dynamic-item #:name "Battery"
                #:icon "battery"
                #:exec "gnome-control-center power"
                #:poll-fn get-battery-status
                #:poll-interval 5000
                #:hover-animate? hover-animate?))

(define (get-cpu-temp)
  (catch #t
         (lambda ()
           (call-with-input-file "/sys/class/thermal/thermal_zone0/temp"
             (lambda (port)
               (let ((raw (string->number (string-trim-both (read-line port)))))
                 (if raw
                     (format #f "~a°C"
                             (round (/ raw 1000))) "Temp N/A")))))
         (lambda _
           "Temp N/A")))

(define* (cpu-temp-item #:key (hover-animate? #f))
  (dynamic-item #:name "CPU Temp"
                #:icon #f
                #:exec "alacritty -e 'btop'"
                #:poll-fn get-cpu-temp
                #:poll-interval 3000
                #:hover-animate? hover-animate?))
