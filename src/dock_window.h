// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

#ifndef DOCK_WINDOW_H
#define DOCK_WINDOW_H

#include "lambdock.h"

/* =========================================================================
 * ONE DOCK'S WINDOW
 * =========================================================================
 * The surface and the motion: layer-shell setup, the slide and auto-hide
 * animations, the fisheye frame, the CSS mode classes and the teardown that
 * takes it all down again.
 *
 * Owns the LambdockState lifecycle (build -> present -> destroy). The widgets
 * it creates are handed back to the state as borrowed pointers.
 */

/**
 * setup_layer_shell_window:
 * @state: Pointer to global application state structure.
 *
 * Initializes Wayland Layer Shell protocols for the dock window surface,
 * configuring screen edge anchoring, layering level, and space reservation
 * rules.
 */
void setup_layer_shell_window(LambdockState *state);

/**
 * setup_dock_motion_controller:
 * @state: Pointer to global application state structure.
 *
 * Attaches a GTK 4 EventControllerMotion to track mouse enter/leave events,
 * triggering auto-hide timers and reveal animations.
 */
void setup_dock_motion_controller(LambdockState *state);

/**
 * reveal_dock:
 * @state: Pointer to global application state structure.
 *
 * Cancels pending auto-hide timers and triggers a smooth slide-in animation to
 * bring the dock into view.
 */
void reveal_dock(LambdockState *state);

/**
 * hide_dock:
 * @state: Pointer to global application state structure.
 *
 * Calculates off-screen negative margins and initiates a slide-out animation to
 * hide the dock surface.
 */
void hide_dock(LambdockState *state);

/**
 * schedule_initial_autohide:
 * @state: Pointer to global application state structure.
 *
 * Schedules an initial 1000ms delay timer upon application launch to hide the
 * dock if auto-hide mode is turned on.
 */
void schedule_initial_autohide(LambdockState *state);

/**
 * on_quit_action:
 * @action: Triggered GSimpleAction object.
 * @parameter: Action target variant parameter (unused).
 * @user_data: Pointer to running GtkApplication context.
 *
 * Callback signal handler to exit GLib's main event processing loop.
 */
void on_quit_action(GSimpleAction *action, GVariant *parameter,
                    gpointer user_data);

/**
 * on_right_click:
 * @gesture: GtkGestureClick object handling click input.
 * @n_press: Count of mouse presses.
 * @x: Relative X mouse coordinate of press.
 * @y: Relative Y mouse coordinate of press.
 * @user_data: Pointer to LambdockState context.
 *
 * Gesture handler displaying the background context popover menu on secondary
 * click.
 */
void on_right_click(GtkGestureClick *gesture, gint n_press, gdouble x,
                    gdouble y, gpointer user_data);

/**
 * on_popover_closed:
 * @popover: Closed GTK Popover widget.
 * @user_data: Pointer to LambdockState context.
 *
 * Decrements active popover counters when context menus close and resumes
 * auto-hide timers.
 */
void on_popover_closed(GtkPopover *popover, gpointer user_data);

/**
 * on_about_action:
 * @action: Triggered GSimpleAction object.
 * @parameter: Action target variant parameter (unused).
 * @user_data: Pointer to running GtkApplication context.
 *
 * Callback signal handler displaying the "About Lambdock" dialog window.
 */
void on_about_action(GSimpleAction *action, GVariant *parameter,
                     gpointer user_data);

/**
 * update_dock_dimensions:
 * Recalculates and updates size requests for overlay surface and background
 * capsule based on the active icon size, orientation, and screen edge.
 */
void update_dock_dimensions(LambdockState *state);

/**
 * dock_apply_hover_at:
 * @state: the dock.
 * @x: pointer x, in the dock window's coordinates.
 * @y: pointer y, in the dock window's coordinates.
 *
 * Applies the fish-eye magnification for that pointer position: a cached
 * geometry lookup and some arithmetic per item, plus one size request per item
 * whose layout step actually changed. Nothing else is resized, and the
 * magnification itself is painted.
 *
 * Public so the per-frame budget can be asserted in a test: the interesting
 * properties (the surface keeps its size, the row relayouts at most once per
 * frame, the geometry cache is reused) are measurements.
 */
void dock_apply_hover_at(LambdockState *state, double x, double y);

/**
 * dock_apply_motion_preference:
 * @state: Pointer to the dock state.
 *
 * Honours the dock's reduced-motion setting and the desktop's animation
 * setting on a running dock: a slide in flight is finished immediately, icons
 * return to their resting size and the fisheye is stopped.
 */
void dock_apply_motion_preference(LambdockState *state);

/**
 * dock_is_settling:
 * @state: Pointer to the dock state.
 *
 * Whether the dock is sliding, or has only just stopped. Crossings reported in
 * that moment are echoes of the dock's own movement rather than the pointer
 * moving, and auto-hide must not act on them: believing one hides the dock, which
 * puts the revealing strip back under a pointer that never moved, which reveal it
 * again -- the dock bounces at the screen edge.
 *
 * Returns: TRUE while crossings are echoes of the dock's movement.
 */
gboolean dock_is_settling(const LambdockState *state);

/**
 * dock_capsule_cross_dim:
 * Returns the thickness requested for the background capsule along the dock's
 * cross axis: the covered part of the icons, plus the reflection band, plus the
 * glass floor, plus the configured capsule padding.
 */
int dock_capsule_cross_dim(const LambdockState *state);

/**
 * dock_apply_exclusive_zone:
 * @state: Pointer to the dock state.
 *
 * Reserves desktop space for the dock, or nothing at all while it auto-hides.
 * Safe to call on a dock whose window is not (yet) a layer surface, where it
 * does nothing.
 */
void dock_apply_exclusive_zone(LambdockState *state);

/**
 * dock_cross_axis_dim:
 * Returns the cross-axis size requested for the overlay surface: fisheye
 * magnification headroom plus the reflection band and the glass floor.
 */
int dock_cross_axis_dim(const LambdockState *state);

/**
 * apply_dock_mode_classes:
 * Toggles the `taskbar` CSS class on the background capsule so themes can
 * style full-width taskbar mode distinctly from the floating dock.
 */
void apply_dock_mode_classes(LambdockState *state);

/**
 * refresh_dock_mode:
 * Re-applies layer-shell anchors, mode CSS classes and dimension requests after
 * a live configuration reload toggles dock <-> taskbar mode.
 */
void refresh_dock_mode(LambdockState *state);

/**
 * unregister_dock_state:
 * Unlinks an active LambdockState instance from the global state tracker list
 * in main.c.
 */
void unregister_dock_state(LambdockState *state);

/**
 * destroy_dock_instance:
 * @state: Pointer to LambdockState context.
 *
 * Unparents popovers and widgets, cleans up window trackers and dock items,
 * and destroys the GTK window safely without disposal warnings.
 */
void destroy_dock_instance(LambdockState *state);

/**
 * is_monitor_valid:
 * Checks if a given GdkMonitor pointer is currently present in the display's
 * active monitor list.
 */
gboolean is_monitor_valid(GdkMonitor *mon);

/**
 * lambdock_layer_shell_available:
 * Returns: TRUE when the current display is Wayland /and/ the compositor
 * implements the wlr-layer-shell protocol.
 *
 * Always use this instead of gtk_layer_is_supported(): the library asserts that
 * it is talking to a Wayland display, so asking it on X11 logs a critical (and a
 * run with G_DEBUG=fatal-warnings aborts there).
 */
gboolean lambdock_layer_shell_available(void);

#endif // DOCK_WINDOW_H
