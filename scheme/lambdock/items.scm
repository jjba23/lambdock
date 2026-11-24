;;; lambdock -- hyper-hackable desktop dock for Wayland
;;;
;;; SPDX-License-Identifier: MIT
;;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

;;; Commentary:
;;;
;;; The dock's items, as one record with a constructor per kind.  A settings
;;; file names what it wants -- an application, a separator, a stack, the
;;; launchpad, the trash -- and each constructor fills in the same
;;; `<dock-item>' record, so the Rust side reads one shape whatever the item
;;; is.  An item that changes while the dock runs carries a poll procedure and
;;; the interval it is called at; what it answers is drawn as text, as the
;;; gauge of a meter, or as the tooltip of the analogue clock face.
;;;
;;; ```mermaid
;;; flowchart LR
;;;   settings["Settings file: (app-item ...), (stack-item ...), presets"] --> ctor["Constructors: app-item, separator-item, dynamic-item, stack-item, launchpad-item, trash-item"]
;;;   ctor --> record["dock-item record"]
;;;   record --> bridge["The Rust bridge reads the accessors"]
;;;   bridge --> timer["GTK timer at dock-item-poll-interval"]
;;;   timer --> poll["dock-item-poll-fn answers a reading or a string"]
;;;   poll --> ui["Icon, gauge, clock face or tooltip"]
;;; ```
;;;
;;; Code:

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
                        dock-item-poll-name
                        dock-item-style

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
                        get-battery-value
                        battery-item
                        get-cpu-temp
                        get-cpu-temp-value
                        cpu-temp-item
                        get-uptime-value
                        get-uptime-text
                        format-uptime
                        uptime-item
                        get-load-value
                        get-load-text
                        load-item
                        get-swap-value
                        get-swap-percent
                        swap-item))

;; =============================================================================
;; Data Structures
;; =============================================================================

;; Record type definition with accessors for polling logic and animation flags.
;;
;; `path` and `view-mode` are only meaningful for stacks: a stack is a dock icon
;; that shows the contents of a directory (like the desktop's Downloads stack), and
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
;;
;; `style` is how a separator is drawn: 'line (the theme's plain divider, the
;; default), 'groove (an engraved pair of lines), 'pill (a short rounded bar),
;; 'dot (a single bead) or 'gap (empty room, no line at all). Every other kind of
;; item keeps 'line; only the dock's separator reads it.
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
                  max
                  style)
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
  (max %dock-item-max)
  (style %dock-item-style))

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

(define (dock-item-style item)
  "How a separator is drawn: 'line, 'groove, 'pill, 'dot or 'gap.

Only separators read it; every other kind answers 'line."
  (%dock-item-style item))

(define (dock-item-key item)
  "Stable identifier of ITEM, used by the dock layout overrides to remember
ordering, removals and additions across restarts. It matches the key the C side
computes, so both halves of the dock agree on the identity of an item."
  (format #f "~a:~a" (dock-item-type item) (dock-item-name item)))

(define (dock-item-poll-name item)
  "The name of the procedure that polls ITEM, or \"\" when it does not poll.

The \"why is this item here\" inspector shows this: a dynamic item's behaviour is
the thunk it carries, and a thunk in a settings file has no name to read back
unless it was defined with `define'."
  (let ((fn (%dock-item-poll-fn item)))
    (if (procedure? fn)
      (let ((name (catch #t
                         (lambda ()
                           (procedure-name fn))
                         (lambda _
                           #f))))
        (cond [(symbol? name) (symbol->string name)]
              [(string? name) name]
              [else "anonymous procedure"]))
      "")))

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
                  100
                  'line))

;; The separator styles the stylesheets know. A typo is the plain divider rather
;; than a selector nothing paints.
(define %separator-styles '(line groove pill dot gap))

(define (normalize-separator-style style)
  "STYLE when it names one of the separator styles, 'line otherwise."
  (if (memq style %separator-styles) style 'line))

;; Generic constructor for separators.
;;
;; `style` says how the divider is drawn, and every theme answers with its own
;; material:
;;
;;   'line    the theme's plain divider (the default)
;;   'groove  an engraved pair: a dark line with a lit one beside it
;;   'pill    a short rounded bar in the theme's divider colour
;;   'dot     a single round bead, centred in the row
;;   'gap     empty room: a group break with no line at all
(define* (separator-item #:key (style 'line))
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
                  100
                  (normalize-separator-style style)))

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
                  max
                  'line))

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
                  100
                  'line))

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
                  100
                  'line))

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
                  100
                  'line))

;; =============================================================================
;; Dynamic Helper Procedures & Presets
;; =============================================================================

(define (read-first-line path)
  "The first line of PATH, trimmed, or #f when it cannot be read.

The readings below are files the kernel publishes; a missing one is \"no
reading\" rather than an error, so every caller answers #f and the item says
N/A."
  (catch #t
         (lambda ()
           (string-trim-both (call-with-input-file path read-line)))
         (lambda _
           #f)))

(define (meminfo-kb field)
  "FIELD's value in /proc/meminfo, in kilobytes, or #f.

FIELD is written the way the kernel writes it, colon included
(\"MemTotal:\", \"SwapFree:\")."
  (catch #t
         (lambda ()
           (call-with-input-file "/proc/meminfo"
                                 (lambda (port)
                                   (let loop
                                     ((line (read-line port)))
                                     (cond
                                       [(eof-object? line)
                                        #f]
                                       [(string-prefix? field line)
                                        ;; The kernel pads its columns with
                                        ;; spaces, so the empty words between them
                                        ;; are not fields.
                                        (let ((parts (filter (lambda (part)
                                                               (not (string-null? part)))
                                                             (string-split line #\space))))
                                          (and (>= (length parts) 2)
                                            (string->number (list-ref parts 1))))]
                                       [else (loop (read-line port))])))))
         (lambda _
           #f)))

(define (used-percent total free)
  "How much of TOTAL is taken, as a whole percentage, or #f."
  (and total free
    (> total 0)
    (inexact->exact (round (* (/ (- total free) total) 100)))))

(define (get-used-ram-value)
  "Used system RAM as a percentage (0-100), or #f.

This is the number a meter item wants; `get-used-ram-percent' is the same
reading formatted for a text item."
  (used-percent (meminfo-kb "MemTotal:")
                (meminfo-kb "MemAvailable:")))

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

;; The reading a clock item shows: the face's tooltip and what a screen reader
;; announces. A settings file can ask for its own wording with `#:format'.
(define %clock-format "%a %b %d  %H:%M")

(define (get-clock-text)
  (strftime %clock-format (localtime (current-time))))

(define (clock-text-in time-format)
  "The system time as TIME-FORMAT (a strftime template), in the user's locale."
  (strftime time-format (localtime (current-time))))

;; `#:format' is the natural name for a strftime template, and it shadows
;; `(ice-9 format)'s procedure inside this body -- which nothing below needs.
(define* (clock-item #:key (format %clock-format)
                     (exec "gnome-calendar")
                     (hover-animate? #f))
  "The clock: an analogue face the dock points at the system time once a second.

The poll function is still what names the reading -- its string is the face's
tooltip and what a screen reader announces -- while the hands themselves are set
from the system clock.

`#:format' is that reading's strftime template, so a clock can say
\"%H:%M\" or \"%A %d\" instead of the default; `#:exec' is what a click opens."
  (dynamic-item #:name "Clock"
                #:icon #f
                #:exec exec
                #:poll-fn (if (string=? format %clock-format)
                            get-clock-text
                            (lambda ()
                              (clock-text-in format)))
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
                                       [(eof-object? line)
                                        #f]
                                       [(string-prefix? "cpu " line)
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
                                              (cons (+ idle iowait) total)) #f))]
                                       [else (loop (read-line port))])))))
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

;; --- Battery -----------------------------------------------------------------
;; `BAT0` is the usual name of the first battery. A machine whose battery is
;; called something else (or which has two) asks for it with `#:device', and
;; `#:meter? #t' draws the charge as a gauge instead of as text.

(define %battery-device "BAT0")

(define (battery-file device name)
  "The sysfs file NAME of power supply DEVICE."
  (format #f "/sys/class/power_supply/~a/~a" device name))

(define (battery-value-for device)
  "DEVICE's charge in percent (0-100), or #f."
  (let ((raw (read-first-line (battery-file device "capacity"))))
    (and raw
      (let ((number (string->number raw)))
        (and number (inexact->exact (round number)))))))

(define (get-battery-value)
  "The first battery's charge in percent (0-100), or #f.

This is the number a meter item wants: `(battery-item #:meter? #t)`."
  (battery-value-for %battery-device))

(define (battery-text-for device)
  "DEVICE's charge as \"88% ⚡\" while it charges, or \"Bat N/A\"."
  (let ((value (battery-value-for device))
        (status (read-first-line (battery-file device "status"))))
    (if value (format #f "~a%~a" value
                      (if (and status (string-ci=? status "Charging")) " ⚡" ""))
      "Bat N/A")))

(define (get-battery-status)
  "The first battery as \"88% ⚡\" while it charges, for text items."
  (battery-text-for %battery-device))

(define (battery-poller device meter?)
  "The poll procedure that reads DEVICE, as a gauge or as text.

The default device keeps the named readers, so an exported settings file writes
the preset back as `(battery-item)` instead of a lambda nobody can read."
  (cond [(and meter? (string=? device %battery-device)) get-battery-value]
        [(string=? device %battery-device) get-battery-status]
        [meter? (lambda ()
                  (battery-value-for device))]
        [else (lambda ()
                (battery-text-for device))]))

(define* (battery-item #:key (device %battery-device)
                       (hover-animate? #f)
                       (meter? #f)
                       (poll-interval 5000))
  "Preset builder for a battery item.

`#:device' is the power supply to read (\"BAT0\" by default, \"BAT1\" or
\"CMB0\" on a machine that names it differently), `#:meter? #t' draws the charge
as a gauge that empties as the battery runs out, and `#:poll-interval' is how
often it is read."
  (dynamic-item #:name "Battery"
                #:icon "battery"
                #:exec "gnome-control-center power"
                #:poll-fn (battery-poller device meter?)
                #:poll-interval poll-interval
                #:hover-animate? hover-animate?
                #:meter? meter?
                #:unit "%"
                #:max 100))

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

;; --- Swap, uptime and load average -------------------------------------------
;; Three more readings the kernel publishes as text: how much of the swap file is
;; in use, how long the machine has been up, and how busy it is. None of them
;; needs a helper program, and none of them blocks the main loop.

(define (get-swap-value)
  "Used swap as a percentage (0-100), or #f on a machine with no swap."
  (used-percent (meminfo-kb "SwapTotal:") (meminfo-kb "SwapFree:")))

(define (get-swap-percent)
  "Used swap as \"Swap 12%\", for text items."
  (let ((value (get-swap-value)))
    (if value (format #f "Swap ~a%" value) "Swap N/A")))

(define* (swap-item #:key (poll-interval 5000)
                    (hover-animate? #f)
                    (meter? #t))
  "Preset builder for the swap item.

By default it is a gauge of the swap file's fill, which is the reading that
explains a machine that has started to crawl; `#:meter? #f' gives a text item.
A machine with no swap has nothing to show, so the item says \"Swap N/A\"."
  (dynamic-item #:name "Swap"
                #:icon #f
                #:exec "alacritty -e 'btop'"
                #:poll-fn (if meter? get-swap-value get-swap-percent)
                #:poll-interval poll-interval
                #:hover-animate? hover-animate?
                #:meter? meter?
                #:unit "%"
                #:max 100))

(define (get-uptime-value)
  "Seconds since boot, from /proc/uptime, or #f."
  (let ((line (read-first-line "/proc/uptime")))
    (and line
      (let ((seconds (string->number (car (string-split line #\space)))))
        (and seconds (inexact->exact (floor seconds)))))))

(define (format-uptime seconds)
  "SECONDS as \"3d 4h\", \"4h 12m\" or \"12m\": the two units that matter."
  (let ((days (quotient seconds 86400))
        (hours (quotient (remainder seconds 86400) 3600))
        (minutes (quotient (remainder seconds 3600) 60)))
    (cond [(> days 0) (format #f "~ad ~ah" days hours)]
          [(> hours 0) (format #f "~ah ~am" hours minutes)]
          [else (format #f "~am" minutes)])))

(define (get-uptime-text)
  "How long the machine has been up, as \"up 4h 12m\"."
  (let ((value (get-uptime-value)))
    (if value (format #f "up ~a" (format-uptime value)) "up N/A")))

(define* (uptime-item #:key (poll-interval 60000)
                      (hover-animate? #f))
  "Preset builder for the uptime item: \"up 3d 4h\" as a text item.

It needs no gauge -- uptime has no ceiling to fill -- so the item is always text,
and once a minute is often enough."
  (dynamic-item #:name "Uptime"
                #:icon #f
                #:exec ""
                #:poll-fn get-uptime-text
                #:poll-interval poll-interval
                #:hover-animate? hover-animate?))

(define (get-load-value)
  "The one minute load average from /proc/loadavg, or #f.

It is a count of runnable and uninterruptible processes, not a percentage, so
compare it with the number of cores: a gauge item wants `#:max' set to that
number."
  (let ((line (read-first-line "/proc/loadavg")))
    (and line (string->number (car (string-split line #\space))))))

(define (get-load-text)
  "The one minute load average as \"Load 0.42\"."
  (let ((value (get-load-value)))
    (if value (format #f "Load ~,2f" value) "Load N/A")))

(define* (load-item #:key (poll-interval 5000)
                    (hover-animate? #f)
                    (meter? #t)
                    (max 8))
  "Preset builder for the load average item.

By default it is a gauge that fills against `#:max' -- set that to the number of
cores for a reading of \"how busy is this machine\" -- and `#:meter? #f' gives
the text item."
  (dynamic-item #:name "Load"
                #:icon #f
                #:exec "alacritty -e 'btop'"
                #:poll-fn (if meter? get-load-value get-load-text)
                #:poll-interval poll-interval
                #:hover-animate? hover-animate?
                #:meter? meter?
                #:unit ""
                #:max max))
