;; lambdock -- hyper-hackable desktop dock for Wayland
;;
;; SPDX-License-Identifier: MIT
;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

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
                        dock-item-path
                        dock-item-view-mode
                        dock-item-meter?
                        dock-item-clock?
                        dock-item-unit
                        dock-item-max
                        dock-item-key

                        ;; Generic Constructors
                        app-item
                        separator-item
                        dynamic-item
                        stack-item
                        launchpad-item
                        trash-item

                        ;; Dynamic Item Helpers & Presets
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
                        cpu-temp-item))

;; =============================================================================
;; Data Structures
;; =============================================================================

;; Record type definition with accessors for polling logic and animation flags.
;;
;; `path` and `view-mode` are only meaningful for stacks: a stack is a dock icon
;; that shows the contents of a directory (like the macOS Downloads stack), and
;; `view-mode` picks how those contents are laid out ('grid or 'list).
;; Every other item keeps them at "" / 'grid.
;;
;; `meter?`, `unit` and `max` describe dynamic items that are drawn as a gauge
;; instead of as text: the poll function then returns a *number*, the gauge fills
;; up to `max` and the reading is shown with `unit` appended ("%" or "°C").
;;
;; `clock?` marks the item that is drawn as an analogue clock face. Its poll
;; function still runs -- the string it returns is the face's tooltip and the text
;; a screen reader announces -- while the hands follow the system clock.
(define-record-type <dock-item>
  (make-dock-item type
                  name
                  icon
                  exec
                  poll-fn
                  poll-interval
                  hover-animate?
                  path
                  view-mode
                  meter?
                  clock?
                  unit
                  max)
  dock-item?
  (type %dock-item-type)
  (name %dock-item-name)
  (icon %dock-item-icon)
  (exec %dock-item-exec)
  (poll-fn %dock-item-poll-fn)
  (poll-interval %dock-item-poll-interval)
  (hover-animate? %dock-item-hover-animate?)
  (path %dock-item-path)
  (view-mode %dock-item-view-mode)
  (meter? %dock-item-meter?)
  (clock? %dock-item-clock?)
  (unit %dock-item-unit)
  (max %dock-item-max))

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

(define (dock-item-path item)
  (%dock-item-path item))

(define (dock-item-view-mode item)
  (%dock-item-view-mode item))

(define (dock-item-meter? item)
  (%dock-item-meter? item))

(define (dock-item-clock? item)
  (%dock-item-clock? item))

(define (dock-item-unit item)
  (%dock-item-unit item))

(define (dock-item-max item)
  (%dock-item-max item))

(define (dock-item-key item)
  "Stable identifier of ITEM, used by the dock layout overrides to remember
ordering, removals and additions across restarts. It matches the key the C side
computes, so both halves of the dock agree on the identity of an item."
  (format #f "~a:~a" (dock-item-type item) (dock-item-name item)))

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
                  hover-animate?
                  ""
                  'grid
                  #f
                  #f
                  ""
                  100))

;; Generic constructor for separators
(define (separator-item)
  (make-dock-item 'separator
                  ""
                  ""
                  ""
                  #f
                  0
                  #f
                  ""
                  'grid
                  #f
                  #f
                  ""
                  100))

;; Generic constructor for dynamic polling items.
;;
;; With `#:meter? #t` the item is drawn as a gauge instead of as text: the poll
;; function must then return a number, which fills the ring up to `#:max` and is
;; written in the middle with `#:unit` appended.
(define* (dynamic-item #:key (name "Dynamic")
                       (icon "")
                       (exec "")
                       (poll-fn #f)
                       (poll-interval 2000)
                       (hover-animate? #t)
                       (meter? #f)
                       (clock? #f)
                       (unit "")
                       (max 100))
  (make-dock-item 'dynamic
                  name
                  icon
                  exec
                  poll-fn
                  poll-interval
                  hover-animate?
                  ""
                  'grid
                  meter?
                  clock?
                  unit
                  max))

;; A stack: a dock icon that opens a popover with the contents of a directory.
;; `view-mode` is one of 'grid or 'list and can be changed at runtime from
;; the item's context menu.
(define* (stack-item #:key (name "Stack")
                     (icon "folder")
                     (path "")
                     (view-mode 'grid)
                     (hover-animate? #t))
  (make-dock-item 'stack
                  name
                  icon
                  ""
                  #f
                  0
                  hover-animate?
                  path
                  (if (memq view-mode '(grid list))
                      view-mode
                      'grid)
                  #f
                  #f
                  ""
                  100))

;; The launchpad: clicking it opens a full screen grid of installed apps.
(define* (launchpad-item #:key (name "Launchpad")
                         (icon "view-app-grid-symbolic")
                         (hover-animate? #t))
  (make-dock-item 'launchpad
                  name
                  icon
                  ""
                  #f
                  0
                  hover-animate?
                  ""
                  'grid
                  #f
                  #f
                  ""
                  100))

;; The trash: a dock icon that follows the desktop trash. It shows whether there
;; is anything in it, opens it on a click, and its context menu empties it. `icon`
;; is the artwork shown while the trash is empty; the full state uses the icon
;; theme's `user-trash-full' (or `user-trash-full-symbolic' for a symbolic icon).
(define* (trash-item #:key (name "Trash")
                     (icon "user-trash")
                     (hover-animate? #t))
  (make-dock-item 'trash
                  name
                  icon
                  ""
                  #f
                  0
                  hover-animate?
                  ""
                  'grid
                  #f
                  #f
                  ""
                  100))

;; =============================================================================
;; Dynamic Helper Procedures & Presets
;; =============================================================================

(define (get-used-ram-value)
  "Parses /proc/meminfo and returns used system RAM as a percentage (0-100).

This is the number a meter item wants; `get-used-ram-percent' is the same
reading formatted for a text item."
  (catch #t
         (lambda ()
           (call-with-input-file "/proc/meminfo"
             (lambda (port)
               (define (parse-kb line)
                 (let* ((parts (string-split line #\space))
                        (nums (filter (lambda (s)
                                        (not (string-null? s))) parts)))
                   (and (>= (length nums) 2)
                        (string->number (list-ref nums 1)))))
               (let loop
                 ((line (read-line port))
                  (mem-total #f)
                  (mem-available #f))
                 (cond
                   ((and mem-total mem-available)
                    (let* ((used (- mem-total mem-available))
                           (pct (inexact->exact (round (* (/ used mem-total)
                                                          100)))))
                      pct))
                   ((eof-object? line)
                    #f)
                   ((string-prefix? "MemTotal:" line)
                    (loop (read-line port)
                          (parse-kb line) mem-available))
                   ((string-prefix? "MemAvailable:" line)
                    (loop (read-line port) mem-total
                          (parse-kb line)))
                   (else (loop (read-line port) mem-total mem-available)))))))
         (lambda _
           #f)))

(define (get-used-ram-percent)
  "Used system RAM as a RAM 42% reading, for text items."
  (let ((value (get-used-ram-value)))
    (if value (format #f "RAM ~a%" value) "RAM N/A")))

(define* (ram-item #:key (exec "alacritty -e 'btop'")
                   (poll-interval 2000)
                   (hover-animate? #f)
                   (meter? #t))
  "Preset builder for the RAM item.

By default it is a gauge: a ring that fills with the used RAM and turns amber
and then red as memory runs out. `#:meter? #f' gives the old text item."
  (dynamic-item #:name "RAM Usage"
                #:icon #f
                #:exec exec
                #:poll-fn (if meter? get-used-ram-value get-used-ram-percent)
                #:poll-interval poll-interval
                #:hover-animate? hover-animate?
                #:meter? meter?
                #:unit "%"
                #:max 100))

(define (get-clock-text)
  (strftime "%a %b %d  %H:%M"
            (localtime (current-time))))

(define* (clock-item #:key (hover-animate? #f))
  "The clock: an analogue face the dock points at the system time once a second.

The poll function is still what names the reading -- its string is the face's
tooltip and what a screen reader announces -- while the hands themselves are set
from the system clock."
  (dynamic-item #:name "Clock"
                #:icon #f
                #:exec "gnome-calendar"
                #:poll-fn get-clock-text
                #:poll-interval 1000
                #:hover-animate? hover-animate?
                #:clock? #t))

;; CPU sampling state. Poll functions run on the GTK main thread, so they must
;; never block. An earlier implementation slept 100ms inside this procedure to
;; measure a delta, freezing the whole dock; instead we keep the previous
;; /proc/stat counters and compute usage between consecutive poll invocations.
(define %cpu-last-sample
  (vector #f))

(define (read-cpu-counters)
  "Reads the aggregate 'cpu' line from /proc/stat as (idle+iowait . total)."
  (catch #t
         (lambda ()
           (call-with-input-file "/proc/stat"
             (lambda (port)
               (let loop
                 ((line (read-line port)))
                 (cond
                   ((eof-object? line)
                    #f)
                   ((string-prefix? "cpu " line)
                    (let* ((parts (string-split line #\space))
                           (nums (map string->number
                                      (filter (lambda (s)
                                                (not (string-null? s)))
                                              (cdr parts)))))
                      (if (>= (length nums) 4)
                          (let ((idle (list-ref nums 3))
                                (iowait (if (>= (length nums) 5)
                                            (list-ref nums 4) 0))
                                (total (apply + nums)))
                            (cons (+ idle iowait) total)) #f)))
                   (else (loop (read-line port))))))))
         (lambda _
           #f)))

(define (get-cpu-value)
  "CPU usage percentage since the previous poll (non-blocking), or #f.

This is the number a meter item wants; `get-cpu-percent' wraps it as text."
  (let ((sample (read-cpu-counters))
        (previous (vector-ref %cpu-last-sample 0)))
    (vector-set! %cpu-last-sample 0 sample)
    (if (and sample previous)
        (let* ((idle1 (car previous))
               (total1 (cdr previous))
               (idle2 (car sample))
               (total2 (cdr sample))
               (diff-idle (- idle2 idle1))
               (diff-total (- total2 total1)))
          (if (> diff-total 0)
              (inexact->exact (round (* 100 (- 1 (/ diff-idle diff-total)))))
              #f))
        #f)))

(define (get-cpu-percent)
  "CPU usage as \"CPU 42%\", for text items."
  (let ((value (get-cpu-value)))
    (if value (format #f "CPU ~a%" value) "CPU N/A")))

(define* (cpu-item #:key (hover-animate? #f)
                   (meter? #t))
  "Preset builder for the CPU usage item.

By default it is a gauge; `#:meter? #f' gives the old text item."
  (dynamic-item #:name "CPU Usage"
                #:icon #f
                #:exec "alacritty -e 'btop'"
                #:poll-fn (if meter? get-cpu-value get-cpu-percent)
                #:poll-interval 2000
                #:hover-animate? hover-animate?
                #:meter? meter?
                #:unit "%"
                #:max 100))

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

(define (get-cpu-temp-value)
  "CPU temperature in degrees Celsius from the first thermal zone, or #f."
  (catch #t
         (lambda ()
           (call-with-input-file "/sys/class/thermal/thermal_zone0/temp"
             (lambda (port)
               (let ((raw (string->number (string-trim-both (read-line port)))))
                 (and raw (round (/ raw 1000)))))))
         (lambda _
           #f)))

(define (get-cpu-temp)
  "CPU temperature as \"54°C\", for text items."
  (let ((value (get-cpu-temp-value)))
    (if value (format #f "~a°C" value) "Temp N/A")))

(define* (cpu-temp-item #:key (hover-animate? #f)
                        (meter? #t))
  "Preset builder for the CPU temperature item.

By default it is a gauge whose ring fills from 0 to 100 °C; `#:meter? #f' gives
the old text item."
  (dynamic-item #:name "CPU Temp"
                #:icon #f
                #:exec "alacritty -e 'btop'"
                #:poll-fn (if meter? get-cpu-temp-value get-cpu-temp)
                #:poll-interval 3000
                #:hover-animate? hover-animate?
                #:meter? meter?
                #:unit "°C"
                #:max 100))
