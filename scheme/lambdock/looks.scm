;; lambdock -- hyper-hackable desktop dock for Wayland
;;
;; SPDX-License-Identifier: MIT
;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

(define-module (lambdock looks)
  #:declarative? #t
  #:use-module (lambdock logger)
  #:export (dock-looks
            look-aliases
            look-name
            preset-look
            look-settings
            look-setting
            look?))

;; =============================================================================
;; LOOKS: whole-dock recipes
;; =============================================================================
;;
;; A theme is the *material* of the dock -- the colour, the gradients, the
;; borders. A *look* is the material plus the numbers that give the dock its
;; shape and its manners: icon size, spacing, end padding, dock or taskbar mode,
;; window titles, whether the icons magnify under the pointer, and the whole
;; `dock-shelf-*` geometry. Every recipe below is a complete description of a
;; dock as it appears on a particular desktop, so one line in a settings file
;;
;;   (define dock-look 'osx-mavericks)
;;
;; is the whole look, while an explicit definition next to it still wins:
;;
;;   (define dock-look 'osx-mavericks)
;;   (define dock-icon-size 64)          ; the recipe, bigger icons
;;
;; The recipes are alists keyed by their plain setting names (`theme`,
;; `icon-size`, `shelf-margin`, ...); both spellings work in a settings file,
;; with or without the `dock-` prefix. The values are *all* written down, even
;; the ones that happen to be the defaults, so switching looks from the REPL
;; (`apply-look!`) never leaves a number behind from the previous one: a
;; geometry the new look does not want is reset to the flat capsule instead of
;; being inherited.
;;
;; The fisheye is part of the manners, not of the geometry: the OS X dock and
;; every dock descended from it magnify the icons under the pointer, and no
;; Windows taskbar ever did, so `hover-animate?` is on for the dock looks and off
;; for the five Windows bars and the plain one.
;;
;; The measurements come from the original docks, scaled to the icon sizes
;; lambdock ships with. Two numbers are worth remembering when reading them:
;; the OS X dock keeps its icons about 0.3 icon apart, and it leaves about
;; 0.25 icon of glass past the first and last icon.

;; =============================================================================
;; Lookup & Convenience Functions
;; =============================================================================

(define (look-key->symbol key)
  "Coerces KEY (a symbol or a string) to a symbol. Returns #f otherwise."
  (cond ((symbol? key) key)
        ((string? key) (string->symbol key))
        (else #f)))

(define (look-name look)
  "Canonical symbol naming LOOK, resolving aliases, or #f when there is no such look.
Accepts a symbol or a string, so both (look-name 'tahoe) and
(look-name \"macos-tahoe-dark\") work."
  (let ((key (look-key->symbol look)))
    (and key
         (let ((canonical (or (assq-ref look-aliases key) key)))
           (and (assq canonical dock-looks) canonical)))))

(define (preset-look look)
  "The recipe alist for LOOK (symbol or string; aliases allowed), or #f."
  (let ((name (look-name look)))
    (and name (assq-ref dock-looks name))))

(define (look-settings look)
  "Alias of preset-look, named after what it returns in a settings file."
  (preset-look look))

(define (look-setting look key)
  "One value out of a recipe: (look-setting 'osx-tahoe 'icon-size) => 52.
KEY may be written with or without the `dock-' prefix."
  (let ((settings (preset-look look)))
    (and settings
         (let* ((sym (look-key->symbol key))
                (str (and sym (symbol->string sym)))
                (bare (if (and str (string-prefix? "dock-" str))
                          (string->symbol (substring str 5))
                          sym)))
           (let ((pair (and bare (assq bare settings))))
             (and pair (cdr pair)))))))

(define (look? look)
  "Whether LOOK names a shipped recipe."
  (and (preset-look look) #t))

;; =============================================================================
;; Association List: Look Aliases
;; =============================================================================
;;
;; The short names first: `mavericks` instead of `osx-mavericks`. The theme
;; names are aliases too, so writing the theme you already know from
;; `dock-theme` gets you the dock that theme was designed for.

(define look-aliases
  '((mavericks . osx-mavericks)
    (mavericks-dark . osx-mavericks-dark)
    (snow-leopard . osx-snow-leopard)
    (snow-leopard-dark . osx-snow-leopard-dark)
    (tiger . osx-tiger)
    (tahoe . osx-tahoe)
    (tahoe-dark . osx-tahoe-dark)
    (big-sur . osx-big-sur)
    (ventura . osx-ventura)
    (macos-big-sur . osx-big-sur)
    (macos-ventura . osx-ventura)
    (macos-tiger . osx-tiger)
    (macos-tahoe . osx-tahoe)
    (macos-tahoe-dark . osx-tahoe-dark)
    (macos-mavericks . osx-mavericks)
    (macos-mavericks-dark . osx-mavericks-dark)
    (snow-leopard-light . osx-snow-leopard)
    (luna . windows-xp)
    (xp . windows-xp)
    (win-xp . windows-xp)
    (aero . windows-vista)
    (vista . windows-vista)
    (win-vista . windows-vista)
    (win-7 . windows-7)
    (win7 . windows-7)
    (seven . windows-7)
    (win-10 . windows-10)
    (win10 . windows-10)
    (win-11 . windows-11)
    (win11 . windows-11)
    (taskbar . plain-taskbar)))

;; =============================================================================
;; Association List: Looks
;; =============================================================================

(define dock-looks
  '(
    ;; ------------------------------------------------------------------------
    ;; Stock Lambdock
    ;; ------------------------------------------------------------------------
    ;; The classic dark glassmorphism capsule: 48px icons, the OS X rhythm
    ;; (4px of setting spacing plus the theme's own padding) and 12px of end
    ;; padding. Nothing touches the shelf, so the capsule is icon + 24px tall.
    (vanilla
     (theme . vanilla)
     (icon-size . 48)
     (item-spacing . 4)
     (capsule-padding . 12)
     (mode . dock)
     (show-window-titles? . #f)
     (hover-animate? . #t)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    ;; The frosted light sibling: same rhythm, light glass.
    (vanilla-light
     (theme . vanilla-light)
     (icon-size . 48)
     (item-spacing . 4)
     (capsule-padding . 12)
     (mode . dock)
     (show-window-titles? . #f)
     (hover-animate? . #t)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    (solarized-light
     (theme . solarized-light)
     (icon-size . 48)
     (item-spacing . 4)
     (capsule-padding . 12)
     (mode . dock)
     (show-window-titles? . #f)
     (hover-animate? . #t)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    (nature
     (theme . nature)
     (icon-size . 48)
     (item-spacing . 4)
     (capsule-padding . 12)
     (mode . dock)
     (show-window-titles? . #f)
     (hover-animate? . #t)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    ;; Ubuntu's launcher: a little airier than the default capsule, with a
    ;; tighter end padding (the Yaru slab is not much wider than its icons).
    (unity
     (theme . unity)
     (icon-size . 48)
     (item-spacing . 6)
     (capsule-padding . 10)
     (mode . dock)
     (show-window-titles? . #f)
     (hover-animate? . #t)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    ;; A plain full-width bar for people who want a taskbar and no period
    ;; dressing: 28px icons, 8px between items, 12px at the ends.
    (plain-taskbar
     (theme . vanilla-light)
     (icon-size . 28)
     (item-spacing . 8)
     (capsule-padding . 12)
     (mode . taskbar)
     (show-window-titles? . #f)
     (hover-animate? . #f)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    ;; ------------------------------------------------------------------------
    ;; Mac OS X / macOS
    ;; ------------------------------------------------------------------------
    ;; 10.4 Tiger: a flat Aqua strip the icons live *inside*. 48px icons, 14px
    ;; of glass above them and a 6px lip below make a 68px strip (about 1.4
    ;; icons, which is what Tiger's dock measured); no mirrors, no keystone --
    ;; both arrived with Leopard.
    (osx-tiger
     (theme . macos-tiger)
     (icon-size . 48)
     (item-spacing . 6)
     (capsule-padding . 10)
     (mode . dock)
     (show-window-titles? . #f)
     (hover-animate? . #t)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 14)
     (shelf-front . 6)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    ;; 10.5 Leopard / 10.6 Snow Leopard: the 3D glass shelf. The icons overhang
    ;; it by ~0.4 icon, the slab is ~0.6 icon thick in front of them, the
    ;; mirrors fill ~0.42 icon and the silhouette flares a full half-icon per
    ;; side towards the screen edge (a 26px keystone at 48px icons).
    (osx-snow-leopard
     (theme . snow-leopard)
     (icon-size . 48)
     (item-spacing . 8)
     (capsule-padding . 10)
     (mode . dock)
     (show-window-titles? . #f)
     (hover-animate? . #t)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . -20)
     (shelf-front . 28)
     (shelf-flare . 26)
     (reflection . 0.42)
     (reflection-intensity . 0.65)
     (reflection-blur . 0.06))

    (osx-snow-leopard-dark
     (theme . snow-leopard-dark)
     (icon-size . 48)
     (item-spacing . 8)
     (capsule-padding . 10)
     (mode . dock)
     (show-window-titles? . #f)
     (hover-animate? . #t)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . -20)
     (shelf-front . 28)
     (shelf-flare . 26)
     (reflection . 0.42)
     (reflection-intensity . 0.65)
     (reflection-blur . 0.06))

    ;; 10.9 Mavericks: the platinum slab, measured off a screenshot of the real
    ;; dock and scaled from its 49px icons to 48px -- one icon of slab in front
    ;; of the icons, icons overhanging by ~0.4 icon, a 0.30-icon mirror band, a
    ;; 24px keystone per side and ~0.25 icon of glass past the end icons.
    (osx-mavericks
     (theme . macos-mavericks)
     (icon-size . 48)
     (item-spacing . 4)
     (capsule-padding . 8)
     (mode . dock)
     (show-window-titles? . #f)
     (hover-animate? . #t)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . -18)
     (shelf-front . 24)
     (shelf-flare . 24)
     (reflection . 0.30)
     (reflection-intensity . 0.70)
     (reflection-blur . 0.03))

    (osx-mavericks-dark
     (theme . macos-mavericks-dark)
     (icon-size . 48)
     (item-spacing . 4)
     (capsule-padding . 8)
     (mode . dock)
     (show-window-titles? . #f)
     (hover-animate? . #t)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . -18)
     (shelf-front . 24)
     (shelf-flare . 24)
     (reflection . 0.30)
     (reflection-intensity . 0.70)
     (reflection-blur . 0.03))

    ;; 11 Big Sur: a dark frosted capsule, icons *in* the glass (no overhang, no
    ;; mirrors -- they left with Mavericks). The theme paints 12px of its own
    ;; padding at each end, which is already the ~0.25 icon the reference keeps,
    ;; so the recipe adds none: the visible inset is 12px, not 24.
    (osx-big-sur
     (theme . macos-big-sur)
     (icon-size . 48)
     (item-spacing . 6)
     (capsule-padding . 0)
     (mode . dock)
     (show-window-titles? . #f)
     (hover-animate? . #t)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    ;; 13 Ventura / 14 Sonoma: denser glass, tighter gaps, same idea. Same
    ;; arithmetic as Big Sur: the theme carries 14px of end padding, so the
    ;; recipe adds none.
    (osx-ventura
     (theme . macos-ventura)
     (icon-size . 48)
     (item-spacing . 4)
     (capsule-padding . 0)
     (mode . dock)
     (show-window-titles? . #f)
     (hover-animate? . #t)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    ;; 26 Tahoe (Liquid Glass): bigger, rounder, translucenter. Icons grow to
    ;; 52px, the gaps to 12px (~0.23 icon) and the end padding to 16px
    ;; (~0.3 icon); the glass is the theme's job and stays the plain capsule.
    (osx-tahoe
     (theme . macos-tahoe)
     (icon-size . 52)
     (item-spacing . 8)
     (capsule-padding . 16)
     (mode . dock)
     (show-window-titles? . #f)
     (hover-animate? . #t)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    (osx-tahoe-dark
     (theme . macos-tahoe-dark)
     (icon-size . 52)
     (item-spacing . 8)
     (capsule-padding . 16)
     (mode . dock)
     (show-window-titles? . #f)
     (hover-animate? . #t)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    ;; ------------------------------------------------------------------------
    ;; Windows taskbars
    ;; ------------------------------------------------------------------------
    ;; The five Windows looks are one family and share everything that can be
    ;; shared: every one is a `'taskbar` (a full-width bar, not a floating
    ;; capsule), every one draws 32px icons, and every one leaves the shelf at
    ;; its flat defaults -- no overhang, no front face, no keystone, no mirrors,
    ;; because a Windows taskbar is a bar and not a shelf. What changes between
    ;; them is the chrome of their era: labels are on where that Windows showed
    ;; them (XP and Vista) and off where it did not (7, 10 and 11), and the gaps
    ;; follow the buttons -- Luna's task buttons touch, Aero's glass tiles keep a
    ;; few pixels of air, and Windows 11 leaves the most room around a rounded
    ;; tile. `windows-xp` is the oldest and tightest, `windows-11` the airiest.
    ;;
    ;; XP Luna: 32px icons packed edge to edge (0px of setting spacing -- the
    ;; theme's own button border is the gap), 4px of end padding so Quick Launch
    ;; starts right next to the Start button, window titles on like the classic
    ;; taskbar buttons.
    (windows-xp
     (theme . windows-xp)
     (icon-size . 32)
     (item-spacing . 0)
     (capsule-padding . 4)
     (mode . taskbar)
     (show-window-titles? . #t)
     (hover-animate? . #f)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    ;; Vista Aero: the same task buttons, now glass -- 4px of setting spacing
    ;; between the tiles and 6px of end padding. Vista still labelled its
    ;; buttons, so the window titles stay on.
    (windows-vista
     (theme . windows-vista)
     (icon-size . 32)
     (item-spacing . 4)
     (capsule-padding . 6)
     (mode . taskbar)
     (show-window-titles? . #t)
     (hover-animate? . #f)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    ;; Windows 7 (Aero superbar): the taller bar of the glass era, so a little
    ;; more air than Vista -- 6px of spacing and 8px of end padding -- and no
    ;; labels, which is the Windows 7 default ("always combine, hide labels").
    (windows-7
     (theme . windows-7)
     (icon-size . 32)
     (item-spacing . 6)
     (capsule-padding . 8)
     (mode . taskbar)
     (show-window-titles? . #f)
     (hover-animate? . #f)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    ;; Windows 10: flat and tight again -- icon-only buttons, 4px of spacing,
    ;; 8px of end padding.
    (windows-10
     (theme . windows-10)
     (icon-size . 32)
     (item-spacing . 4)
     (capsule-padding . 8)
     (mode . taskbar)
     (show-window-titles? . #f)
     (hover-animate? . #f)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))

    ;; Windows 11: the airiest of the five, a rounded tile with room around it
    ;; -- 8px of spacing and 12px of end padding, icon-only.
    (windows-11
     (theme . windows-11)
     (icon-size . 32)
     (item-spacing . 8)
     (capsule-padding . 12)
     (mode . taskbar)
     (show-window-titles? . #f)
     (hover-animate? . #f)
     (shelf-tilt . 0)
     (shelf-perspective . 400)
     (shelf-margin . 24)
     (shelf-front . 0)
     (shelf-flare . 0)
     (reflection . 0.0)
     (reflection-intensity . 0.55)
     (reflection-blur . 0.05))))
