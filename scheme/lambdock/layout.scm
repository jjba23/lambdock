;; lambdock -- hyper-hackable desktop dock for Wayland
;;
;; SPDX-License-Identifier: MIT
;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

;; =============================================================================
;; Dock layout overrides
;; =============================================================================
;;
;; Your settings file is the source of truth for the dock. Drag and drop edits
;; the running dock, though, and those edits must survive restarts *without*
;; having to rewrite (or fight with) a settings file that other tools may
;; regenerate. So they live in a small override file next to the dock's state:
;;
;;   ${XDG_STATE_HOME:-~/.local/state}/lambdock/layout-<settings-name>.scm
;;
;; It records four independent things:
;;
;;   (order   "app:Firefox" ...)        <- left to right order of the items
;;   (removed "app:Files" ...)          <- items the user dragged out
;;   (added   ("app" "Name" ...) ...)   <- items the user dragged in
;;   (views   ("stack:Downloads" . list))<- per stack view mode
;;
;; The overrides are applied on top of whatever the settings file says, so a
;; changed settings file keeps working: new items appear, removed ones stay
;; removed, and nothing is lost. Deleting the file resets the dock layout.
;;
;; The `added' descriptors are (kind name icon argument view) tuples. Kind is
;; "app", "stack", "launchpad" or "preset": the last one names one of the dock's
;; ready-made widgets -- ("preset" "trash" "" "" "") is the trash can, "clock"
;; the analog clock, "ram" the memory gauge, and so on (see (lambdock presets)).
;; A preset descriptor's key is the key of the item it builds, so removing and
;; reordering work the same way for a widget dragged in, dropped in or added from
;; the Preferences window.
;;
;; Two rules matter when debugging "my layout was not applied":
;;
;;   * the file is re-read whenever the dock asks for its items (there is no
;;     cache), so an edit, a second instance or a restart all see the same data;
;;   * `order' only reorders. An item that the settings file no longer defines
;;     cannot be restored from `order' alone -- only the `added' section carries
;;     the icon and command an item needs -- so such keys are skipped with a
;;     warning instead of silently doing nothing.
;; =============================================================================

(define-module (lambdock layout)
  #:declarative? #f
  #:use-module (ice-9 format)
  #:use-module (srfi srfi-1)
  #:use-module (lambdock items)
  #:use-module (lambdock loader)
  #:use-module (lambdock logger)
  #:use-module (lambdock presets)
  #:export (dock-layout-path
            dock-layout-ref
            dock-layout-reset!
            apply-dock-overrides
            dock-layout-add!
            dock-layout-remove!
            dock-layout-set-order!
            dock-layout-set-view!))

;; =============================================================================
;; State directory & file location
;; =============================================================================

(define (dock-state-dir)
  "Directory where lambdock keeps runtime state (the layout overrides)."
  (let ((xdg (getenv "XDG_STATE_HOME"))
        (home (getenv "HOME")))
    (cond ((and xdg (not (string-null? xdg)))
           (string-append xdg "/lambdock"))
          (home (string-append home "/.local/state/lambdock"))
          (else "/tmp/lambdock"))))

(define (dock-layout-path config-path)
  "Path of the layout override file belonging to CONFIG-PATH.

Each settings file gets its own override file, named after it, so several docks
never share their layout."
  (let* ((raw (cond ((string? config-path) config-path)
                    ((symbol? config-path) (symbol->string config-path))
                    (else "settings.scm")))
         (base (basename raw))
         (stem (if (string-suffix? ".scm" base)
                   (substring base 0 (- (string-length base) 4))
                   base)))
    (string-append (dock-state-dir) "/layout-" stem ".scm")))

;; =============================================================================
;; Reading & writing the override file
;; =============================================================================

;; There is deliberately no cache of the parsed layout: the file is a few
;; hundred bytes and is read whenever the dock asks for its items, so a layout
;; written by the Preferences window, by another dock instance or by hand is
;; never masked by a copy parsed at startup. Mutations carry the layout they just
;; built and write it directly (see %write-layout!), so they never have to read
;; it back either.

(define (dock-layout-target config-path)
  "The settings path whose override file belongs to CONFIG-PATH.

A dock created without a settings path -- the fallback dock, or a REPL call with
no arguments -- still has a layout: the one belonging to the default settings
file. Resolving it here is what keeps those docks reading the same override file
as a PATH-ed one, instead of silently dropping it because the key is not a
string."
  (let ((candidate (if (and (string? config-path)
                            (not (string-null? config-path)))
                       config-path
                       (get-config-path))))
    (and (string? candidate)
         (not (string-null? candidate))
         candidate)))

(define (%empty-layout)
  (list (cons 'order '())
        (cons 'removed '())
        (cons 'added '())
        (cons 'views '())))

(define (ensure-directory dir)
  (unless (or (string-null? dir) (file-exists? dir))
    (ensure-directory (dirname dir))
    (catch 'system-error (lambda () (mkdir dir)) (lambda _ #t))))

(define (dock-layout-read config-path)
  "Reads the override file for CONFIG-PATH, returning an alist with the four
sections. A missing, unreadable or malformed file simply means \"no overrides\"."
  (let* ((path (dock-layout-path config-path))
         (data (catch #t
                 (lambda ()
                   (and (file-exists? path) (call-with-input-file path read)))
                 (lambda (key . args)
                   (log-warn "Layout"
                             "Ignoring unreadable dock layout ~a (~a)"
                             path key)
                   #f))))
    (if (and (list? data) (every pair? data))
        (list (cons 'order (or (assq-ref data 'order) '()))
              (cons 'removed (or (assq-ref data 'removed) '()))
              (cons 'added (or (assq-ref data 'added) '()))
              (cons 'views (or (assq-ref data 'views) '())))
        (%empty-layout))))

(define (dock-layout-ref config-path)
  "Returns the parsed override layout of CONFIG-PATH, read fresh from disk.

Reading every time is what makes the answer always current: a reload, a second
dock on another monitor and a hand edit all see the same file."
  (let ((target (dock-layout-target config-path)))
    (if target (dock-layout-read target) (%empty-layout))))

(define (%write-layout! config-path layout)
  "Writes LAYOUT as the override layout of CONFIG-PATH.

Takes the layout as a value instead of re-reading it: a mutation already holds
the layout it just built, and re-reading would write the file it is replacing."
  (let ((target (dock-layout-target config-path)))
    (if (not target)
        #f
        (let ((path (dock-layout-path target)))
          (ensure-directory (dirname path))
          (catch #t
                 (lambda ()
                   (call-with-output-file path
                     (lambda (port)
                       (format port ";; lambdock dock layout for ~a~%" target)
                       (format port
                               ";; Written automatically by drag and drop in the dock, and by the~%")
                       (format port
                               ";; Preferences window's \"Add an item\" row.~%")
                       (format port
                               ";; Delete this file to go back to the items in your settings file.~%")
                       (write layout port)
                       (newline port)))
                   (log-info "Layout" "Saved dock layout overrides to ~a" path)
                   #t)
                 (lambda (err . args)
                   (log-error "Layout" "Could not write dock layout ~a (~a ~s)"
                              path err args)
                   #f))))))

(define (dock-layout-reset! config-path)
  "Forgets every override of CONFIG-PATH (used by tests and by the REPL)."
  (%write-layout! config-path (%empty-layout)))

;; =============================================================================
;; Applying overrides to a list of dock items
;; =============================================================================

(define (%preset-item preset-name)
  "The dock item a stored \"preset\" descriptor names, or #f.

The presets are the dock's ready-made widgets (trash, clock, ram, cpu,
cpu-temp, battery) and its launcher shortcuts; (lambdock presets) is the single
place that knows them, so this module does not keep a second list."
  (and (string? preset-name)
       (not (string-null? preset-name))
       (preset-launcher (string->symbol preset-name))))

(define (%descriptor-item-key desc)
  "The dock-item-key of the item DESC builds, without building it twice."
  (if (and (pair? desc) (equal? (car desc) "preset"))
      (let ((item (%preset-item (and (>= (length desc) 2) (cadr desc)))))
        (and item (dock-item-key item)))
      #f))

(define (%descriptor-key desc)
  "The stable key of a stored descriptor, which must match the key of the item
it rebuilds: that is what makes \"remove from dock\" and reordering work for an
added item.

Preset descriptors store the preset's name (\"ram\"), not the item's name
(\"RAM Usage\"), so their key is derived from the item they build."
  (or (%descriptor-item-key desc)
      (format #f "~a:~a" (list-ref desc 0) (list-ref desc 1))))

(define (%descriptor->item desc)
  "Rebuilds a dock item from a stored (kind name icon argument view) descriptor."
  (catch #t
         (lambda ()
           (if (or (not (list? desc)) (< (length desc) 5))
               #f
               (let ((kind (list-ref desc 0))
                     (name (list-ref desc 1))
                     (icon (list-ref desc 2))
                     (arg (list-ref desc 3))
                     (view (string->symbol (list-ref desc 4))))
                 (cond ((string=? kind "app")
                        (app-item #:name name #:icon icon #:exec arg))
                       ((string=? kind "stack")
                        (stack-item #:name name #:icon icon #:path arg
                                    #:view-mode view))
                       ((string=? kind "launchpad")
                        (launchpad-item #:name name #:icon icon))
                       ((string=? kind "preset")
                        (or (%preset-item name)
                            (begin
                              (log-warn "Layout"
                                        "Skipping unknown stored dock preset ~s"
                                        name)
                              #f)))
                       (else #f)))))
         (lambda (key . args)
           (log-warn "Layout" "Skipping invalid stored dock item ~s (~a)"
                     desc key)
           #f)))

(define (%apply-view item views)
  "Applies a stored per-item view mode (stacks only) to ITEM."
  (let ((stored (and (eq? (dock-item-type item) 'stack)
                     (assoc-ref views (dock-item-key item)))))
    (if (memq stored '(grid list))
        (stack-item #:name (dock-item-name item)
                    #:icon (dock-item-icon item)
                    #:path (dock-item-path item)
                    #:view-mode stored
                    #:hover-animate? (dock-item-hover-animate? item))
        item)))

(define (%rank-items base added order)
  "Pairs every item with the rank that decides its place in the dock.

BASE items are the configured ones: they take their stored index from ORDER, and
items that are not listed (for example separators, which have no stable identity,
or items the user never moved) inherit a rank just after their predecessor, so
they keep their place between the items that *are* listed.

ADDED items are the ones dragged onto the dock. They go after everything the
settings file configured, in the order they were added, unless ORDER places them
explicitly (which is what the running dock writes as soon as the drop lands)."
  (let ((positions (make-hash-table)))
    (let loop ((index 0) (keys order))
      (unless (null? keys)
        (hash-set! positions (car keys) index)
        (loop (+ index 1) (cdr keys))))

    (let base-loop ((rest base) (previous 0.0) (ranked '()))
      (if (null? rest)
          (let* ((base-ranked (reverse ranked))
                 (last-rank (if (null? base-ranked)
                                0.0
                                (apply max (map car base-ranked)))))
            (append base-ranked
                    (let add-loop ((items added)
                                   (rank (+ (ceiling last-rank) 1.0))
                                   (out '()))
                      (if (null? items)
                          (reverse out)
                          (add-loop (cdr items) (+ rank 1.0)
                                    (cons (cons rank (car items)) out))))))
          (let* ((item (car rest))
                 (stored (hash-ref positions (dock-item-key item) #f))
                 (rank (or stored (+ previous 0.5))))
            (base-loop (cdr rest) rank (cons (cons rank item) ranked)))))))

(define (%log-overrides target layout items)
  "Say what the stored layout does to ITEMS, and flag stale order keys.

A key that the settings file no longer defines cannot be shown: only the `added'
section carries the icon and command an item needs. That is the usual reason a
layout looks like it was not applied at all, so it is worth one warning instead
of silently ignoring it."
  (let* ((order (assq-ref layout 'order))
         (removed (assq-ref layout 'removed))
         (added (assq-ref layout 'added))
         (known (append (map dock-item-key items)
                        (filter-map (lambda (desc)
                                      (and (list? desc)
                                           (>= (length desc) 2)
                                           (%descriptor-key desc)))
                                    added)))
         (unknown (remove (lambda (key) (member key known)) order)))
    (when (or (pair? order) (pair? removed) (pair? added))
      (log-info "Layout"
                "Applied layout for ~a: ~a ordered, ~a removed, ~a added"
                target (length order) (length removed) (length added)))
    (when (pair? unknown)
      (log-warn "Layout"
                "~a stored order key(s) are not in this dock any more (~s); only `added' items can be restored. Drag them in again, or delete ~a to forget them."
                (length unknown) unknown (dock-layout-path target)))))

(define (apply-dock-overrides items config-path)
  "Returns ITEMS with the override layout stored for CONFIG-PATH applied.

Overrides never replace the configured items, they only reorder them, drop the
ones the user removed, append the ones the user added and patch stack view
modes. Items the override does not mention keep their configured relative
order, which is why the sort is stable.

A dock with no settings path of its own uses the default settings file's layout
(see dock-layout-target), so it behaves like a path-ed dock instead of quietly
losing its layout."
  (let ((target (dock-layout-target config-path)))
    (if (not target)
        items
        (let* ((layout (dock-layout-ref config-path))
               (order (assq-ref layout 'order))
               (removed (assq-ref layout 'removed))
               (views (assq-ref layout 'views))
               (base (filter (lambda (item)
                               (not (member (dock-item-key item) removed)))
                             items))
               (base (map (lambda (item) (%apply-view item views)) base))
               (existing (map dock-item-key base))
               (added (filter (lambda (item)
                                (not (member (dock-item-key item) existing)))
                              (map (lambda (item) (%apply-view item views))
                                   (filter-map %descriptor->item
                                               (assq-ref layout 'added)))))
               (ranked (%rank-items base added order)))
          (%log-overrides target layout items)
          (map cdr (stable-sort ranked
                                (lambda (a b) (< (car a) (car b)))))))))

;; =============================================================================
;; Mutation (called from the C side for drag and drop)
;; =============================================================================

;; Every mutation ends here: the caller built the new layout, this writes it.
(define (%store! config-path layout) (%write-layout! config-path layout))

(define (%with-sections layout order removed added views)
  (list (cons 'order order)
        (cons 'removed removed)
        (cons 'added added)
        (cons 'views views)))

(define (dock-layout-add! config-path kind name icon arg view)
  "Remembers an item the user dragged onto the dock."
  (let* ((layout (dock-layout-ref config-path))
         (desc (list (if (symbol? kind) (symbol->string kind) (or kind "app"))
                     (or name "")
                     (or icon "")
                     (or arg "")
                     (or view "grid")))
         (desc-key (%descriptor-key desc)))
    (%store! config-path
             (%with-sections layout
                             (assq-ref layout 'order)
                             (remove (lambda (key) (string=? key desc-key))
                                     (assq-ref layout 'removed))
                             ;; Appended, not consed: the stored list is also the
                             ;; order in which items were dragged in.
                             (append (remove (lambda (other)
                                               (string=? (%descriptor-key other)
                                                         desc-key))
                                             (assq-ref layout 'added))
                                     (list desc))
                             (assq-ref layout 'views)))
    (log-info "Layout" "Added ~a to the dock layout of ~a" desc-key
              config-path)))

(define (dock-layout-remove! config-path item-key)
  "Remembers that the user dragged ITEM-KEY out of the dock."
  (let* ((layout (dock-layout-ref config-path))
         (removed (assq-ref layout 'removed)))
    (%store! config-path
             (%with-sections layout
                             (remove (lambda (key) (string=? key item-key))
                                     (assq-ref layout 'order))
                             (if (member item-key removed)
                                 removed
                                 (cons item-key removed))
                             (remove (lambda (desc)
                                       (string=? (%descriptor-key desc)
                                                 item-key))
                                     (assq-ref layout 'added))
                             (assq-ref layout 'views)))
    (log-info "Layout" "Removed ~a from the dock layout of ~a" item-key
              config-path)))

(define (dock-layout-set-order! config-path keys)
  "Stores the left-to-right order of the dock items as the list of KEYS."
  (let* ((layout (dock-layout-ref config-path))
         (clean (filter (lambda (key)
                          (and (string? key) (not (string-null? key))))
                        (if (list? keys) keys '()))))
    (%store! config-path
             (%with-sections layout clean
                             (assq-ref layout 'removed)
                             (assq-ref layout 'added)
                             (assq-ref layout 'views)))))

(define (dock-layout-set-view! config-path item-key view)
  "Stores the stack view mode of ITEM-KEY ('grid or 'list)."
  (let* ((layout (dock-layout-ref config-path))
         (mode (if (memq view '(grid list)) view 'grid))
         (views (acons item-key
                       mode
                       (assoc-remove (assq-ref layout 'views) item-key))))
    (%store! config-path
             (%with-sections layout
                             (assq-ref layout 'order)
                             (assq-ref layout 'removed)
                             (assq-ref layout 'added)
                             views))))

(define (assoc-remove alist key)
  (filter (lambda (pair) (not (equal? (car pair) key))) alist))
