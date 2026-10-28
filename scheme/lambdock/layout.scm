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
;; =============================================================================

(define-module (lambdock layout)
  #:declarative? #f
  #:use-module (ice-9 format)
  #:use-module (srfi srfi-1)
  #:use-module (lambdock items)
  #:use-module (lambdock logger)
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

;; Parsed layouts, keyed by config path, so the file is read once per dock.
(define *dock-layouts* (make-hash-table))

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
  "Returns (and caches) the parsed override layout of CONFIG-PATH."
  (let ((key (if (string? config-path) config-path 'default)))
    (or (hash-ref *dock-layouts* key #f)
        (let ((layout (dock-layout-read key)))
          (hash-set! *dock-layouts* key layout)
          layout))))

(define (dock-layout-save! config-path)
  "Writes the in-memory override layout of CONFIG-PATH back to disk."
  (let* ((key (if (string? config-path) config-path 'default))
         (path (dock-layout-path key))
         (layout (dock-layout-ref key)))
    (ensure-directory (dirname path))
    (catch #t
           (lambda ()
             (call-with-output-file path
               (lambda (port)
                 (format port ";; lambdock dock layout for ~a~%" key)
                 (format port
                         ";; Written automatically by drag and drop in the dock.~%")
                 (format port
                         ";; Delete this file to go back to the items in your settings file.~%")
                 (write layout port)
                 (newline port)))
             (log-info "Layout" "Saved dock layout overrides to ~a" path)
             #t)
           (lambda (err . args)
             (log-error "Layout" "Could not write dock layout ~a (~a ~s)"
                        path err args)
             #f))))

(define (dock-layout-reset! config-path)
  "Forgets every override of CONFIG-PATH (used by tests and by the REPL)."
  (let ((key (if (string? config-path) config-path 'default)))
    (hash-set! *dock-layouts* key (%empty-layout))
    (dock-layout-save! key)))

;; =============================================================================
;; Applying overrides to a list of dock items
;; =============================================================================

(define (%descriptor-key desc)
  (format #f "~a:~a" (list-ref desc 0) (list-ref desc 1)))

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
                       (else #f)))))
         (lambda (key . args)
           (log-warn "Layout" "Skipping invalid stored dock item ~s (~a)"
                     desc key)
           #f)))

(define (%apply-view item views)
  "Applies a stored per-item view mode (stacks only) to ITEM."
  (let ((stored (and (eq? (dock-item-type item) 'stack)
                     (assoc-ref views (dock-item-key item)))))
    (if (memq stored '(grid list fan))
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

(define (apply-dock-overrides items config-path)
  "Returns ITEMS with the override layout stored for CONFIG-PATH applied.

Overrides never replace the configured items, they only reorder them, drop the
ones the user removed, append the ones the user added and patch stack view
modes. Items the override does not mention keep their configured relative
order, which is why the sort is stable."
  (if (not (and (string? config-path) (not (string-null? config-path))))
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
        (map cdr (stable-sort ranked
                              (lambda (a b) (< (car a) (car b))))))))

;; =============================================================================
;; Mutation (called from the C side for drag and drop)
;; =============================================================================

(define (%store! config-path layout)
  (let ((key (if (string? config-path) config-path 'default)))
    (hash-set! *dock-layouts* key layout)
    (dock-layout-save! key)))

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
  "Stores the stack view mode of ITEM-KEY ('grid, 'list or 'fan)."
  (let* ((layout (dock-layout-ref config-path))
         (mode (if (memq view '(grid list fan)) view 'grid))
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
